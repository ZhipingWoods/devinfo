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

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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

void cpu_features(Cpu& c) {
  int r[4] = {0};
  cpuid(r, 1, 0);
  bool sse2 = (r[3] & (1 << 26)) != 0;
  bool avx = (r[2] & (1 << 28)) != 0;
  if (sse2) c.features.push_back("SSE2");
  if (avx) c.features.push_back("AVX");

  cpuid(r, 7, 0);
  if (r[1] & (1 << 5))  c.features.push_back("AVX2");
  if (r[1] & (1 << 16)) c.features.push_back("AVX512F");
  if (r[1] & (1 << 30)) c.features.push_back("AVX512BW");
  if (r[3] & (1 << 3))  c.features.push_back("BMI1");
  if (r[3] & (1 << 8))  c.features.push_back("BMI2");
  if (r[1] & (1 << 3))  c.features.push_back("FMA");
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
void probe_gpu(Report& r) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory) return;

  NvmlApi nv = load_nvml();
  bool nv_ready = false;
  unsigned nv_count = 0;
  unsigned nv_seen = 0;
  if (nv.ok() && nv.init() == 0 && nv.count(&nv_count) == 0) nv_ready = true;

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
            int major = 0, minor = 0;
            if (nv.cc && nv.cc(&dev, &major, &minor) == 0 && major > 0) {
              char cc[16];
              std::snprintf(cc, sizeof cc, "sm_%d.%d", major, minor);
              g.arch = cc;
            }
          }
        }
        r.gpus.push_back(std::move(g));
      }
    }
    adapter->Release();
  }
  factory->Release();
  if (nv.lib) FreeLibrary((HMODULE)nv.lib);
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
}

}  /* namespace devinfo */
