/* core.cpp - everything that is NOT platform specific.
 *
 *   - OS / kernel / container / VM identity
 *   - accelerator stack discovery (pure dlopen, never a subprocess)
 *   - peak-throughput derivation from detected counts + the builtin table
 *   - optional memory bandwidth micro-benchmark
 *   - probe() orchestration
 */
#include "devinfo_core.hpp"

#include "devinfo.h"

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#endif

namespace devinfo {
namespace {

/* ------------------------------------------------------------ helpers */
bool file_exists(const char* p) {
#if defined(_WIN32)
  DWORD a = GetFileAttributesA(p);
  return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return ::stat(p, &st) == 0;
#endif
}

std::string read_small_file(const char* p, size_t cap = 4096) {
  std::string out;
  FILE* f = std::fopen(p, "rb");
  if (!f) return out;
  out.resize(cap);
  size_t n = std::fread(&out[0], 1, cap, f);
  std::fclose(f);
  out.resize(n);
  return out;
}

bool contains(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

[[maybe_unused]] std::string slurp_lower(const char* p) {
  std::string s = read_small_file(p);
  for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}

Num N(double v) { Num n; n.v = v; n.has = true; n.estimated = false; return n; }
Num E(double v) { Num n; n.v = v; n.has = true; n.estimated = true; return n; }

/* ----------------------------------------------------- dynamic loading */
void* lib_open(const char* name) {
#if defined(_WIN32)
  return (void*)LoadLibraryA(name);
#else
  return ::dlopen(name, RTLD_LAZY | RTLD_LOCAL);
#endif
}

void* lib_sym(void* handle, const char* sym) {
  if (!handle) return nullptr;
#if defined(_WIN32)
  return (void*)GetProcAddress((HMODULE)handle, sym);
#else
  return ::dlsym(handle, sym);
#endif
}

struct AccelHit {
  void* handle = nullptr;
  const char* lib = nullptr;
};

AccelHit load_accel(const char* const* names) {
  AccelHit h;
  for (int i = 0; names[i]; ++i) {
    h.handle = lib_open(names[i]);
    if (h.handle) { h.lib = names[i]; return h; }
  }
  return h;
}

/* Version reporting conventions differ per vendor, so each decoder below
 * is spelled out rather than guessed from a generic template. */
typedef int (*ver_intptr_fn)(int*);
typedef size_t (*ver_size_fn)(void);
typedef int (*ver_int_fn)(void);
typedef int (*ver_charbuf_fn)(char*, unsigned);

std::string fmt_version(unsigned major, unsigned minor, unsigned patch) {
  char buf[48];
  if (patch) std::snprintf(buf, sizeof buf, "%u.%u.%u", major, minor, patch);
  else       std::snprintf(buf, sizeof buf, "%u.%u", major, minor);
  return buf;
}

void add_accel(Report& r, Accel a) { r.accels.push_back(std::move(a)); }

double parse_ghz_from_brand(const std::string& s) {
  size_t at = s.rfind('@');
  if (at == std::string::npos) return 0.0;
  return std::atof(s.c_str() + at + 1);
}

bool has_feature(const Cpu& c, const char* f) {
  size_t n = std::strlen(f);
  for (const auto& x : c.features) {
    if (x.size() != n) continue;
    bool same = true;
    for (size_t i = 0; i < n && same; ++i)
      same = std::tolower((unsigned char)x[i]) == std::tolower((unsigned char)f[i]);
    if (same) return true;
  }
  return false;
}

}  /* anonymous namespace */

/* ================================================================ accels */
void collect_accels(Report& r) {
  /* ---------------- NVIDIA / CUDA ---------------- */
  {
    static const char* kLib[] = {
#if defined(_WIN32)
      "nvcuda.dll", nullptr
#elif defined(__APPLE__)
      "libcuda.dylib", nullptr
#else
      "libcuda.so.1", "libcuda.so", nullptr
#endif
    };
    AccelHit h = load_accel(kLib);
    Accel a{"cuda", "driver", false, "", ""};
    if (h.handle) {  /* NOLINT */
      int v = 0;
      auto fn = (ver_intptr_fn)lib_sym(h.handle, "cuDriverGetVersion");
      a.available = true;
      a.detail = std::string("lib=") + h.lib;
      if (fn && fn(&v) == 0)
        a.version = fmt_version((unsigned)(v / 1000), (unsigned)((v % 1000) / 10), 0);
    }
#if defined(__linux__)
    if (!a.available) {
      std::string nv = read_small_file("/proc/driver/nvidia/version", 512);
      if (!nv.empty()) {
        a.available = true;
        a.version = "present";
        a.detail = "via /proc/driver/nvidia";
      }
    }
#endif
    add_accel(r, a);
  }
  {
    static const char* kLib[] = {
#if defined(_WIN32)
      "cudart64_12.dll", "cudart64_11.dll", "cudart64_10.dll", nullptr
#elif defined(__APPLE__)
      "libcudart.dylib", nullptr
#else
      "libcudart.so.12", "libcudart.so.11", "libcudart.so", nullptr
#endif
    };
    AccelHit h = load_accel(kLib);
    Accel a{"cuda_runtime", "runtime", false, "", ""};
    if (h.handle) {
      int v = 0;
      auto fn = (ver_intptr_fn)lib_sym(h.handle, "cudaRuntimeGetVersion");
      if (fn && fn(&v) == 0) {
        a.available = true;
        a.version = fmt_version((unsigned)(v / 1000), (unsigned)((v % 1000) / 10), 0);
        a.detail = std::string("lib=") + h.lib;
      }
    }
    if (a.available) add_accel(r, a);
  }
  {
    static const char* kLib[] = {
#if defined(_WIN32)
      "nvml.dll", nullptr
#else
      "libnvidia-ml.so.1", "libnvidia-ml.so", nullptr
#endif
    };
    AccelHit h = load_accel(kLib);
    Accel a{"nvml", "lib", false, "", ""};
    if (h.handle) {
      a.available = true;
      a.detail = std::string("lib=") + h.lib;
#if defined(__linux__)
      /* The kernel already publishes the driver version; querying NVML would
       * cost ~28 ms of driver init for information we can read for free. */
      /* e.g. "... Kernel Module for x86_64  580.119.02  Release Build".
       * Scan tokens after the prefix and take the first one that looks like a
       * version: a leading digit and a dot. Naively grabbing the first digit
       * would land on the "8" of "x86_64". */
      std::string nv = read_small_file("/proc/driver/nvidia/version", 512);
      size_t at = nv.find("Kernel Module");
      if (at != std::string::npos) {
        std::string::size_type i = at + 13;
        while (i < nv.size()) {
          std::string::size_type b = nv.find_first_not_of(" \t", i);
          if (b == std::string::npos) break;
          std::string::size_type e = nv.find_first_of(" \t\r\n", b);
          if (e == std::string::npos) e = nv.size();
          std::string tok = nv.substr(b, e - b);
          if (!tok.empty() && tok.find_first_of("0123456789") == 0 &&
              tok.find('.') != std::string::npos) {
            a.version = tok;
            a.detail = "/proc/driver/nvidia";
            break;
          }
          i = e;
        }
      }
#endif
      if (a.version.empty()) {
        /* NVML refuses queries until nvmlInit_v2() has run. */
        auto init = (ver_int_fn)lib_sym(h.handle, "nvmlInit_v2");
        if (init && init() == 0) {
          char buf[80] = {0};
          auto fn = (ver_charbuf_fn)lib_sym(h.handle, "nvmlSystemGetDriverVersion");
          if (fn && fn(buf, (unsigned)sizeof buf) == 0) {
            a.version = buf;
            a.detail += " (driver)";
          }
        }
      }
    }
    if (a.available) add_accel(r, a);
  }
  {
    static const char* kLib[] = {
#if defined(_WIN32)
      "cudnn64_9.dll", "cudnn64_8.dll", nullptr
#elif defined(__APPLE__)
      "libcudnn.dylib", nullptr
#else
      "libcudnn.so.9", "libcudnn.so.8", "libcudnn.so", nullptr
#endif
    };
    AccelHit h = load_accel(kLib);
    Accel a{"cudnn", "lib", false, "", ""};
    if (h.handle) {
      auto fn = (ver_size_fn)lib_sym(h.handle, "cudnnGetVersion");
      if (fn) {
        size_t v = fn();
        a.available = true;
        a.version = fmt_version((unsigned)(v / 1000), (unsigned)((v % 1000) / 100), (unsigned)(v % 100));
        a.detail = std::string("lib=") + h.lib;
      }
    }
    if (a.available) add_accel(r, a);
  }
  {
    static const char* kLib[] = {
#if defined(_WIN32)
      "nvinfer_10.dll", "nvinfer_8.dll", "nvinfer.dll", nullptr
#elif defined(__APPLE__)
      "libnvinfer.dylib", nullptr
#else
      "libnvinfer.so.10", "libnvinfer.so.8", "libnvinfer.so", nullptr
#endif
    };
    AccelHit h = load_accel(kLib);
    Accel a{"tensorrt", "lib", false, "", ""};
    if (h.handle) {
      auto fn = (ver_int_fn)lib_sym(h.handle, "getInferLibVersion");
      if (fn) {
        int v = fn();
        a.available = true;
        a.version = fmt_version((unsigned)(v / 10000), (unsigned)((v % 10000) / 100), (unsigned)(v % 100));
        a.detail = std::string("lib=") + h.lib;
      }
    }
    if (a.available) add_accel(r, a);
  }
  {
    static const char* kLib[] = {
#if defined(_WIN32)
      "nccl.dll", nullptr
#else
      "libnccl.so.2", "libnccl.so", nullptr
#endif
    };
    AccelHit h = load_accel(kLib);
    Accel a{"nccl", "lib", false, "", ""};
    if (h.handle) {
      int v = 0;
      auto fn = (ver_intptr_fn)lib_sym(h.handle, "ncclGetVersion");
      if (fn && fn(&v) == 0) {
        a.available = true;
        a.version = fmt_version((unsigned)(v / 10000), (unsigned)((v % 10000) / 100), (unsigned)(v % 100));
      }
    }
    if (a.available) add_accel(r, a);
  }

  /* ---------------- AMD / ROCm ---------------- */
  {
    static const char* kLib[] = {
#if defined(_WIN32)
      "amdhip64.dll", nullptr
#else
      "libamdhip64.so.6", "libamdhip64.so", nullptr
#endif
    };
    AccelHit h = load_accel(kLib);
    Accel a{"rocm", "runtime", false, "", ""};
    if (h.handle) {
      int v = 0;
      auto fn = (ver_intptr_fn)lib_sym(h.handle, "hipRuntimeGetVersion");
      a.available = true;
      a.detail = std::string("lib=") + h.lib;
      if (fn && fn(&v) == 0)
        a.version = fmt_version((unsigned)(v / 10000000), (unsigned)((v % 10000000) / 100000), 0);
    }
#if defined(__linux__)
    if (!a.available && file_exists("/opt/rocm/.info/version")) {
      std::string s = read_small_file("/opt/rocm/.info/version", 64);
      while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
      a.available = true;
      a.version = s;
      a.detail = "/opt/rocm";
    }
#endif
    if (a.available) add_accel(r, a);
  }

  /* ---------------- Huawei / CANN ---------------- */
  {
    static const char* kLib[] = {
#if defined(_WIN32)
      nullptr
#else
      "libascendcl.so", "libascend_hal.so", nullptr
#endif
    };
    AccelHit h = load_accel(kLib);
    Accel a{"cann", "toolkit", false, "", ""};
    if (h.handle) {
      a.available = true;
      a.detail = std::string("lib=") + h.lib;
    }
#if defined(__linux__)
    const char* kCfg[] = {
      "/usr/local/Ascend/ascend-toolkit/latest/ascend_toolkit_install.info",
      "/usr/local/Ascend/ascend-toolkit/latest/version.cfg",
      "/usr/local/Ascend/driver/version.info",
    };
    for (const char* cfg : kCfg) {
      if (!file_exists(cfg)) continue;
      std::string s = read_small_file(cfg, 1024);
      if (s.empty()) continue;
      a.available = true;
      if (a.version.empty()) {
        size_t eq = s.find("version=");
        std::string v = eq != std::string::npos ? s.substr(eq + 8, 32) : s.substr(0, 32);
        size_t cut = v.find_first_of("\r\n");
        if (cut != std::string::npos) v.erase(cut);
        if (!v.empty()) a.version = v;
        a.detail = cfg;
      }
    }
#endif
    if (a.available) add_accel(r, a);
  }

  /* ---------------- Intel / generic runtimes ---------------- */
  {
    static const char* kLib[] = {
#if defined(_WIN32)
      "openvino.dll", nullptr
#else
      "libopenvino.so", "libopenvino_c.so", nullptr
#endif
    };
    AccelHit h = load_accel(kLib);
    Accel a{"openvino", "runtime", false, "", ""};
    if (h.handle) { a.available = true; a.detail = std::string("lib=") + h.lib; }
    if (a.available) add_accel(r, a);
  }
  {
    static const char* kLib[] = {
#if defined(_WIN32)
      "onnxruntime.dll", nullptr
#elif defined(__APPLE__)
      "libonnxruntime.dylib", nullptr
#else
      "libonnxruntime.so.1", "libonnxruntime.so", nullptr
#endif
    };
    AccelHit h = load_accel(kLib);
    Accel a{"onnxruntime", "runtime", false, "", ""};
    if (h.handle) { a.available = true; a.detail = std::string("lib=") + h.lib; }
    if (a.available) add_accel(r, a);
  }
#if defined(_WIN32)
  {
    static const char* kLib[] = {"DirectML.dll", nullptr};
    AccelHit h = load_accel(kLib);
    Accel a{"directml", "runtime", false, "", ""};
    if (h.handle) { a.available = true; a.detail = std::string("lib=") + h.lib; }
    if (a.available) add_accel(r, a);
  }
#endif
  {
    static const char* kLib[] = {
#if defined(_WIN32)
      "vulkan-1.dll", nullptr
#elif defined(__APPLE__)
      "libvulkan.dylib", "libvulkan.1.dylib", nullptr
#else
      "libvulkan.so.1", "libvulkan.so", nullptr
#endif
    };
    AccelHit h = load_accel(kLib);
    Accel a{"vulkan", "runtime", false, "", ""};
    if (h.handle) { a.available = true; a.detail = std::string("lib=") + h.lib; }
    if (a.available) add_accel(r, a);
  }
  {
    static const char* kLib[] = {
#if defined(_WIN32)
      "OpenCL.dll", nullptr
#elif defined(__APPLE__)
      "/System/Library/Frameworks/OpenCL.framework/OpenCL", nullptr
#else
      "libOpenCL.so.1", "libOpenCL.so", nullptr
#endif
    };
    AccelHit h = load_accel(kLib);
    Accel a{"opencl", "runtime", false, "", ""};
    if (h.handle) { a.available = true; a.detail = std::string("lib=") + h.lib; }
    if (a.available) add_accel(r, a);
  }
  {
    static const char* kLib[] = {
#if defined(_WIN32)
      "sycl8.dll", "sycl7.dll", nullptr
#else
      "libsycl.so.8", "libsycl.so", nullptr
#endif
    };
    AccelHit h = load_accel(kLib);
    Accel a{"sycl", "runtime", false, "", ""};
    if (h.handle) { a.available = true; a.detail = std::string("lib=") + h.lib; }
    if (a.available) add_accel(r, a);
  }
#if defined(__APPLE__)
  if (lib_open("/System/Library/Frameworks/Metal.framework/Metal"))
    add_accel(r, Accel{"metal", "runtime", true, "", "system framework"});
#endif

  /* -------- compile toolchains: cheap stat() calls, never exec -------- */
  {
    struct Tool { const char* name; const char* path; };
    static const Tool kTools[] = {
#if defined(_WIN32)
      {"nvcc", "C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA\\v12.4\\bin\\nvcc.exe"},
      {"hipcc", "C:\\Program Files\\AMD\\ROCm\\bin\\hipcc.exe"},
      {"npu-smi", "C:\\Program Files\\Huawei\\Ascend\\tools\\npu-smi.exe"},
#else
      {"nvcc", "/usr/local/cuda/bin/nvcc"},
      {"hipcc", "/opt/rocm/bin/hipcc"},
      {"npu-smi", "/usr/local/Ascend/driver/tools/npu-smi"},
#endif
      {nullptr, nullptr},
    };
    for (int i = 0; kTools[i].name; ++i) {
      if (!file_exists(kTools[i].path)) continue;
      add_accel(r, Accel{kTools[i].name, "toolchain", true, "", kTools[i].path});
    }
  }
}

/* ============================================================ environment */
void probe_environment(Report& r) {
#if defined(_WIN32)
  typedef struct { unsigned long size, major, minor, build, platform; wchar_t csd[128]; } OsVer;
  typedef long(WINAPI* RtlGetVersion_t)(OsVer*);

  r.os = "windows";
#if defined(_M_ARM64)
  r.arch = "arm64";
#else
  r.arch = "x86_64";
#endif
  {
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    auto fn = (RtlGetVersion_t)GetProcAddress(nt, "RtlGetVersion");
    OsVer vi{};
    vi.size = sizeof(vi);
    if (fn && fn(&vi) == 0) {
      char b[64];
      std::snprintf(b, sizeof b, "%lu.%lu.%lu", vi.major, vi.minor, vi.build);
      r.os_version = b;
      r.kernel = std::string("Windows ") + b;
      /* Windows 11 still reports itself as 10.0: the build number is the only
       * thing that separates them. Without this a Win11 box reads "windows". */
      if (vi.major == 10 && vi.build >= 22000) r.os = "windows 11";
      else if (vi.major == 10) r.os = "windows 10";
    }
  }
  {
    char host[256] = {0};
    DWORD n = (DWORD)sizeof host;
    if (GetComputerNameA(host, &n)) r.host = host;
  }
  {
    char buf[8];
    if (GetEnvironmentVariableA("WSL_DISTRO_NAME", buf, (DWORD)sizeof buf) > 0) r.env = "wsl";
    else r.env = "native";
  }
#else
  struct utsname u {};
  if (uname(&u) == 0) {
    r.kernel = std::string(u.sysname) + " " + u.release;
    r.arch = u.machine;
  }
  {
    char host[256] = {0};
    if (gethostname(host, sizeof host - 1) == 0) r.host = host;
  }
#if defined(__APPLE__)
  r.os = "macos";
  {
    char buf[64] = {0};
    size_t sz = sizeof buf;
    if (sysctlbyname("kern.osproductversion", buf, &sz, nullptr, 0) == 0) r.os_version = buf;
  }
  {
    int hv = 0;
    size_t sz = sizeof hv;
    r.env = (sysctlbyname("kern.hv_vmm_present", &hv, &sz, nullptr, 0) == 0 && hv) ? "vm" : "native";
  }
#else
  r.os = "linux";
  {
    std::string rel = read_small_file("/etc/os-release", 2048);
    size_t p = rel.find("PRETTY_NAME=");
    if (p != std::string::npos) {
      size_t b = rel.find('"', p), e = rel.find('"', b + 1);
      if (b != std::string::npos && e != std::string::npos) r.os_version = rel.substr(b + 1, e - b - 1);
    }
  }
  if (getenv("WSL_DISTRO_NAME") || contains(slurp_lower("/proc/version"), "microsoft")) {
    r.env = "wsl";
  } else if (file_exists("/.dockerenv") || contains(slurp_lower("/proc/1/cgroup"), "docker") ||
             contains(slurp_lower("/proc/1/cgroup"), "kubepods") ||
             contains(slurp_lower("/proc/1/cgroup"), "lxc")) {
    r.env = "container";
  } else {
    std::string dmi = slurp_lower("/sys/class/dmi/id/product_name") +
                      slurp_lower("/sys/class/dmi/id/sys_vendor");
    r.env = (contains(dmi, "qemu") || contains(dmi, "virtualbox") || contains(dmi, "vmware") ||
             contains(dmi, "kvm") || contains(dmi, "amazon ec2") || contains(dmi, "google compute") ||
             contains(dmi, "microsoft corporation") || contains(dmi, "bochs"))
                ? "vm"
                : "native";
  }
#endif
#endif
}

/* ================================================================ peaks */
/* /proc/cpuinfo hands back every flag the kernel knows, most of which say
 * nothing about what a model runtime can use. Map the ones that matter onto
 * the same names Windows and macOS produce, in a fixed order, so a report from
 * a Linux box and a report from a Windows box can be diffed directly. */
void canonical_features(std::vector<std::string>& feats, const std::string& arch) {
  struct Entry { const char* key; const char* name; };
  /* Split by architecture on purpose: x86 and ARM reuse the same word for
   * different things. "sme" on x86 is Secure Memory Encryption, on ARM it is
   * the Scalable Matrix Extension, and reporting the second from the first
   * would claim a matrix unit that does not exist. */
  static const Entry kX86[] = {
    {"sse2", "SSE2"}, {"sse4_1", "SSE4.1"}, {"sse4_2", "SSE4.2"}, {"popcnt", "POPCNT"},
    {"avx", "AVX"}, {"avx2", "AVX2"}, {"fma", "FMA"}, {"f16c", "F16C"},
    {"bmi1", "BMI1"}, {"bmi2", "BMI2"}, {"aes", "AES"}, {"sha_ni", "SHA"},
    {"gfni", "GFNI"}, {"vaes", "VAES"}, {"vpclmulqdq", "VPCLMULQDQ"},
    {"avx512f", "AVX512F"}, {"avx512dq", "AVX512DQ"}, {"avx512cd", "AVX512CD"},
    {"avx512bw", "AVX512BW"}, {"avx512vl", "AVX512VL"},
    {"avx512ifma", "AVX512IFMA"}, {"avx512vbmi", "AVX512_VBMI"},
    {"avx512vbmi2", "AVX512_VBMI2"}, {"avx512_vnni", "AVX512_VNNI"},
    {"avx512_bitalg", "AVX512_BITALG"}, {"avx512_vpopcntdq", "AVX512_VPOPCNTDQ"},
    {"avx512_bf16", "AVX512_BF16"}, {"avx512_fp16", "AVX512_FP16"},
    {"avx512_vp2intersect", "AVX512_VP2INTERSECT"}, {"avx_vnni", "AVX_VNNI"},
    {"amx_tile", "AMX_TILE"}, {"amx_int8", "AMX_INT8"}, {"amx_bf16", "AMX_BF16"},
  };
  static const Entry kArm[] = {
    {"asimd", "NEON"}, {"fp", "FP"}, {"fphp", "FP16"}, {"asimdhp", "FP16"},
    {"asimdbf16", "BF16"}, {"bf16", "BF16"}, {"i8mm", "I8MM"},
    {"asimddp", "DotProd"}, {"sve", "SVE"}, {"sve2", "SVE2"},
    {"sme", "SME"}, {"sme2", "SME2"},
    {"pmull", "PMULL"}, {"sha1", "SHA1"}, {"sha2", "SHA256"}, {"sha3", "SHA3"},
    {"sha512", "SHA512"}, {"crc32", "CRC32"}, {"atomics", "LSE"},
    {"jscvt", "JSCVT"}, {"fcma", "FCMA"}, {"asimdfhm", "FHM"},
    {"asimdrdm", "RDM"}, {"lrcpc", "LRCPC"}, {"dcpop", "DCPOP"}, {"sb", "SB"},
  };

  const bool is_arm = arch == "aarch64" || arch == "arm64";
  const Entry* map_begin = is_arm ? kArm : kX86;
  const size_t map_size = is_arm ? sizeof(kArm) / sizeof(kArm[0])
                                 : sizeof(kX86) / sizeof(kX86[0]);

  auto lower = [](std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
  };

  std::vector<std::string> low;
  low.reserve(feats.size());
  for (const auto& f : feats) low.push_back(lower(f));

  std::vector<std::string> out;
  std::vector<bool> taken(low.size(), false);
  for (size_t m = 0; m < map_size; ++m) {
    const Entry& e = map_begin[m];
    const std::string want = lower(e.name);
    for (size_t i = 0; i < low.size(); ++i) {
      if (taken[i]) continue;
      if (low[i] == e.key || low[i] == want) {
        out.push_back(e.name);
        taken[i] = true;
        break;
      }
    }
  }
  /* Anything we appended ourselves (SVE_VL=..., SME_VL=...) rides along. */
  for (size_t i = 0; i < feats.size(); ++i) {
    if (taken[i]) continue;
    if (feats[i].find('=') != std::string::npos) out.push_back(feats[i]);
  }
  feats.swap(out);
}

void derive_cpu_peaks(Cpu& c) {
  if (c.physical == 0) return;

  if (c.vendor == "Apple") {
    const AppleChip* ch = apple_chip(c.model);
    if (!ch) return;
    c.clock_ghz = E(ch->p_ghz);
    c.mem_bw_gbs = E(ch->bw_gbs);
    /* P-core: four 128-bit pipes => 32 FP32 FMA flops/cycle. E-core: 16. */
    double flops = c.p_cores * ch->p_ghz * 32.0 + c.e_cores * ch->e_ghz * 16.0;
    c.peak_fp32_tflops = E(flops / 1000.0);
    return;
  }

  unsigned lanes = 8;  /* SSE baseline */
  if (has_feature(c, "avx512f")) lanes = 32;
  else if (has_feature(c, "avx2") || has_feature(c, "avx")) lanes = 16;

  double ghz = c.clock_ghz.has ? c.clock_ghz.v : parse_ghz_from_brand(c.model);
  if (ghz > 0) c.peak_fp32_tflops = E((double)c.physical * ghz * (double)lanes / 1000.0);
}

void derive_gpu_peaks(Gpu& g) {
  if (!g.cores.has) return;

  if (g.vendor == "apple") {
    double ghz = g.clock_ghz.has ? g.clock_ghz.v : apple_gpu_clock_ghz(g.name);
    if (ghz <= 0) return;
    g.clock_ghz = E(ghz);
    /* 128 FP32 ALUs per GPU core, FMA = 2 flops */
    g.fp32_tflops = E(g.cores.v * 128.0 * 2.0 * ghz / 1000.0);
    g.fp16_tflops = g.fp32_tflops;
    return;
  }

  if (g.vendor == "nvidia") {
    unsigned maj = 0, minr = 0;
    if (std::sscanf(g.arch.c_str(), "sm_%u.%u", &maj, &minr) != 2) return;
    const NvArch* a = nv_arch(maj, minr);
    if (!a) return;
    double ghz = g.clock_ghz.has ? g.clock_ghz.v : 0.0;
    if (ghz <= 0) return;
    g.fp32_tflops = E(g.cores.v * a->fp32_per_sm * 2.0 * ghz / 1000.0);
    bool dc = contains(g.name, "A100") || contains(g.name, "A800") || contains(g.name, "H100") ||
              contains(g.name, "H800") || contains(g.name, "H200") || contains(g.name, "B100") ||
              contains(g.name, "B200") || contains(g.name, "GB200") || contains(g.name, "V100");
    double tc = (double)(dc ? a->tc_flops_dc : a->tc_flops_c);
    if (tc > 0) {
      g.tensor_cores = E(g.cores.v * a->tc_per_sm);
      g.tc_per_core = a->tc_per_sm;
      g.fp16_tflops = E(g.cores.v * tc * ghz / 1000.0);
      g.bf16_tflops = g.fp16_tflops;
      g.fp16_tflops_sparse = E(g.fp16_tflops.v * 2.0);
    }
  }
}

/* ============================================================ benchmark */
/* A single core rarely saturates a modern DRAM interface, so the copy is
 * split across threads. Threads are created once and reused, so their cost
 * stays outside the timed region. */
Num measure_memory_bandwidth(uint64_t bytes_hint, unsigned threads_hint) {
  using clk = std::chrono::steady_clock;
  size_t n = (size_t)(bytes_hint ? bytes_hint : (64ull << 20));

  /* Copy bandwidth saturates early and then degrades: extra threads add
   * scheduling and cross-socket NUMA traffic. Measured sweeps on an Apple M5
   * (10 cores) and on a 2-socket 96-thread EPYC both peak at 4 threads:
   * EPYC 1/2/4/8 -> 29.5 / 40.2 / 44.2 / 33.7 GB/s. Do not scale with the
   * core count; clamp instead.
   *
   * Note the page-fault cost: first touch of the two buffers costs roughly
   * 60 ms per 128 MB on a VM. That happens before timing starts, so it slows
   * the call down but does not corrupt the number. */
  unsigned nt = threads_hint;
  if (nt == 0) {
    unsigned hw = std::thread::hardware_concurrency();
    nt = hw < 4 ? hw : 4;
  }
  if (nt < 1) nt = 1;
  if (nt > 4) nt = 4;
  if (n < (size_t)nt * 4096) nt = 1;

  std::vector<unsigned char> src(n, 1), dst(n, 0);
  size_t chunk = n / nt;
  std::vector<std::pair<size_t, size_t>> spans;
  for (unsigned t = 0; t < nt; ++t) {
    size_t off = (size_t)t * chunk;
    size_t len = (t + 1 == nt) ? (n - off) : chunk;
    spans.emplace_back(off, len);
  }

  /* First pass is untimed: it pays for page faults and lets the scheduler
   * spread the threads before anything is measured. */
  double best = 0.0;
  for (int rep = -1; rep < 6; ++rep) {
    std::vector<std::thread> pool;
    pool.reserve(nt);
    auto t0 = clk::now();
    for (unsigned t = 0; t < nt; ++t) {
      size_t off = spans[t].first, len = spans[t].second;
      pool.emplace_back([&src, &dst, off, len] {
        std::memcpy(dst.data() + off, src.data() + off, len);
      });
    }
    for (auto& th : pool) th.join();
    auto t1 = clk::now();
    if (rep < 0) continue;

    double sec = std::chrono::duration<double>(t1 - t0).count();
    if (sec <= 0) continue;
    double gbs = (double)n * 2.0 / sec / 1e9;   /* read + written */
    if (gbs > best) best = gbs;
  }

  volatile unsigned char sink = dst[n / 2];
  (void)sink;
  if (best <= 0) return Num{};
  return N(best);
}

/* ================================================================ probe */
Report probe(uint32_t flags) {
  using clk = std::chrono::steady_clock;
  auto t0 = clk::now();

  Report r;
  r.level = (flags & DEVINFO_BENCH) ? "bench" : "fast";

  probe_environment(r);

  probe_cpu(r.cpu, r);
  derive_cpu_peaks(r.cpu);

  /* An unknown SoC is the normal case for brand-new hardware. Say so out
   * loud instead of shipping silently-empty fields. */
  if (r.cpu.vendor == "Apple" && !r.cpu.clock_ghz.has)
    r.warnings.push_back("SoC '" + r.cpu.model + "' is not in the spec table: clock, "
                         "bandwidth and peak throughput were omitted");

  probe_gpu(r);
  for (auto& g : r.gpus) {
    derive_gpu_peaks(g);
    if (g.vendor == "nvidia" && !g.fp32_tflops.has)
      r.warnings.push_back("GPU '" + g.name + "' has no matching architecture entry: "
                           "Tensor-Core throughput omitted");
  }

  probe_npu(r);
  collect_accels(r);

  if (flags & DEVINFO_BENCH) {
    uint64_t hint = r.cpu.mem_bytes.has ? (uint64_t)(r.cpu.mem_bytes.v / 128) : (64ull << 20);
    if (hint < (8ull << 20)) hint = 8ull << 20;
    if (hint > (64ull << 20)) hint = 64ull << 20;
    /* On a machine that is nearly out of memory the copy thrashes: every rep
     * pays page faults again. Stay well inside what is actually available. */
#if defined(__linux__)
    {
      std::string mi = read_small_file("/proc/meminfo", 4096);
      size_t at = mi.find("MemAvailable:");
      if (at != std::string::npos) {
        unsigned long long kb = std::strtoull(mi.c_str() + at + 13, nullptr, 10);
        unsigned long long spare = kb * 1024ull / 16ull;
        if (spare < (8ull << 20)) spare = 8ull << 20;
        if (hint > spare) hint = spare;
      }
    }
#endif
    if (hint > (64ull << 20)) hint = 64ull << 20;
    r.cpu.mem_bw_measured_gbs = measure_memory_bandwidth(hint, 0);
  }

  r.probe_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
  return r;
}

}  /* namespace devinfo */
