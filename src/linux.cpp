/* linux.cpp - Linux probe.
 *
 * Deliberately POSIX-only: /proc and /sys are read as plain files and the
 * only syscalls used are sysconf/opendir/stat. That keeps this translation
 * unit free of <linux/...> headers, so it compiles (and is syntax-checked)
 * on any POSIX host, and it never shells out to lspci/nvidia-smi/npu-smi.
 */
#include "devinfo_core.hpp"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace devinfo {
namespace {

std::string read_file(const std::string& p, size_t cap = 1 << 16) {
  std::string out;
  FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) return out;
  out.resize(cap);
  size_t n = std::fread(&out[0], 1, cap, f);
  std::fclose(f);
  out.resize(n);
  return out;
}

bool exists(const std::string& p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0;
}

std::vector<std::string> list_dir(const std::string& p) {
  std::vector<std::string> v;
  DIR* d = ::opendir(p.c_str());
  if (!d) return v;
  while (struct dirent* e = ::readdir(d)) {
    if (e->d_name[0] == '.') continue;
    v.push_back(e->d_name);
  }
  ::closedir(d);
  return v;
}

std::string one_line(const std::string& p) {
  std::string s = read_file(p, 512);
  size_t cut = s.find_first_of("\r\n");
  if (cut != std::string::npos) s.resize(cut);
  return s;
}

std::string trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
    s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

/* "32K" / "1024K" / "2M" -> bytes */
double parse_cache_size(const std::string& raw) {
  std::string s = trim(raw);
  if (s.empty()) return 0;
  double mult = 1;
  if (!s.empty() && (s.back() == 'K' || s.back() == 'k')) { mult = 1024.0; s.pop_back(); }
  else if (!s.empty() && (s.back() == 'M' || s.back() == 'm')) { mult = 1024.0 * 1024; s.pop_back(); }
  return std::atof(s.c_str()) * mult;
}


/* ---------------------------------------------------------- /proc/cpuinfo */
struct CpuInfo {
  std::string vendor, model;
  std::vector<std::string> features;
  double mhz = 0;
  unsigned sockets = 0;
  unsigned cores_per_socket = 0;
};

void parse_cpuinfo(CpuInfo& ci) {
  std::string txt = read_file("/proc/cpuinfo");
  if (txt.empty()) return;

  unsigned seen = 0;
  size_t pos = 0;
  while (pos < txt.size()) {
    size_t nl = txt.find('\n', pos);
    std::string line = txt.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = (nl == std::string::npos) ? txt.size() : nl + 1;
    if (line.empty()) continue;

    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = trim(line.substr(0, colon));
    std::string val = trim(line.substr(colon + 1));
    if (val.empty()) continue;

    if (key == "vendor_id" || key == "CPU implementer") {
      if (ci.vendor.empty()) ci.vendor = val;
    } else if (key == "model name" || key == "Processor" || key == "Hardware") {
      if (ci.model.empty()) ci.model = val;
    } else if (key == "flags" || key == "Features") {
      if (ci.features.empty()) {
        size_t i = 0;
        while (i < val.size()) {
          size_t sp = val.find(' ', i);
          std::string tok = val.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
          if (!tok.empty()) ci.features.push_back(tok);
          if (sp == std::string::npos) break;
          i = sp + 1;
        }
      }
    } else if (key == "cpu MHz") {
      if (ci.mhz == 0) ci.mhz = std::atof(val.c_str());
    } else if (key == "physical id") {
      seen |= 1;
    } else if (key == "cpu cores") {
      if (ci.cores_per_socket == 0) ci.cores_per_socket = (unsigned)std::atoi(val.c_str());
    }
  }
  if (seen) {
    /* sockets = distinct "physical id"; cheap proxy is max+1 */
    unsigned maxid = 0;
    size_t p = 0;
    while ((p = txt.find("physical id", p)) != std::string::npos) {
      size_t c = txt.find(':', p);
      if (c == std::string::npos) break;
      unsigned id = (unsigned)std::atoi(txt.c_str() + c + 1);
      if (id > maxid) maxid = id;
      p = c;
    }
    ci.sockets = maxid + 1;
  }
}

/* --------------------------------------------------------------- topology */
unsigned count_physical_cores() {
  /* Count distinct (package, core) pairs exposed by sysfs. */
  std::vector<std::string> cpus = list_dir("/sys/devices/system/cpu");
  std::vector<std::string> seen;
  unsigned fallback = 0;
  for (const auto& c : cpus) {
    if (c.rfind("cpu", 0) != 0 || c.size() < 4) continue;
    if (c.find_first_not_of("0123456789", 3) != std::string::npos) continue;  /* skip cpu0/cpufreq etc */
    ++fallback;
    std::string base = "/sys/devices/system/cpu/" + c + "/topology/";
    std::string pkg = one_line(base + "physical_package_id");
    std::string core = one_line(base + "core_id");
    if (core.empty()) continue;
    std::string key = pkg + "/" + core;
    bool dup = false;
    for (const auto& s : seen) if (s == key) dup = true;
    if (!dup) seen.push_back(key);
  }
  if (!seen.empty()) return (unsigned)seen.size();
  return fallback;
}

/* Expand a kernel cpulist ("0-3,8,10-11") into cpu indices. */
std::vector<unsigned> cpulist_expand(const std::string& s) {
  std::vector<unsigned> out;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(',', i);
    if (j == std::string::npos) j = s.size();
    std::string part = trim(s.substr(i, j - i));
    if (!part.empty()) {
      size_t dash = part.find('-');
      if (dash == std::string::npos) {
        out.push_back((unsigned)std::atoi(part.c_str()));
      } else {
        unsigned a = (unsigned)std::atoi(part.substr(0, dash).c_str());
        unsigned b = (unsigned)std::atoi(part.substr(dash + 1).c_str());
        for (unsigned k = a; k <= b && k - a < 4096; ++k) out.push_back(k);
      }
    }
    i = j + 1;
  }
  return out;
}

/* A cpulist holds threads; SMT siblings share one core. Count real cores. */
unsigned distinct_cores(const std::vector<unsigned>& cpus) {
  std::vector<std::string> seen;
  for (unsigned id : cpus) {
    std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(id) + "/topology/";
    std::string pkg = one_line(base + "physical_package_id");
    std::string core = one_line(base + "core_id");
    if (core.empty()) continue;
    std::string key = pkg + "/" + core;
    bool dup = false;
    for (const auto& s : seen) if (s == key) { dup = true; break; }
    if (!dup) seen.push_back(key);
  }
  return (unsigned)seen.size();
}

void read_caches(Cpu& c) {
  /* Walk every CPU and count each cache *instance* exactly once. Scaling one
   * CPU's cache size by a core count is wrong on hybrid parts, where P and E
   * cores ship different L1/L2 sizes, and on multi-socket boxes. Every
   * instance is a distinct symlink target, so realpath() is its identity. */
  unsigned nlogical = c.logical ? c.logical : 1;
  if (nlogical > 1024) nlogical = 1024;
  std::vector<std::string> seen;
  unsigned line = 0;

  for (unsigned cpu = 0; cpu < nlogical; ++cpu) {
    std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache/";
    for (const auto& i : list_dir(base)) {
      if (i.rfind("index", 0) != 0) continue;
      std::string dir = base + i;

      /* shared_cpu_list names the CPUs sharing one cache, so it identifies the
       * instance. These sysfs dirs are real directories, not symlinks, so
       * neither the path nor realpath() dedupes them. */
      std::string shared = one_line(dir + "/shared_cpu_list");
      if (shared.empty()) continue;
      /* index name + sharing set is a unique instance id, so level and type
       * only need reading once per instance rather than once per CPU. */
      std::string key = i + "/" + shared;
      bool dup = false;
      for (const auto& s : seen) if (s == key) { dup = true; break; }
      if (dup) continue;
      seen.push_back(key);

      std::string level = one_line(dir + "/level");
      std::string type = one_line(dir + "/type");

      double bytes = parse_cache_size(one_line(dir + "/size"));
      if (bytes <= 0) continue;
      if (level == "1" && type == "Instruction") c.l1i += (unsigned)bytes;
      else if (level == "1")                    c.l1d += (unsigned)bytes;
      else if (level == "2")                    c.l2 += (unsigned)bytes;
      else if (level == "3")                    c.l3 += (unsigned)bytes;
      if (line == 0 && level == "1")
        line = (unsigned)std::atol(one_line(dir + "/coherency_line_size").c_str());
    }
  }
  c.line = line;
}

/* ------------------------------------------------------------------- GPU */
struct PciDev {
  std::string path;   /* .../cardN/device/ */
  std::string card;   /* .../cardN/ */
  unsigned vendor = 0, device = 0;
  std::string driver;
};

/* Turn a PCI id into a human name from the distro's pci.ids database. Read
 * the file directly; still no subprocesses. Empty when unavailable. */
std::string pci_ids_name(unsigned vendor, unsigned device) {
  static const char* kPaths[] = {
      "/usr/share/hwdata/pci.ids",
      "/usr/share/misc/pci.ids",
      "/usr/share/pci.ids",
  };
  std::FILE* f = nullptr;
  for (const char* p : kPaths) {
    f = std::fopen(p, "rb");
    if (f) break;
  }
  if (!f) return {};

  char line[512];
  bool in_vendor = false;
  std::string out;
  while (std::fgets(line, sizeof line, f)) {
    if (line[0] == '#') continue;
    if (line[0] == '\t') {
      if (!in_vendor) continue;
      if (line[1] == '\t') continue;            /* subsystem, not a device */
      unsigned id = (unsigned)std::strtoul(line + 1, nullptr, 16);
      if (id != device) continue;
      const char* s = line + 5;
      while (*s == ' ' || *s == '\t') ++s;
      std::string n(s);
      while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
      out = n;
      break;
    }
    in_vendor = ((unsigned)std::strtoul(line, nullptr, 16) == vendor);
  }
  std::fclose(f);
  return out;
}

std::vector<PciDev> drm_cards() {
  std::vector<PciDev> out;
  for (const auto& e : list_dir("/sys/class/drm")) {
    if (e.rfind("card", 0) != 0) continue;
    std::string base = "/sys/class/drm/" + e + "/device/";
    std::string ven = one_line(base + "vendor");
    if (ven.empty()) continue;
    PciDev d;
    d.path = base;
    d.card = "/sys/class/drm/" + e + "/";
    d.vendor = (unsigned)std::strtoul(ven.c_str(), nullptr, 0);
    std::string dev = one_line(base + "device");
    d.device = (unsigned)std::strtoul(dev.c_str(), nullptr, 0);
    /* driver symlink name */
    std::string link = base + "driver";
    char buf[512];
    ssize_t n = ::readlink(link.c_str(), buf, sizeof buf - 1);
    if (n > 0) {
      buf[n] = 0;
      const char* slash = std::strrchr(buf, '/');
      d.driver = slash ? slash + 1 : buf;
    }
    bool dup = false;
    for (const auto& x : out) if (x.vendor == d.vendor && x.device == d.device) dup = true;
    if (!dup) out.push_back(d);
  }
  return out;
}

/* ---- NVIDIA via libcuda: the only way to get SM count without a context */
typedef int (*cuInit_t)(unsigned);
typedef int (*cuDeviceGetCount_t)(int*);
typedef int (*cuDeviceGet_t)(int*, int);
typedef int (*cuDeviceGetName_t)(char*, int, int);
typedef int (*cuDeviceGetAttribute_t)(int*, int, int);
typedef int (*cuDeviceTotalMem_t)(unsigned long long*, int);

enum {
  ATTR_CLOCK_RATE = 13,
  ATTR_MULTIPROCESSOR_COUNT = 16,
  ATTR_MEMORY_CLOCK_RATE = 36,
  ATTR_GLOBAL_MEMORY_BUS_WIDTH = 37,
  ATTR_CC_MAJOR = 75,
  ATTR_CC_MINOR = 76,
};

void probe_nvidia_cuda(Report& r) {
  void* h = dlopen("libcuda.so.1", RTLD_LAZY | RTLD_LOCAL);
  if (!h) h = dlopen("libcuda.so", RTLD_LAZY | RTLD_LOCAL);
  if (!h) return;

  auto init_ = (cuInit_t)dlsym(h, "cuInit");
  auto count_ = (cuDeviceGetCount_t)dlsym(h, "cuDeviceGetCount");
  auto get_ = (cuDeviceGet_t)dlsym(h, "cuDeviceGet");
  auto name_ = (cuDeviceGetName_t)dlsym(h, "cuDeviceGetName");
  auto attr_ = (cuDeviceGetAttribute_t)dlsym(h, "cuDeviceGetAttribute");
  auto mem_ = (cuDeviceTotalMem_t)dlsym(h, "cuDeviceTotalMem_v2");
  if (!mem_) mem_ = (cuDeviceTotalMem_t)dlsym(h, "cuDeviceTotalMem");
  if (!init_ || !count_ || !get_ || !name_ || !attr_) return;
  if (init_(0) != 0) return;

  int n = 0;
  if (count_(&n) != 0 || n <= 0) return;

  for (int i = 0; i < n; ++i) {
    int dev = 0;
    if (get_(&dev, i) != 0) continue;
    char name[256] = {0};
    if (name_(name, (int)sizeof name, dev) != 0) continue;

    Gpu g;
    g.vendor = "nvidia";
    g.name = name;
    g.core_unit = "SM";
    g.method = "libcuda cuDeviceGetAttribute";

    int v = 0;
    if (attr_(&v, ATTR_MULTIPROCESSOR_COUNT, dev) == 0 && v > 0)
      g.cores = Num{(double)v, true, false};
    if (attr_(&v, ATTR_CLOCK_RATE, dev) == 0 && v > 0)
      g.clock_ghz = Num{(double)v / 1e6, true, false};      /* kHz -> GHz */

    int major = 0, minor = 0;
    attr_(&major, ATTR_CC_MAJOR, dev);
    attr_(&minor, ATTR_CC_MINOR, dev);
    if (major) {
      char cc[16];
      std::snprintf(cc, sizeof cc, "sm_%d.%d", major, minor);
      g.arch = cc;
    }

    unsigned bus = 0, memclk = 0;
    if (attr_(&v, ATTR_GLOBAL_MEMORY_BUS_WIDTH, dev) == 0 && v > 0) bus = (unsigned)v;
    if (attr_(&v, ATTR_MEMORY_CLOCK_RATE, dev) == 0 && v > 0) memclk = (unsigned)v;
    if (bus && memclk) {
      double gbs = (double)bus / 8.0 * (double)memclk * 1000.0 * 2.0 / 1e9;
      g.mem_bw_gbs = Num{gbs, true, false};
    }

    if (mem_) {
      unsigned long long total = 0;
      if (mem_(&total, dev) == 0 && total > 0)
        g.vram_bytes = Num{(double)total, true, false};
    }
    r.gpus.push_back(std::move(g));
  }
  dlclose(h);
}

/* --------------------------------------------------------------- NPU */
void probe_ascend(Report& r) {
  unsigned n = 0;
  for (int i = 0; i < 64; ++i) {
    char p[64];
    std::snprintf(p, sizeof p, "/dev/davinci%d", i);
    if (!exists(p)) break;
    ++n;
  }
  if (n == 0) return;

  Npu npu;
  npu.vendor = "huawei";
  npu.name = "Ascend NPU";
  npu.arch = "Da Vinci";
  npu.cores = Num{(double)n, true, false};
  npu.method = "/dev/davinci* device nodes";
  r.npus.push_back(std::move(npu));
}

void probe_accel_nodes(Report& r) {
  std::vector<std::string> nodes = list_dir("/dev/accel");
  if (nodes.empty()) return;
  bool have_accel = false;
  for (const auto& n : nodes) {
    if (n.rfind("accel", 0) == 0) have_accel = true;
  }
  if (!have_accel) return;

  Npu npu;
  npu.vendor = "intel";
  npu.name = "Integrated NPU";
  npu.arch = "accel";
  npu.method = "/dev/accel/* device nodes";
  r.npus.push_back(std::move(npu));
}

}  /* anonymous namespace */

/* ================================================================ CPU */
void probe_cpu(Cpu& c, Report& r) {
  CpuInfo ci;
  parse_cpuinfo(ci);

  c.arch = r.arch;
  c.model = ci.model;
  c.features = ci.features;

  /* arm64 SoCs usually have no "model name"; the device tree does. */
  if (c.model.empty()) {
    c.model = trim(read_file("/sys/firmware/devicetree/base/model", 256));
    if (c.model.empty()) c.model = trim(read_file("/proc/device-tree/model", 256));
  }

  if (!ci.vendor.empty()) {
    if (ci.vendor.rfind("0x", 0) == 0) {
      unsigned impl = (unsigned)std::strtoul(ci.vendor.c_str(), nullptr, 0);
      switch (impl) {
        case 0x41: c.vendor = "ARM"; break;
        case 0x48: c.vendor = "HiSilicon"; break;
        case 0x51: c.vendor = "Qualcomm"; break;
        case 0x61: c.vendor = "Apple"; break;
        case 0x6d: c.vendor = "Microsoft"; break;
        case 0x46: c.vendor = "Fujitsu"; break;
        case 0x43: c.vendor = "Cavium"; break;
        case 0x50: c.vendor = "Ampere"; break;
        default:   c.vendor = ci.vendor; break;
      }
    } else {
      c.vendor = ci.vendor;
    }
  }
  if (c.vendor == "Apple" || c.model.rfind("Apple", 0) == 0) c.vendor = "Apple";

  long logical = sysconf(_SC_NPROCESSORS_ONLN);
  if (logical > 0) c.logical = (unsigned)logical;

  unsigned phys = count_physical_cores();
  if (phys == 0) {
    if (ci.cores_per_socket && ci.sockets) phys = ci.cores_per_socket * ci.sockets;
    else phys = c.logical;
  }
  c.physical = phys;

  /* Intel hybrid: the kernel publishes separate P/E cpu lists. */
  {
    /* These lists hold logical CPUs: a P core appears twice when SMT is on. */
    std::vector<unsigned> plist = cpulist_expand(one_line("/sys/devices/cpu_core/cpus"));
    std::vector<unsigned> elist = cpulist_expand(one_line("/sys/devices/cpu_atom/cpus"));
    unsigned p = plist.empty() ? 0 : distinct_cores(plist);
    unsigned e = elist.empty() ? 0 : distinct_cores(elist);
    if (p && e) {
      c.p_cores = p;
      c.e_cores = e;
      c.p_name = "P";
      c.e_name = "E";
    }
    /* Non-hybrid parts get no P/E split: inventing "48 P cores" on a
     * homogeneous EPYC would be a fiction. */
  }

  read_caches(c);
  if (c.line == 0) c.line = 64;

  {
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGESIZE);
    if (pages > 0 && psize > 0) c.mem_bytes = Num{(double)pages * (double)psize, true, false};
  }

  /* Current reported clock lives in cpufreq; the max is what we want. */
  {
    std::string khz = one_line("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    double v = std::atof(khz.c_str());
    if (v > 0) c.clock_ghz = Num{v / 1e6, true, false};
    else if (ci.mhz > 0) c.clock_ghz = Num{ci.mhz / 1000.0, true, false};
  }
}

/* ================================================================ GPU */
void probe_gpu(Report& r) {
  probe_nvidia_cuda(r);

  for (const auto& d : drm_cards()) {
    if (d.vendor == 0x10de) continue;  /* NVIDIA already covered above */

    Gpu g;
    if (d.vendor == 0x1002) { g.vendor = "amd"; g.core_unit = "CU"; }
    else if (d.vendor == 0x8086) { g.vendor = "intel"; g.core_unit = "EU"; }
    else continue;

    char id[64];
    std::snprintf(id, sizeof id, "%04x:%04x", d.vendor, d.device);
    std::string pci = pci_ids_name(d.vendor, d.device);
    g.name = pci.empty() ? (g.vendor + std::string(" GPU [") + id + "]")
                         : (pci + " [" + id + "]");
    if (!d.driver.empty()) g.name += " (" + d.driver + ")";
    g.method = "sysfs /sys/class/drm";

    /* amdgpu exposes real VRAM numbers */
    std::string vram = one_line(d.path + "mem_info_vram_total");
    if (!vram.empty()) {
      double b = std::atof(vram.c_str());
      if (b > 0) g.vram_bytes = Num{b, true, false};
    }

    /* i915 publishes the GT max frequency at the card level */
    std::string gt = one_line(d.card + "gt_max_freq_mhz");
    if (!gt.empty()) {
      double mhz = std::atof(gt.c_str());
      if (mhz > 0) g.clock_ghz = Num{mhz / 1000.0, true, false};
    }
    r.gpus.push_back(std::move(g));
  }
}

/* ================================================================ NPU */
void probe_npu(Report& r) {
  probe_ascend(r);
  probe_accel_nodes(r);
}

}  /* namespace devinfo */
