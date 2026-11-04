/* win32.cpp - Windows probe.
 *
 * CPUID + Kernel32 topology + registry for the CPU, DXGI for displays-class
 * adapters, NVML for NVIDIA detail, SetupAPI for NPUs. No WMI (slow), no
 * shelling out, no device-context creation.
 */
#include "devinfo_core.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
/* IDXGIFactory1/IDXGIAdapter1 live in dxgi.h; there is no dxgi1_1.h in
 * either the Windows SDK or mingw, so naming one breaks every toolchain. */
#include <dxgi.h>
#include <setupapi.h>

#include <devguid.h>
#include <powerbase.h>   /* CallNtPowerInformation */

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if defined(_M_IX86) || defined(_M_X64)
#include <intrin.h>
#define DEVINFO_X86 1
#endif

namespace devinfo {
namespace {

/* ------------------------------------------------------------- helpers */
std::string wide_to_utf8(const wchar_t* w) {
  if (!w || !*w) return std::string();
  int need = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (need <= 1) return std::string();
  std::string out((size_t)need - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], need, nullptr, nullptr);
  return out;
}

std::string reg_string(const char* subkey, const char* name) {
  char buf[512] = {0};
  DWORD sz = (DWORD)sizeof buf;
  if (RegGetValueA(HKEY_LOCAL_MACHINE, subkey, name, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
    return std::string();
  return buf;
}

DWORD reg_dword(const char* subkey, const char* name) {
  DWORD v = 0, sz = (DWORD)sizeof v;
  if (RegGetValueA(HKEY_LOCAL_MACHINE, subkey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
    return 0;
  return v;
}

double parse_ghz(const std::string& s) {
  size_t at = s.find('@');
  if (at == std::string::npos) return 0.0;
  std::string tail = s.substr(at + 1);
  double v = std::atof(tail.c_str());
  if (tail.find("MHz") != std::string::npos || tail.find("mhz") != std::string::npos) v /= 1000.0;
  return v;
}

/* ---------------------------------------------------------------- CPU */
#if defined(DEVINFO_X86)
void cpuid(int out[4], int leaf, int sub) { __cpuidex(out, leaf, sub); }

std::string cpu_vendor_string() {
  int r[4] = {0};
  cpuid(r, 0, 0);
  char s[13] = {0};
  std::memcpy(s + 0, &r[1], 4);   /* EBX */
  std::memcpy(s + 4, &r[3], 4);   /* EDX */
  std::memcpy(s + 8, &r[2], 4);   /* ECX */
  return s;
}

std::string cpu_brand_string() {
  int r[4] = {0};
  char buf[49] = {0};
  cpuid(r, 0x80000000, 0);
  unsigned maxext = (unsigned)r[0];
  if (maxext < 0x80000004u) return std::string();
  for (int i = 0; i < 3; ++i) {
    cpuid(r, 0x80000002 + i, 0);
    std::memcpy(buf + i * 16 + 0,  &r[0], 4);
    std::memcpy(buf + i * 16 + 4,  &r[1], 4);
    std::memcpy(buf + i * 16 + 8,  &r[2], 4);
    std::memcpy(buf + i * 16 + 12, &r[3], 4);
  }
  std::string s = buf;
  /* collapse the leading spaces Intel pads the brand string with */
  size_t b = s.find_first_not_of(' ');
  return b == std::string::npos ? s : s.substr(b);
}

/* Linux hands us a ready-made flag list from /proc/cpuinfo; Windows does not,
 * so decode CPUID here. Every vector extension is gated on its parent: a CPU
 * that lacks AVX512F cannot be claimed to have AVX512_VNNI no matter what a
 * stray bit says, which keeps a mistyped bit from inventing hardware. */
void cpu_features(Cpu& c) {
  auto bit = [](const int r[4], int reg, int n) { return (r[reg] >> n) & 1; };

  int lv[4] = {0};
  cpuid(lv, 0, 0);
  const unsigned max_leaf = (unsigned)lv[0];

  int r1[4] = {0};
  cpuid(r1, 1, 0);
  const bool sse2  = bit(r1, 3, 26);
  const bool sse41 = bit(r1, 2, 19);
  const bool avx   = bit(r1, 2, 28);
  const bool fma   = bit(r1, 2, 12);
  const bool popcnt = bit(r1, 2, 23);

  int r7[4] = {0}, r71[4] = {0};
  bool have7 = max_leaf >= 7;
  bool have71 = false;
  if (have7) {
    cpuid(r7, 7, 0);
    have71 = ((unsigned)r7[0]) >= 1;   /* EAX holds the highest subleaf */
    if (have71) cpuid(r71, 7, 1);
  }

  const bool avx2      = have7 && bit(r7, 1, 5);
  const bool avx512f   = have7 && bit(r7, 1, 16);

  if (sse2)   c.features.push_back("SSE2");
  if (sse41)  c.features.push_back("SSE4.1");
  if (popcnt) c.features.push_back("POPCNT");
  if (avx)    c.features.push_back("AVX");
  if (fma)    c.features.push_back("FMA");
  if (avx2)   c.features.push_back("AVX2");

  if (have7) {
    /* leaf 7 EBX */
    if (bit(r7, 1, 3))  c.features.push_back("BMI1");
    if (bit(r7, 1, 8))  c.features.push_back("BMI2");
    if (bit(r7, 1, 29)) c.features.push_back("SHA");
    /* leaf 7 ECX - need no AVX512 */
    if (bit(r7, 2, 8))  c.features.push_back("GFNI");
    if (bit(r7, 2, 9))  c.features.push_back("VAES");
    if (bit(r7, 2, 10)) c.features.push_back("VPCLMULQDQ");
  }

  if (avx512f) {
    c.features.push_back("AVX512F");
    if (bit(r7, 1, 17)) c.features.push_back("AVX512DQ");
    if (bit(r7, 1, 21)) c.features.push_back("AVX512IFMA");
    if (bit(r7, 1, 28)) c.features.push_back("AVX512CD");
    if (bit(r7, 1, 30)) c.features.push_back("AVX512BW");
    if (bit(r7, 1, 31)) c.features.push_back("AVX512VL");
    if (bit(r7, 2, 1))  c.features.push_back("AVX512_VBMI");
    if (bit(r7, 2, 6))  c.features.push_back("AVX512_VBMI2");
    if (bit(r7, 2, 11)) c.features.push_back("AVX512_VNNI");
    if (bit(r7, 2, 12)) c.features.push_back("AVX512_BITALG");
    if (bit(r7, 2, 14)) c.features.push_back("AVX512_VPOPCNTDQ");
  }

  if (have71) {
    /* 256-bit VNNI and the BF16/FP16 AVX512 forms live in subleaf 1 */
    if (bit(r71, 0, 4))  c.features.push_back("AVX_VNNI");
    if (avx512f && bit(r71, 0, 5))  c.features.push_back("AVX512_BF16");
    if (avx512f && bit(r71, 0, 21)) c.features.push_back("AVX_IFMA");
    if (avx512f && bit(r71, 3, 23)) c.features.push_back("AVX512_FP16");
  }

  /* AMX is independent of AVX-512: it feeds a separate matrix unit. */
  if (have7 && bit(r7, 3, 23)) {
    c.features.push_back("AMX_TILE");
    if (bit(r7, 3, 24)) c.features.push_back("AMX_INT8");
    if (bit(r7, 3, 22)) c.features.push_back("AMX_BF16");
  }
}
#endif

void read_caches(Cpu& c) {
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
  if (len == 0) return;
  std::vector<unsigned char> buf(len);
  if (!GetLogicalProcessorInformationEx(RelationCache,
                                        (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len))
    return;

  DWORD off = 0;
  while (off < len) {
    auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
    if (e->Relationship == RelationCache) {
      const CACHE_RELATIONSHIP& ci = e->Cache;
      unsigned long long bytes = ci.CacheSize;
      if (ci.Level == 1) {
        if (ci.Type == CacheInstruction) c.l1i += (unsigned)bytes;
        else                             c.l1d += (unsigned)bytes;
      } else if (ci.Level == 2) {
        c.l2 += (unsigned)bytes;
      } else if (ci.Level == 3) {
        c.l3 += (unsigned)bytes;
      }
      if (ci.LineSize) c.line = ci.LineSize;
    }
    if (e->Size == 0) break;
    off += e->Size;
  }
}

unsigned physical_cores() {
  DWORD len = 0;
  GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
  if (len == 0) return 0;
  std::vector<unsigned char> buf(len);
  if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
                                        (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf.data(), &len))
    return 0;
  unsigned n = 0;
  DWORD off = 0;
  while (off < len) {
    auto* e = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf.data() + off);
    if (e->Relationship == RelationProcessorCore) ++n;
    if (e->Size == 0) break;
    off += e->Size;
  }
  return n;
}

/* ---------------------------------------------------------------- GPU */
struct NvmlApi {
  void* lib = nullptr;
  int (*init)(void);
  int (*count)(unsigned*);
  int (*handle)(unsigned*, unsigned);
  int (*name)(unsigned*, char*, unsigned);
  int (*num_cores)(unsigned*, unsigned*);
  int (*clock)(unsigned*, int, unsigned*);
  int (*meminfo)(unsigned*, void*);
  int (*cc)(unsigned*, int*, int*);
  int (*pcie_gen)(unsigned*, unsigned*);
  int (*pcie_gen_max)(unsigned*, unsigned*);
  int (*pcie_width)(unsigned*, unsigned*);
  int (*pcie_width_max)(unsigned*, unsigned*);
  bool ok() const { return lib && init && count && handle; }
};

struct NvmlMem {
  unsigned long long total, free, used;
};

NvmlApi load_nvml() {
  NvmlApi a{};
  a.lib = (void*)LoadLibraryA("nvml.dll");
  if (!a.lib) return a;
  HMODULE m = (HMODULE)a.lib;
  a.init   = (decltype(a.init))GetProcAddress(m, "nvmlInit_v2");
  a.count  = (decltype(a.count))GetProcAddress(m, "nvmlDeviceGetCount_v2");
  a.handle = (decltype(a.handle))GetProcAddress(m, "nvmlDeviceGetHandleByIndex_v2");
  a.name   = (decltype(a.name))GetProcAddress(m, "nvmlDeviceGetName");
  a.num_cores = (decltype(a.num_cores))GetProcAddress(m, "nvmlDeviceGetNumGpuCores");
  a.clock  = (decltype(a.clock))GetProcAddress(m, "nvmlDeviceGetClockInfo");
  a.meminfo = (decltype(a.meminfo))GetProcAddress(m, "nvmlDeviceGetMemoryInfo");
  a.cc     = (decltype(a.cc))GetProcAddress(m, "nvmlDeviceGetCudaComputeCapability");
  a.pcie_gen       = (decltype(a.pcie_gen))GetProcAddress(m, "nvmlDeviceGetCurrPcieLinkGeneration");
  a.pcie_gen_max   = (decltype(a.pcie_gen_max))GetProcAddress(m, "nvmlDeviceGetMaxPcieLinkGeneration");
  a.pcie_width     = (decltype(a.pcie_width))GetProcAddress(m, "nvmlDeviceGetCurrPcieLinkWidth");
  a.pcie_width_max = (decltype(a.pcie_width_max))GetProcAddress(m, "nvmlDeviceGetMaxPcieLinkWidth");
  return a;
}

/* ToDesk, GameViewer, Oray, Parsec and friends ship indirect display drivers.
 * Those are real WDDM adapters, not DXGI_ADAPTER_FLAG_SOFTWARE ones, so they
 * turn up next to the actual GPUs and would otherwise be reported as hardware. */
bool is_virtual_display(const std::string& name) {
  static const char* kPatterns[] = {
    "virtual display", "virtual monitor", "idddriver", "indirect display",
    "basic render", "remote display", "mirror driver", "displaylink",
  };
  std::string low;
  low.reserve(name.size());
  for (char ch : name) low.push_back((char)std::tolower((unsigned char)ch));
  for (const char* p : kPatterns)
    if (low.find(p) != std::string::npos) return true;
  return false;
}

}  /* anonymous namespace */

/* probe_gpu/probe_npu are the platform entry points core.cpp calls, so they
 * must sit in devinfo itself: inside the anonymous namespace they get internal
 * linkage and every build fails to link. */
/* DXGI reports every indirect display driver as an extra adapter, and those
 * mirror the description of the GPU they extend, so name matching cannot
 * identify them. Count the real PCI display devices instead and let each
 * (vendor, device) pair account for at most that many adapters: a box with
 * eight identical 4090s has eight matching PCI devices and keeps all eight,
 * while a single iGPU shadowed by three virtual monitors keeps one. */
std::map<std::pair<unsigned, unsigned>, unsigned> pci_display_quota() {
  std::map<std::pair<unsigned, unsigned>, unsigned> quota;
  HDEVINFO set = SetupDiGetClassDevsA(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr,
                                      DIGCF_PRESENT);
  if (set == INVALID_HANDLE_VALUE) return quota;

  SP_DEVINFO_DATA did{};
  did.cbSize = sizeof did;
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &did); ++i) {
    char ids[1024] = {0};
    DWORD type = 0;
    DWORD need = sizeof ids;
    if (!SetupDiGetDeviceRegistryPropertyA(set, &did, SPDRP_HARDWAREID, &type,
                                           (PBYTE)ids, need, &need))
      continue;
    /* REG_MULTI_SZ: first string is the most specific hardware id. */
    const char* hw = ids;
    if (hw[0] != 'P' || hw[1] != 'C' || hw[2] != 'I') continue;  /* not PCI */
    unsigned vendor = 0, device = 0;
    const char* v = std::strstr(hw, "VEN_");
    const char* d = std::strstr(hw, "DEV_");
    if (!v || !d) continue;
    vendor = (unsigned)std::strtoul(v + 4, nullptr, 16);
    device = (unsigned)std::strtoul(d + 4, nullptr, 16);
    if (vendor && device) ++quota[{vendor, device}];
  }
  SetupDiDestroyDeviceInfoList(set);
  return quota;
}

void probe_gpu(Report& r) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory) return;

  NvmlApi nv = load_nvml();
  bool nv_ready = false;
  unsigned nv_count = 0;
  unsigned nv_seen = 0;
  if (nv.ok() && nv.init() == 0 && nv.count(&nv_count) == 0) nv_ready = true;

  auto quota = pci_display_quota();
  const bool have_quota = !quota.empty();
  std::map<std::pair<unsigned, unsigned>, unsigned> emitted;
  std::vector<Gpu> spares;  /* over-quota mirrors, used only as a fallback */

  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      std::string adapter_name = wide_to_utf8(desc.Description);
      if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && !is_virtual_display(adapter_name)) {
        Gpu g;
        g.name = adapter_name;
        switch (desc.VendorId) {
          case 0x10DE: g.vendor = "nvidia"; g.core_unit = "SM"; break;
          case 0x1002:
          case 0x1022: g.vendor = "amd"; g.core_unit = "CU"; break;
          case 0x8086: g.vendor = "intel"; g.core_unit = "EU"; break;
          case 0x5143: g.vendor = "qualcomm"; g.core_unit = "core"; break;
          default:     g.vendor = "other"; g.core_unit = "core"; break;
        }
        g.method = "DXGI";
        {
          char pci[16];
          std::snprintf(pci, sizeof pci, "%04x:%04x", desc.VendorId, desc.DeviceId);
          g.pci_id = pci;
        }
        if (desc.DedicatedVideoMemory > 0)
          g.vram_bytes = Num{(double)desc.DedicatedVideoMemory, true, false};

        /* Index NVML by NVIDIA adapters seen, not by DXGI adapter index: on a
         * box with an Intel iGPU in slot 0 the two orders differ. */
        if (g.vendor == "nvidia" && nv_ready && nv_seen < nv_count) {
          unsigned dev = 0;
          unsigned nv_i = nv_seen++;
          if (nv.handle(&dev, nv_i) == 0) {
            unsigned cores = 0;
            if (nv.num_cores && nv.num_cores(&dev, &cores) == 0 && cores > 0) {
              g.cores = Num{(double)cores, true, false};
              g.method += " + NVML";
            }
            unsigned mhz = 0;
            if (nv.clock && nv.clock(&dev, 0 /*graphics*/, &mhz) == 0 && mhz > 0)
              g.clock_ghz = Num{(double)mhz / 1000.0, true, false};
            NvmlMem mi{};
            if (nv.meminfo && nv.meminfo(&dev, &mi) == 0 && mi.total > 0)
              g.vram_bytes = Num{(double)mi.total, true, false};
            /* NVML reports both the negotiated and the capable link; the
             * capable one is what the slot allows and is the useful figure. */
            unsigned v = 0;
            if (nv.pcie_gen && nv.pcie_gen(&dev, &v) == 0) g.pcie_gen = v;
            if (nv.pcie_gen_max && nv.pcie_gen_max(&dev, &v) == 0) g.pcie_gen_max = v;
            if (nv.pcie_width && nv.pcie_width(&dev, &v) == 0) g.pcie_lanes = v;
            if (nv.pcie_width_max && nv.pcie_width_max(&dev, &v) == 0) g.pcie_lanes_max = v;
            int major = 0, minor = 0;
            if (nv.cc && nv.cc(&dev, &major, &minor) == 0 && major > 0) {
              char cc[16];
              std::snprintf(cc, sizeof cc, "sm_%d.%d", major, minor);
              g.arch = cc;
            }
          }
        }
        /* How many physical adapters claim this (vendor, device) pair?
         * Anything past that count is a virtual display mirroring real
         * hardware, which DXGI describes with the real card's name. */
        auto key = std::make_pair((unsigned)desc.VendorId, (unsigned)desc.DeviceId);
        bool within_quota = true;
        if (have_quota) {
          auto it = quota.find(key);
          unsigned allowed = (it == quota.end()) ? 0u : it->second;
          if (emitted[key] >= allowed) within_quota = false;
        }
        if (within_quota) {
          ++emitted[key];
          r.gpus.push_back(std::move(g));
        } else {
          spares.push_back(std::move(g));
        }
      }
    }
    adapter->Release();
  }
  factory->Release();
  if (nv.lib) FreeLibrary((HMODULE)nv.lib);

  /* If the quota said no to everything, the quota is wrong, not DXGI. Report
   * the unfiltered set: over-reporting is recoverable, reporting no GPU on a
   * machine that has one is not. */
  if (r.gpus.empty() && !spares.empty()) {
    r.gpus = std::move(spares);
    r.warnings.push_back("gpu list may include virtual display adapters");
  }
}

/* -------------------------------------------------------------- limits */
/* A job object is the Windows equivalent of a cgroup: Docker Desktop, Windows
 * Sandbox and most CI runners put the process in one. The memory ceiling and
 * the CPU rate cap are both worth knowing before scheduling anything. */
void probe_limits(Report& r) {
  Limits& L = r.limits;

  /* A NULL handle means "the job this process belongs to", which is only
   * supported from Windows 8. On 7 the call simply fails and the probe reports
   * nothing rather than a wrong number. */
  {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    if (QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
                                  &li, sizeof li, nullptr)) {
      const unsigned flags = li.BasicLimitInformation.LimitFlags;
      if ((flags & JOB_OBJECT_LIMIT_JOB_MEMORY) && li.JobMemoryLimit > 0)
        L.mem_bytes = Num{(double)li.JobMemoryLimit, true, false};
      else if ((flags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) && li.ProcessMemoryLimit > 0)
        L.mem_bytes = Num{(double)li.ProcessMemoryLimit, true, false};
    }
  }

  {
    JOBOBJECT_CPU_RATE_CONTROL_INFORMATION ci{};
    if (QueryInformationJobObject(nullptr, JobObjectCpuRateControlInformation,
                                  &ci, sizeof ci, nullptr) &&
        (ci.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_ENABLE) &&
        (ci.ControlFlags & JOB_OBJECT_CPU_RATE_CONTROL_HARD_CAP)) {
      /* CpuRate is in hundredths of a percent of the whole machine. */
      double cores = (double)ci.CpuRate / 10000.0 * (double)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
      if (cores > 0 && cores < (double)r.cpu.logical)
        L.cpu_cores = Num{cores, true, false};
    }
  }

  if (L.cpu_cores.has || L.mem_bytes.has) {
    L.container = true;
    if (L.cpu_cores.has) {
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.2f", L.cpu_cores.v);
      std::string n(buf);
      while (!n.empty() && n.back() == '0') n.pop_back();
      if (!n.empty() && n.back() == '.') n.pop_back();
      r.warnings.push_back("job object limits this process to " + n + " of " +
                           std::to_string(r.cpu.logical) + " logical CPUs");
    }
  }
}

/* ================================================================ NUMA */
void probe_numa(Report& r) {
  ULONG highest = 0;
  if (!GetNumaHighestNodeNumber(&highest) || highest == 0) return;  /* one node */

  r.numa.nodes = (unsigned)highest + 1;
  std::string cols;
  for (ULONG n = 0; n <= highest; ++n) {
    GROUP_AFFINITY ga{};
    unsigned cpus = 0;
    if (GetNumaNodeProcessorMaskEx((USHORT)n, &ga)) {
      /* One group's worth of CPUs per call. A node spanning groups is rare on
       * client hardware and would undercount here rather than overcount. */
      for (unsigned b = 0; b < 64; ++b)
        if (ga.Mask & (KAFFINITY(1) << b)) ++cpus;
    }
    if (!cols.empty()) cols += "+";
    cols += std::to_string(cpus);
  }
  r.numa.layout = std::to_string(r.numa.nodes) + " nodes: " + cols + " cores";
}

/* ---------------------------------------------------------------- NPU */
void probe_npu(Report& r) {
  /* Every shipping NPU today exposes a PnP node with a recognisable name.
   * SetupAPI is the cheap way to enumerate them - no WMI, no device opens. */
  HDEVINFO set = SetupDiGetClassDevsA(nullptr, "PCI", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
  if (set == INVALID_HANDLE_VALUE) return;

  SP_DEVINFO_DATA did{};
  did.cbSize = sizeof did;

  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &did); ++i) {
    char name[512] = {0};
    DWORD type = 0;
    if (!SetupDiGetDeviceRegistryPropertyA(set, &did, SPDRP_FRIENDLYNAME, &type,
                                           (PBYTE)name, (DWORD)sizeof name, nullptr)) {
      if (!SetupDiGetDeviceRegistryPropertyA(set, &did, SPDRP_DEVICEDESC, &type,
                                             (PBYTE)name, (DWORD)sizeof name, nullptr))
        continue;
    }
    std::string n = name;
    for (auto& ch : n) ch = (char)tolower((unsigned char)ch);

    bool is_npu = n.find("npu") != std::string::npos ||
                  n.find("ai boost") != std::string::npos ||
                  n.find("neural") != std::string::npos ||
                  n.find("hexagon") != std::string::npos;
    if (!is_npu) continue;

    Npu npu;
    npu.name = name;
    if (n.find("intel") != std::string::npos)      { npu.vendor = "intel"; npu.arch = "NPU"; }
    else if (n.find("amd") != std::string::npos)   { npu.vendor = "amd"; npu.arch = "XDNA"; }
    else if (n.find("qualcomm") != std::string::npos) { npu.vendor = "qualcomm"; npu.arch = "Hexagon"; }
    else                                           { npu.vendor = "other"; npu.arch = "NPU"; }
    npu.method = "SetupAPI PCI enumeration";
    r.npus.push_back(std::move(npu));
  }
  SetupDiDestroyDeviceInfoList(set);
}

/* ================================================================ CPU */
/* Windows 10 exposes the hybrid topology through CPU Sets: every logical CPU
 * carries an EfficiencyClass and the highest class is the performance tier.
 * Compiled out on toolchains without the Win10 SDK. */
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0A00
typedef BOOL(WINAPI* GetCpuSetInfo_t)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG,
                                      HANDLE, ULONG);
#endif

void detect_hybrid(Cpu& c) {
#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0A00
  /* GetSystemCpuSetInformation only exists on Windows 10 and later. Importing
   * it directly makes the loader refuse to start the whole program on 7 and 8
   * with "procedure entry point could not be located", which is a fine way to
   * lose every machine you actually care about. Resolve it at run time. */
  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  if (!k32) return;
  auto get_info = (GetCpuSetInfo_t)(void*)GetProcAddress(k32, "GetSystemCpuSetInformation");
  if (!get_info) return;

  DWORD len = 0;
  get_info(nullptr, 0, &len, GetCurrentProcess(), 0);
  if (len == 0) return;
  std::vector<unsigned char> buf(len);
  if (!get_info(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data()),
                len, &len, GetCurrentProcess(), 0))
    return;

  /* Group logical CPUs into tiers and dedupe SMT siblings by (group, core). */
  struct Tier {
    unsigned char cls = 0;
    std::vector<std::string> cores;
  };
  std::vector<Tier> tiers;

  size_t off = 0;
  while (off + sizeof(SYSTEM_CPU_SET_INFORMATION) <= len) {
    auto* info = reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buf.data() + off);
    if (info->Size == 0) break;
    off += info->Size;
    if (info->Type != CpuSetInformation) continue;

    const auto& cs = info->CpuSet;
    Tier* t = nullptr;
    for (auto& x : tiers) if (x.cls == cs.EfficiencyClass) t = &x;
    if (!t) {
      tiers.push_back(Tier{});
      tiers.back().cls = cs.EfficiencyClass;
      t = &tiers.back();
    }
    std::string key = std::to_string(cs.Group) + "/" + std::to_string(cs.CoreIndex);
    bool dup = false;
    for (const auto& s : t->cores) if (s == key) { dup = true; break; }
    if (!dup) t->cores.push_back(key);
  }

  if (tiers.size() < 2) return;   /* uniform part: no P/E split to report */

  size_t best = 0;
  for (size_t i = 1; i < tiers.size(); ++i)
    if (tiers[i].cls > tiers[best].cls) best = i;
  unsigned p = (unsigned)tiers[best].cores.size();
  unsigned e = 0;
  for (size_t i = 0; i < tiers.size(); ++i)
    if (i != best) e += (unsigned)tiers[i].cores.size();
  if (p && e) {
    c.p_cores = p;
    c.e_cores = e;
    c.p_name = "P";
    c.e_name = "E";
  }
#else
  (void)c;
#endif
}

/* ------------------------------------------------------- clock and memory */
/* The brand string carries the guaranteed base clock ("@ 2.50GHz"), which is
 * what a datasheet quotes and roughly half of what the part actually reaches.
 * ACPI knows the ceiling; without it every roofline number is off by the turbo
 * ratio. */
struct DevPowerInfo {
  ULONG Number;
  ULONG MaxMhz;
  ULONG CurrentMhz;
  ULONG MhzLimit;
  ULONG MaxIdleState;
  ULONG CurrentIdleState;
};

void read_clock_ceiling(Cpu& c) {
  std::vector<DevPowerInfo> ppi(64);
  if (CallNtPowerInformation(ProcessorInformation, nullptr, 0, ppi.data(),
                             (ULONG)(ppi.size() * sizeof(ppi[0]))) != 0)
    return;
  unsigned best = 0;
  for (const auto& p : ppi)
    if (p.MaxMhz > best) best = p.MaxMhz;
  if (best > 0) c.clock_max_ghz = Num{(double)best / 1000.0, true, false};
}

/* SMBIOS type 17 describes each installed DIMM. The board's configured speed
 * is often below the module's rated speed, which is exactly the difference
 * between the bandwidth on the box and the bandwidth on the datasheet. */
void read_memory_speed(Cpu& c) {
  const DWORD size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
  if (size < 8) return;
  std::vector<unsigned char> buf(size);
  const DWORD got = GetSystemFirmwareTable('RSMB', 0, buf.data(), size);
  if (got < 8) return;

  unsigned modules = 0, best_configured = 0, best_rated = 0;
  size_t off = 8;                                  /* past "RSMB" + length */
  while (off + 4 <= got) {
    const unsigned char type = buf[off];
    const unsigned char len = buf[off + 1];
    if (len < 4) break;
    if (type == 0x7F) break;                       /* end-of-table */

    if (type == 17 && off + len <= got) {
      auto u16 = [&](size_t at) -> unsigned {
        if (at + 1 >= len) return 0;
        return (unsigned)buf[off + at] | ((unsigned)buf[off + at + 1] << 8);
      };
      const unsigned msize = u16(0x08);            /* MB, 0 = empty slot */
      if (msize != 0 && msize != 0xFFFF) {
        ++modules;
        const unsigned rated = u16(0x15);
        const unsigned configured = u16(0x20);
        if (rated > best_rated) best_rated = rated;
        if (configured > best_configured) best_configured = configured;
      }
    }

    size_t p = off + len;                          /* skip the string area */
    while (p + 1 < got && !(buf[p] == 0 && buf[p + 1] == 0)) ++p;
    off = p + 2;
  }

  c.mem_modules = modules;
  const unsigned speed = best_configured ? best_configured : best_rated;
  if (speed) c.mem_speed_mts = Num{(double)speed, true, false};
}

void probe_cpu(Cpu& c, Report& r) {
  c.arch = r.arch;

#if defined(DEVINFO_X86)
  c.vendor = cpu_vendor_string();
  if (c.vendor == "GenuineIntel") c.vendor = "Intel";
  else if (c.vendor == "AuthenticAMD") c.vendor = "AMD";
  c.model = cpu_brand_string();
  cpu_features(c);
#endif

  if (c.model.empty()) {
    c.model = reg_string("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString");
    size_t b = c.model.find_first_not_of(' ');
    if (b != std::string::npos) c.model = c.model.substr(b);
  }

  unsigned phys = physical_cores();
  c.physical = phys;
  DWORD logical = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  if (logical != (DWORD)-1) c.logical = logical;

  detect_hybrid(c);

  read_caches(c);
  if (c.line == 0) c.line = 64;

  MEMORYSTATUSEX ms{};
  ms.dwLength = sizeof ms;
  if (GlobalMemoryStatusEx(&ms)) c.mem_bytes = Num{(double)ms.ullTotalPhys, true, false};

  double ghz = parse_ghz(c.model);
  if (ghz <= 0) {
    DWORD mhz = reg_dword("HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "~MHz");
    if (mhz) ghz = (double)mhz / 1000.0;
  }
  if (ghz > 0) c.clock_ghz = Num{ghz, true, false};

  read_clock_ceiling(c);
  read_memory_speed(c);
}

}  /* namespace devinfo */
