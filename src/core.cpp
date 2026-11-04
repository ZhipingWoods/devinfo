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

#if !defined(_WIN32)
#include <dirent.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <sys/mount.h>
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

/* Names only, no stat of each entry: enough to spot versioned install dirs. */
std::vector<std::string> list_dir(const std::string& dir) {
  std::vector<std::string> out;
#if defined(_WIN32)
  std::string pat = dir + "\\*";
  WIN32_FIND_DATAA fd{};
  HANDLE h = FindFirstFileA(pat.c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return out;
  do {
    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
    if (fd.cFileName[0] == '.') continue;
    out.push_back(fd.cFileName);
  } while (FindNextFileA(h, &fd));
  FindClose(h);
#else
  void* d = nullptr;
  DIR* dp = nullptr;
  (void)d;
  dp = opendir(dir.c_str());
  if (!dp) return out;
  while (struct dirent* e = readdir(dp)) {
    if (e->d_name[0] == '.') continue;
    out.push_back(e->d_name);
  }
  closedir(dp);
#endif
  return out;
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

  /* -------- compile toolchains: stat() and read files, never exec -------- */
  {
    struct Tool { const char* name; const char* path; };
    static const Tool kTools[] = {
#if defined(_WIN32)
      {"hipcc", "C:\\Program Files\\AMD\\ROCm\\bin\\hipcc.exe"},
      {"npu-smi", "C:\\Program Files\\Huawei\\Ascend\\tools\\npu-smi.exe"},
#else
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

  /* -------- CUDA toolkit: the version that actually decides nvcc ---- */
  {
    static const char* kRoots[] = {
#if defined(_WIN32)
      "C:\\Program Files\\NVIDIA GPU Computing Toolkit\\CUDA",
#else
      "/usr/local",
#endif
      nullptr,
    };
    for (int i = 0; kRoots[i]; ++i) {
      for (const auto& name : list_dir(kRoots[i])) {
#if defined(_WIN32)
        if (name.size() < 2 || name[0] != 'v') continue;
        std::string dir = std::string(kRoots[i]) + "\\" + name;
        std::string nvcc = dir + "\\bin\\nvcc.exe";
        if (!file_exists(nvcc.c_str())) continue;
        add_accel(r, Accel{"cuda_toolkit", "toolkit", true, name.substr(1), dir});
#else
        if (name.compare(0, 4, "cuda") != 0) continue;
        std::string dir = std::string(kRoots[i]) + "/" + name;
        std::string nvcc = dir + "/bin/nvcc";
        if (!file_exists(nvcc.c_str())) continue;
        /* version.json is authoritative; fall back to the directory suffix. */
        std::string ver;
        std::string js = read_small_file((dir + "/version.json").c_str(), 4096);
        size_t vp = js.find("\"version\"");
        if (vp != std::string::npos) {
          size_t q1 = js.find('"', js.find(':', vp));
          size_t q2 = (q1 == std::string::npos) ? std::string::npos : js.find('"', q1 + 1);
          if (q2 != std::string::npos) ver = js.substr(q1 + 1, q2 - q1 - 1);
        }
        if (!ver.empty()) {
          /* version.json carries the build string ("13.0.20250728"); the
           * toolkit version people quote is the first two fields. */
          size_t d1 = ver.find('.');
          if (d1 != std::string::npos) {
            size_t d2 = ver.find('.', d1 + 1);
            if (d2 != std::string::npos) ver = ver.substr(0, d2);
          }
        }
        if (ver.empty() && name.size() > 4 && name[4] == '-') ver = name.substr(5);
        /* /usr/local/cuda is a symlink to /usr/local/cuda-13.0, so the same
         * toolkit would otherwise be reported twice. */
        bool dup = false;
        for (const auto& a : r.accels)
          if (a.name == "cuda_toolkit" && a.version == ver) { dup = true; break; }
        if (!dup) add_accel(r, Accel{"cuda_toolkit", "toolkit", true, ver, dir});
#endif
      }
    }
  }

  /* -------- python frameworks: read site-packages, never import ------ */
  /* torch/version.py is a plain module of assignments. Reading it beats
   * spawning an interpreter, and works on a box where python is not on PATH. */
  {
    static const char* kSites[] = {
#if defined(_WIN32)
      nullptr,   /* enumerated per-interpreter below */
#else
      "/usr/lib/python3/dist-packages",
      "/usr/local/lib/python3.12/dist-packages",
      "/usr/local/lib/python3.11/dist-packages",
      "/usr/local/lib/python3.10/dist-packages",
      "/usr/local/lib/python3.9/dist-packages",
#endif
      nullptr,
    };
    std::vector<std::string> roots;
    for (int i = 0; kSites[i]; ++i) roots.push_back(kSites[i]);

#if !defined(_WIN32)
    /* Python lives in too many places to hardcode: system, home, conda,
     * and virtualenvs. Enumerate the parents instead of guessing layouts. */
    {
      std::vector<std::string> bases;
      if (const char* home = std::getenv("HOME")) {
        bases.push_back(std::string(home) + "/.local/lib");
      }
      bases.push_back("/usr/lib");
      bases.push_back("/usr/local/lib");
      bases.push_back("/opt/conda/lib");
      bases.push_back("/opt/miniconda3/lib");
      for (const auto& b : bases) {
        for (const auto& py : list_dir(b)) {
          if (py.compare(0, 7, "python3") != 0) continue;
          roots.push_back(b + "/" + py + "/site-packages");
          roots.push_back(b + "/" + py + "/dist-packages");
        }
      }
    }
#endif

#if defined(_WIN32)
    for (const char* base : {"C:\\Python312", "C:\\Python311", "C:\\Python310",
                             "C:\\ProgramData\\miniconda3", "C:\\ProgramData\\anaconda3",
                             "C:\\Users\\Public\\miniconda3", "C:\\Users\\Public\\anaconda3"}) {
      std::string sp = std::string(base) + "\\Lib\\site-packages";
      if (file_exists((sp + "\\torch\\version.py").c_str())) roots.push_back(sp);
      std::string lsp = std::string(base) + "\\Lib\\site-packages";
      (void)lsp;
      std::string en = std::string(base) + "\\envs";
      for (const auto& env : list_dir(en)) {
        std::string ep = en + "\\" + env + "\\Lib\\site-packages";
        if (file_exists((ep + "\\torch\\version.py").c_str())) roots.push_back(ep);
      }
    }
#endif

#if defined(_WIN32)
    const char kSep = '\\';
#else
    const char kSep = '/';
#endif
    for (const auto& sp : roots) {
      const std::string path = sp + kSep + "torch" + kSep + "version.py";
      if (!file_exists(path.c_str())) continue;
      std::string txt = read_small_file(path.c_str(), 8192);
      if (txt.empty()) continue;
      /* Anchor on the start of a line: __all__ lists the same names as quoted
       * strings, so a plain find() lands inside the list. Both quote styles
       * occur in the wild - 2.8.0 writes '...' where 2.5 wrote "...". */
      auto grab = [&txt](const char* key) -> std::string {
        const std::string anchored = std::string("\n") + key;
        size_t p = txt.find(anchored);
        if (p == std::string::npos) {
          if (txt.compare(0, std::strlen(key), key) != 0) return std::string();
          p = 0;
        }
        size_t q = txt.find_first_of("'\"", p);
        if (q == std::string::npos) return std::string();
        const char quote = txt[q];
        size_t e = txt.find(quote, q + 1);
        if (e == std::string::npos) return std::string();
        return txt.substr(q + 1, e - q - 1);
      };
      const std::string ver = grab("__version__");
      const std::string cu = grab("cuda");
      if (ver.empty()) continue;
      std::string detail = sp;
      if (!cu.empty()) detail += " cuda=" + cu;
      add_accel(r, Accel{"pytorch", "framework", true, ver, detail});
      break;
    }
  }

  /* -------- does the installed runtime outrun the driver? ------------ */
  /* The single most common reason a compiled extension refuses to load. */
  {
    const Accel* drv = nullptr;
    const Accel* rt = nullptr;
    for (const auto& a : r.accels) {
      if (a.name == "cuda") drv = &a;
      else if (a.name == "cuda_runtime") rt = &a;
    }
    if (drv && rt && drv->available && rt->available &&
        !drv->version.empty() && !rt->version.empty()) {
      auto major = [](const std::string& v) {
        return (unsigned)std::strtoul(v.c_str(), nullptr, 10);
      };
      if (major(rt->version) > major(drv->version))
        r.warnings.push_back("cuda runtime " + rt->version +
                             " is newer than the driver supports (" +
                             drv->version + "); kernels built against it will not load");
    }
  }
}

/* ================================================================ limits */
/* Windows and Linux override this in their own translation units; elsewhere
 * there is nothing to ask. macOS containers are full VMs, so the machine the
 * process sees is the machine it gets. */
#if defined(__APPLE__)
void probe_storage(Report& r) {
  struct statfs sf {};
  if (::statfs("/", &sf) != 0) return;
  r.storage.mount = "/";
  r.storage.total_bytes = Num{(double)sf.f_blocks * (double)sf.f_bsize, true, false};
  r.storage.free_bytes = Num{(double)sf.f_bavail * (double)sf.f_bsize, true, false};
  r.storage.media = (sf.f_flags & MNT_LOCAL) ? "ssd" : "network";
}

/* macOS link aggregation makes "the" interface ambiguous, and multi-node
 * training on Macs is not a real workflow. Left unset rather than guessed. */
void probe_network(Report& r) { (void)r; }

void probe_limits(Report& r) { (void)r; }

/* Apple Silicon is a single NUMA domain: the CPU and GPU share one memory
 * pool, and there is no second socket to be remote from. */
void probe_numa(Report& r) { (void)r; }
#endif

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

      /* Every Tensor Core generation since Ampere runs INT8 at twice the FP16
       * rate; FP8 arrived with Ada and Hopper at the same 2x. These are
       * architecture ratios, not datasheet lookups, so they stay marked
       * estimated. Ampere has no FP8 at all and is left unset. */
      g.int8_tops = E(g.fp16_tflops.v * 2.0);
      if (maj >= 9 || (maj == 8 && minr == 9))
        g.fp8_tflops = E(g.fp16_tflops.v * 2.0);
    }
  }
}

/* ============================================================== deploy */
/* Turns the measurements into the four things someone about to deploy a model
 * actually asks: how big, at what precision, split how, and what will bind it.
 * Every line names its rule; none of this is read from hardware. */
void derive_deploy(Report& r) {
  Deploy& d = r.deploy;

  /* ---- memory ceiling ---- */
  double vram = 0;
  unsigned gpu_count = 0;
  for (const auto& g : r.gpus) {
    if (!g.vram_bytes.has) continue;
    vram += g.vram_bytes.v;
    ++gpu_count;
  }
  double mem_gib = 0;
  bool unified = false;
  for (const auto& g : r.gpus)
    if (g.unified) unified = true;

  if (gpu_count > 0 && !unified) {
    mem_gib = vram / (1024.0 * 1024 * 1024);
  } else if (r.cpu.mem_bytes.has) {
    /* Unified memory is the system pool; summing GPU and RAM would count the
     * same bytes twice. */
    mem_gib = r.cpu.mem_bytes.v / (1024.0 * 1024 * 1024);
    if (unified && gpu_count > 0) unified = true;
  } else if (gpu_count > 0) {
    mem_gib = vram / (1024.0 * 1024 * 1024);
  }
  if (mem_gib > 0) {
    /* Bytes per parameter, the only part of this that is a judgement:
     * inference at a given dtype is just the weights, so 2 bytes for fp16;
     * training with Adam holds weights, gradients and two moments, which is
     * 16 bytes at fp32 and about the same in mixed precision once the fp32
     * master copy is added. No allowance for activations or KV cache, both
     * of which scale with batch and sequence length and neither of which this
     * tool knows. */
    char buf[320];
    std::snprintf(buf, sizeof buf,
                  "memory %.1f GiB%s -> inference ~%.0fb params int4, ~%.0fb int8, "
                  "~%.0fb fp16; training ~%.0fb (16 B/param, Adam, no activations)",
                  mem_gib,
                  unified ? (gpu_count ? " of unified memory shared with the GPU"
                                       : " of system RAM")
                          : (gpu_count ? " over all GPUs" : " of system RAM"),
                  mem_gib * 1024.0 / 0.5 / 1000.0, mem_gib * 1024.0 / 1.0 / 1000.0,
                  mem_gib * 1024.0 / 2.0 / 1000.0, mem_gib * 1024.0 / 16.0 / 1000.0);
    d.lines.push_back(buf);
  }

  /* ---- precision ---- */
  {
    std::vector<std::string> p;
    bool tc = false;
    for (const auto& g : r.gpus) {
      if (!g.tensor_cores.has) continue;
      tc = true;
      if (g.fp8_tflops.has) p.push_back("fp8");
      if (g.bf16_tflops.has) p.push_back("bf16");
      if (g.fp16_tflops.has) p.push_back("fp16");
      if (g.int8_tops.has) p.push_back("int8");
      break;
    }
    if (!tc && has_feature(r.cpu, "neon")) p.push_back("fp16");
    if (p.empty() && has_feature(r.cpu, "avx512bf16")) p.push_back("bf16");
    if (!p.empty())
      d.lines.push_back(std::string("precision ") +
                        (tc ? "tensor cores: " : "vector units: ") +
                        [&p] {
                          std::string s;
                          for (const auto& x : p) {
                            if (!s.empty()) s += ' ';
                            s += x;
                          }
                          return s;
                        }() +
                        "  (from the hardware's own capability bits)");
  }

  /* ---- parallelism ---- */
  {
    if (gpu_count == 1) {
      d.lines.push_back("parallel single GPU, no cross-device link in the path");
    } else if (gpu_count > 1) {
      /* Width is not power-managed, so it is a usable signal for whether the
       * cards share a fast backplane. */
      unsigned narrow = 0, linked = 0;
      for (const auto& g : r.gpus) {
        if (!g.pcie_lanes_max) continue;
        ++linked;
        if (g.pcie_lanes_max < 16) ++narrow;
      }
      std::string s = std::to_string(gpu_count) + " GPUs";
      if (linked == gpu_count && narrow == 0)
        s += ", all x16: tensor parallel fits within PCIe";
      else if (narrow > 0)
        s += ", " + std::to_string(narrow) + " on a narrow link: prefer data or "
             "pipeline parallel over tensor parallel across those";
      d.lines.push_back("parallel " + s);
    }
  }

  /* ---- what binds it ---- */
  {
    const Gpu* g = nullptr;
    for (const auto& x : r.gpus)
      if (x.fp32_tflops.has && x.mem_bw_gbs.has) { g = &x; break; }
    if (g) {
      const double ridge = g->fp32_tflops.v * 1e12 / (g->mem_bw_gbs.v * 1e9);
      char buf[256];
      std::snprintf(buf, sizeof buf,
                    "ridge point %.0f FLOP/byte (%.1f TFLOPS / %.0f GB/s): "
                    "kernels below it are bandwidth-bound",
                    ridge, g->fp32_tflops.v, g->mem_bw_gbs.v);
      d.lines.push_back(buf);
    } else if (r.cpu.peak_fp32_tflops.has && r.cpu.mem_bw_gbs.has) {
      const double ridge = r.cpu.peak_fp32_tflops.v * 1e12 / (r.cpu.mem_bw_gbs.v * 1e9);
      char buf[256];
      std::snprintf(buf, sizeof buf,
                    "ridge point %.0f FLOP/byte (%.1f TFLOPS / %.0f GB/s), CPU only",
                    ridge, r.cpu.peak_fp32_tflops.v, r.cpu.mem_bw_gbs.v);
      d.lines.push_back(buf);
    }
  }

  /* ---- does the disk fit the checkpoint ---- */
  if (r.storage.free_bytes.has && mem_gib > 0) {
    const double free_gib = r.storage.free_bytes.v / (1024.0 * 1024 * 1024);
    if (free_gib < mem_gib * 2.0) {
      char buf[192];
      std::snprintf(buf, sizeof buf,
                    "disk %.1f GiB free vs %.1f GiB of memory: a half-precision "
                    "checkpoint needs about %.1f GiB to stage, so space is tight",
                    free_gib, mem_gib, mem_gib);
      d.lines.push_back(buf);
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

  /* Before anything is derived from the core count: inside a container the
   * number CPUID reports is the host's, not the schedulable one. */
  probe_limits(r);
  probe_numa(r);
  probe_storage(r);
  probe_network(r);

  collect_accels(r);

  /* Last, so it can read everything above. */
  derive_deploy(r);

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
