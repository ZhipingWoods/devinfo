/* apple.cpp - macOS probe.
 *
 * Everything here is a syscall or an IOKit registry walk. No subprocesses,
 * no Metal context creation, nothing that costs more than a few milliseconds.
 */
#include "devinfo_core.hpp"

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/sysctl.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace devinfo {
namespace {

bool sys_str(const char* name, std::string& out) {
  size_t sz = 0;
  if (sysctlbyname(name, nullptr, &sz, nullptr, 0) != 0 || sz == 0) return false;
  std::string buf(sz, '\0');
  if (sysctlbyname(name, &buf[0], &sz, nullptr, 0) != 0) return false;
  size_t end = buf.find('\0');
  if (end != std::string::npos) buf.resize(end);
  out = buf;
  return !out.empty();
}

bool sys_u64(const char* name, uint64_t& v) {
  size_t sz = sizeof(v);
  v = 0;
  return sysctlbyname(name, &v, &sz, nullptr, 0) == 0;
}

bool sys_i32(const char* name, int& v) {
  size_t sz = sizeof(v);
  v = 0;
  return sysctlbyname(name, &v, &sz, nullptr, 0) == 0;
}

/* ------------------------------------------------------------- IOKit */
bool cf_str(CFTypeRef v, std::string& out) {
  if (!v || CFGetTypeID(v) != CFStringGetTypeID()) return false;
  char buf[512];
  if (!CFStringGetCString((CFStringRef)v, buf, (CFIndex)sizeof buf, kCFStringEncodingUTF8)) return false;
  out = buf;
  return true;
}

bool cf_num(CFTypeRef v, long long& out) {
  if (!v || CFGetTypeID(v) != CFNumberGetTypeID()) return false;
  return CFNumberGetValue((CFNumberRef)v, kCFNumberLongLongType, &out);
}

CFMutableDictionaryRef entry_props(io_registry_entry_t e) {
  CFMutableDictionaryRef d = nullptr;
  if (IORegistryEntryCreateCFProperties(e, &d, kCFAllocatorDefault, 0) != KERN_SUCCESS) return nullptr;
  return d;
}

bool class_present(const char* cls) {
  io_service_t s = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching(cls));
  if (!s) return false;
  IOObjectRelease(s);
  return true;
}

/* ----------------------------------------------------------- features */
struct FeatPair { const char* label; const char* key; };

void collect_features(Cpu& c) {
  static const FeatPair kFeat[] = {
    {"NEON",        "hw.optional.neon"},
    {"FP16",        "hw.optional.arm.FEAT_FP16"},
    {"AES",         "hw.optional.arm.FEAT_AES"},
    {"PMULL",       "hw.optional.arm.FEAT_PMULL"},
    {"SHA1",        "hw.optional.arm.FEAT_SHA1"},
    {"SHA256",      "hw.optional.arm.FEAT_SHA256"},
    {"SHA512",      "hw.optional.arm.FEAT_SHA512"},
    {"SHA3",        "hw.optional.arm.FEAT_SHA3"},
    {"CRC32",       "hw.optional.arm.FEAT_CRC32"},
    {"DotProd",     "hw.optional.arm.FEAT_DotProd"},
    {"I8MM",        "hw.optional.arm.FEAT_I8MM"},
    {"BF16",        "hw.optional.arm.FEAT_BF16"},
    {"EBF16",       "hw.optional.arm.FEAT_EBF16"},
    {"FHM",         "hw.optional.arm.FEAT_FHM"},
    {"FCMA",        "hw.optional.arm.FEAT_FCMA"},
    {"JSCVT",       "hw.optional.arm.FEAT_JSCVT"},
    {"RDM",         "hw.optional.arm.FEAT_RDM"},
    {"LSE",         "hw.optional.arm.FEAT_LSE"},
    {"LSE2",        "hw.optional.arm.FEAT_LSE2"},
    {"LRCPC",       "hw.optional.arm.FEAT_LRCPC"},
    {"LRCPC2",      "hw.optional.arm.FEAT_LRCPC2"},
    {"SVE",         "hw.optional.arm.FEAT_SVE"},
    {"SVE2",        "hw.optional.arm.FEAT_SVE2"},
    {"SME",         "hw.optional.arm.FEAT_SME"},
    {"SME2",        "hw.optional.arm.FEAT_SME2"},
    {"PAuth",       "hw.optional.arm.FEAT_PAuth"},
    {"SSBS",        "hw.optional.arm.FEAT_SSBS"},
    {"SB",          "hw.optional.arm.FEAT_SB"},
    {"HBC",         "hw.optional.arm.FEAT_HBC"},
    {"CSSC",        "hw.optional.arm.FEAT_CSSC"},
    {"AFP",         "hw.optional.arm.FEAT_AFP"},
    {"CSV2",        "hw.optional.arm.FEAT_CSV2"},
    {"CSV3",        "hw.optional.arm.FEAT_CSV3"},
    {nullptr, nullptr},
  };
  for (int i = 0; kFeat[i].label; ++i) {
    int v = 0;
    if (sys_i32(kFeat[i].key, v) && v) c.features.push_back(kFeat[i].label);
  }
}

}  /* anonymous namespace */

/* ================================================================ CPU */
void probe_cpu(Cpu& c, Report& r) {
  sys_str("machdep.cpu.brand_string", c.model);
  sys_str("hw.machine", c.arch);
  if (c.arch.empty()) c.arch = "arm64";

  if (c.model.rfind("Apple", 0) == 0 || c.arch == "arm64") c.vendor = "Apple";
  else if (c.model.find("Intel") != std::string::npos) c.vendor = "Intel";
  else if (c.model.find("AMD") != std::string::npos) c.vendor = "AMD";

  int n = 0;
  if (sys_i32("hw.physicalcpu", n)) c.physical = (unsigned)n;
  if (sys_i32("hw.logicalcpu", n)) c.logical = (unsigned)n;

  /* Apple Silicon exposes performance tiers instead of a uniform core set. */
  int levels = 0;
  sys_i32("hw.nperflevels", levels);
  uint64_t l1i = 0, l1d = 0, l2 = 0;
  for (int i = 0; i < levels; ++i) {
    char key[96];
    std::snprintf(key, sizeof key, "hw.perflevel%d.physicalcpu", i);
    int cores = 0;
    sys_i32(key, cores);

    std::snprintf(key, sizeof key, "hw.perflevel%d.name", i);
    std::string lname;
    sys_str(key, lname);

    uint64_t v = 0;
    std::snprintf(key, sizeof key, "hw.perflevel%d.l1icachesize", i);
    if (sys_u64(key, v)) l1i += v * (uint64_t)(cores ? cores : 1);
    std::snprintf(key, sizeof key, "hw.perflevel%d.l1dcachesize", i);
    if (sys_u64(key, v)) l1d += v * (uint64_t)(cores ? cores : 1);
    std::snprintf(key, sizeof key, "hw.perflevel%d.l2cachesize", i);
    if (sys_u64(key, v)) l2 += v;

    if (i == 0) { c.p_cores = (unsigned)cores; c.p_name = lname; }
    else if (i == 1) { c.e_cores = (unsigned)cores; c.e_name = lname; }
  }
  if (levels == 0) {
    /* Intel Macs: single tier, but there may still be SMT. */
    c.p_cores = c.physical;
  }
  c.l1i = (unsigned)l1i;
  c.l1d = (unsigned)l1d;
  c.l2 = (unsigned)l2;

  uint64_t v = 0;
  if (sys_u64("hw.cachelinesize", v)) c.line = (unsigned)v;
  if (sys_u64("hw.l3cachesize", v)) c.l3 = (unsigned)v;
  if (sys_u64("hw.memsize", v)) c.mem_bytes = Num{(double)v, true, false};

  /* On Apple Silicon there is no CPU-clock sysctl at all: leaving this
   * unset is correct, the table fills it in and marks it estimated. */
  if (sys_u64("hw.cpufrequency", v) && v > 0) c.clock_ghz = Num{(double)v / 1e9, true, false};

  collect_features(c);

  /* No SMT on Apple Silicon; report logical==physical there. */
  if (c.logical == 0) c.logical = c.physical;

  std::string model_id;
  if (sys_str("hw.model", model_id)) r.model_id = model_id;
}

/* ================================================================ GPU */
void probe_gpu(Report& r) {
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(MACH_PORT_NULL, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS)
    return;

  std::vector<std::string> seen;
  io_service_t svc = 0;
  while ((svc = IOIteratorNext(it)) != 0) {
    CFMutableDictionaryRef d = entry_props(svc);
    IOObjectRelease(svc);
    if (!d) continue;

    Gpu g;
    std::string model, plugin;
    long long cores = 0, vram_mb = 0;

    if (cf_str(CFDictionaryGetValue(d, CFSTR("model")), model)) g.name = model;
    cf_str(CFDictionaryGetValue(d, CFSTR("MetalPluginName")), plugin);
    cf_num(CFDictionaryGetValue(d, CFSTR("gpu-core-count")), cores);
    cf_num(CFDictionaryGetValue(d, CFSTR("VRAM,totalMB")), vram_mb);
    CFRelease(d);

    if (g.name.empty()) continue;
    bool dup = false;
    for (const auto& s : seen) if (s == g.name) dup = true;
    if (dup) continue;
    seen.push_back(g.name);

    if (g.name.rfind("Apple", 0) == 0) {
      g.vendor = "apple";
      g.core_unit = "GPU core";
      g.unified = true;
      if (!plugin.empty()) g.arch = plugin;    /* e.g. AGXMetalG17G */
      if (r.cpu.mem_bytes.has) g.vram_bytes = Num{r.cpu.mem_bytes.v, true, false};
      g.mem_bw_gbs = r.cpu.mem_bw_gbs;   /* unified memory: same physical DRAM */
      g.method = "IOKit IOAccelerator + Apple chip table";
    } else if (g.name.find("NVIDIA") != std::string::npos || g.name.find("GeForce") != std::string::npos) {
      g.vendor = "nvidia";
    } else if (g.name.find("AMD") != std::string::npos || g.name.find("Radeon") != std::string::npos) {
      g.vendor = "amd";
      g.core_unit = "CU";
    } else {
      g.vendor = "intel";
      g.core_unit = "EU";
    }

    if (cores > 0) g.cores = Num{(double)cores, true, false};
    if (vram_mb > 0) {
      g.vram_bytes = Num{(double)vram_mb * 1024.0 * 1024.0, true, false};
      g.unified = false;
    }
    r.gpus.push_back(std::move(g));
  }
  IOObjectRelease(it);
}

/* ================================================================ NPU */
void probe_npu(Report& r) {
  /* The ANE driver class name is versioned by SoC generation, so probe a
   * family of names rather than hard-coding one. */
  static const char* kAne[] = {
    "H11ANEIn", "H12ANEIn", "H13ANEIn", "H14ANEIn",
    "H15ANEIn", "H16ANEIn", "H17ANEIn", "AppleARMANE", nullptr,
  };
  bool present = false;
  for (int i = 0; kAne[i]; ++i) {
    if (class_present(kAne[i])) { present = true; break; }
  }
  if (!present) {
    /* Fall back to scanning AppleARMIODevice children named "ane". */
    io_service_t dev = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("AppleARMIODevice"));
    if (dev) {
      io_iterator_t it = 0;
      if (IORegistryEntryGetChildIterator(dev, kIOServicePlane, &it) == KERN_SUCCESS) {
        io_registry_entry_t ch = 0;
        while ((ch = IOIteratorNext(it)) != 0) {
          io_name_t name{};
          if (IORegistryEntryGetName(ch, name) == KERN_SUCCESS && std::strcmp(name, "ane") == 0) present = true;
          IOObjectRelease(ch);
          if (present) break;
        }
        IOObjectRelease(it);
      }
      IOObjectRelease(dev);
    }
  }
  if (!present) return;

  const AppleChip* ch = apple_chip(r.cpu.model);
  Npu n;
  n.vendor = "apple";
  n.name = "Apple Neural Engine";
  n.arch = "ANE";
  n.method = "IOKit ANE class + Apple chip table";
  if (ch) {
    n.cores = Num{(double)ch->ane_cores, true, true};
    n.int8_tops = Num{ch->ane_tops, true, true};
  }
  r.npus.push_back(std::move(n));
}

}  /* namespace devinfo */
