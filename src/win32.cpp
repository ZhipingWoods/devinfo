/* win32.cpp - Windows probe.
 *
 * CPUID + Kernel32 topology + registry for the CPU, DXGI for displays-class
 * adapters, NVML for NVIDIA detail, SetupAPI for NPUs. No WMI (slow), no
 * shelling out, no device-context creation.
 */
#include "devinfo_core.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_1.h>
#include <setupapi.h>

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
      const CACHE_INFO& ci = e->Cache;
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

void probe_gpu(Report& r) {
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) || !factory) return;

  NvmlApi nv = load_nvml();
  bool nv_ready = false;
  unsigned nv_count = 0;
  if (nv.ok() && nv.init() == 0 && nv.count(&nv_count) == 0) nv_ready = true;

  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        Gpu g;
        g.name = wide_to_utf8(desc.Description);
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

        /* Enrich the first NVIDIA adapter through NVML. */
        if (g.vendor == "nvidia" && nv_ready && nv_count > i) {
          unsigned dev = 0;
          if (nv.handle(&dev, i) == 0) {
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

}  /* anonymous namespace */

/* ================================================================ CPU */
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

  /* Windows does not publish a performance-tier split; assume uniform. */
  c.p_cores = c.physical;

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
