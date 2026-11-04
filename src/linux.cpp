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
#if defined(__aarch64__) && defined(__linux__)
#include <sys/prctl.h>   /* SVE vector length */
#endif

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

/* ---- PCIe link state: a card can train below its slot and its own spec ---- */
unsigned speed_to_gen(const std::string& s) {
  /* 2.5 GT/s is gen1, 5 gen2, 8 gen3, 16 gen4, 32 gen5, 64 gen6. */
  double v = std::atof(s.c_str());
  if (v <= 2.5) return 1;
  if (v <= 5.0) return 2;
  if (v <= 8.0) return 3;
  if (v <= 16.0) return 4;
  if (v <= 32.0) return 5;
  return 6;
}

void read_pcie_link(const std::string& devdir, Gpu& g) {
  std::string dir = devdir;
  if (!dir.empty() && dir.back() != '/') dir += '/';
  std::string cs = one_line(dir + "current_link_speed");
  std::string cw = one_line(dir + "current_link_width");
  std::string ms = one_line(dir + "max_link_speed");
  std::string mw = one_line(dir + "max_link_width");
  if (!cw.empty()) g.pcie_lanes = (unsigned)std::strtoul(cw.c_str(), nullptr, 10);
  if (!cs.empty()) g.pcie_gen = speed_to_gen(cs);
  if (!mw.empty()) g.pcie_lanes_max = (unsigned)std::strtoul(mw.c_str(), nullptr, 10);
  if (!ms.empty()) g.pcie_gen_max = speed_to_gen(ms);
}

/* ---- NVIDIA via libcuda: the only way to get SM count without a context */
typedef int (*cuInit_t)(unsigned);
typedef int (*cuDeviceGetCount_t)(int*);
typedef int (*cuDeviceGet_t)(int*, int);
typedef int (*cuDeviceGetName_t)(char*, int, int);
typedef int (*cuDeviceGetAttribute_t)(int*, int, int);
typedef int (*cuDeviceTotalMem_t)(unsigned long long*, int);
typedef int (*cuDeviceGetPCIBusId_t)(char*, int, int);

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
  auto pciid_ = (cuDeviceGetPCIBusId_t)dlsym(h, "cuDeviceGetPCIBusId");
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

    /* The bus id lets us ask the kernel what the link actually trained to,
     * which libcuda itself will not say. */
    if (pciid_) {
      char busid[32] = {0};
      if (pciid_(busid, (int)sizeof busid, dev) == 0 && busid[0]) {
        std::string s(busid);
        for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
        g.pci_id = s;
        read_pcie_link("/sys/bus/pci/devices/" + s, g);
      }
    }
    r.gpus.push_back(std::move(g));
  }
  dlclose(h);
}

/* -------------------------------------------------------------- limits */
/* Inside a container CPUID and /proc/meminfo describe the host. Scheduling
 * against the host's 96 cores when cgroup allows 4 costs the difference
 * between a fast build and a slow one, and every thread pool oversubscribes
 * by the same factor. Read the ceiling the kernel actually enforces. */
void probe_limits_impl(Report& r) {
  Limits& L = r.limits;

  auto first_tok = [](const char* p) {
    std::string s = read_file(p, 256);
    size_t sp = s.find_first_of(" \n");
    return (sp == std::string::npos) ? s : s.substr(0, sp);
  };

  double cores = 0.0;
  {
    std::string quota_s = read_file("/sys/fs/cgroup/cpu.max", 128);
    if (!quota_s.empty()) {
      L.cgroup = "v2";
      if (quota_s.compare(0, 3, "max") != 0) {
        double q = std::atof(quota_s.c_str());
        const char* sp = std::strchr(quota_s.c_str(), ' ');
        double period = sp ? std::atof(sp + 1) : 100000.0;
        if (q > 0 && period > 0) cores = q / period;
      }
    } else if (::access("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", F_OK) == 0) {
      L.cgroup = "v1";
      double q = std::atof(read_file("/sys/fs/cgroup/cpu/cpu.cfs_quota_us", 64).c_str());
      double period = std::atof(read_file("/sys/fs/cgroup/cpu/cpu.cfs_period_us", 64).c_str());
      if (q > 0 && period > 0) cores = q / period;
    }
  }

  /* cpuset is a hard affinity mask and can be tighter than the quota */
  {
    const char* kCpuset[] = {
      "/sys/fs/cgroup/cpuset.cpus.effective",   /* v2 */
      "/sys/fs/cgroup/cpuset/cpuset.cpus",      /* v1 */
      nullptr,
    };
    for (int i = 0; kCpuset[i]; ++i) {
      std::string s = read_file(kCpuset[i], 4096);
      if (s.empty()) continue;
      unsigned n = 0;
      size_t p = 0;
      while (p < s.size()) {
        size_t comma = s.find(',', p);
        std::string tok = s.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
        size_t dash = tok.find('-');
        if (dash != std::string::npos) {
          unsigned a = (unsigned)std::strtoul(tok.c_str(), nullptr, 10);
          unsigned b = (unsigned)std::strtoul(tok.c_str() + dash + 1, nullptr, 10);
          if (b >= a) n += b - a + 1;
        } else if (!tok.empty()) {
          ++n;
        }
        if (comma == std::string::npos) break;
        p = comma + 1;
      }
      if (n > 0 && (cores <= 0 || (double)n < cores)) cores = (double)n;
      break;
    }
  }
  if (cores > 0 && cores < (double)r.cpu.logical)
    L.cpu_cores = Num{cores, true, false};

  {
    double limit = 0.0;
    std::string v2 = first_tok("/sys/fs/cgroup/memory.max");
    if (!v2.empty() && v2 != "max") limit = std::atof(v2.c_str());
    if (limit <= 0) {
      std::string v1 = first_tok("/sys/fs/cgroup/memory/memory.limit_in_bytes");
      if (!v1.empty()) {
        double d = std::atof(v1.c_str());
        /* v1 writes a near-SIZE_MAX number for "unlimited", not -1 */
        if (d > 0 && d < 9.0e18) limit = d;
      }
    }
    if (limit > 0 && (!r.cpu.mem_bytes.has || limit < r.cpu.mem_bytes.v))
      L.mem_bytes = Num{limit, true, false};
  }

  if (L.cpu_cores.has || L.mem_bytes.has) {
    L.container = true;
    if (L.cpu_cores.has) {
      /* 2.5 cores is a legal quota; truncating it to 2 would understate what
       * the process may use, which is the opposite of the point. */
      char buf[64];
      std::snprintf(buf, sizeof buf, "%.2f", L.cpu_cores.v);
      std::string n(buf);
      while (!n.empty() && n.back() == '0') n.pop_back();
      if (!n.empty() && n.back() == '.') n.pop_back();
      r.warnings.push_back("cgroup limits this process to " + n + " of " +
                           std::to_string(r.cpu.logical) + " logical CPUs");
    }
    if (L.mem_bytes.has)
      r.warnings.push_back("cgroup limits memory to " +
                           std::to_string((long long)(L.mem_bytes.v / (1024.0 * 1024 * 1024))) +
                           " GiB of " +
                           std::to_string((long long)(r.cpu.mem_bytes.v / (1024.0 * 1024 * 1024))) +
                           " GiB");
  }
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

/* ============================================================== limits */
void probe_limits(Report& r) { probe_limits_impl(r); }

/* ================================================================ CPU */
void probe_cpu(Cpu& c, Report& r) {
  CpuInfo ci;
  parse_cpuinfo(ci);

  c.arch = r.arch;
  c.model = ci.model;
  c.features = ci.features;

#if defined(__linux__) && defined(__aarch64__)
  /* On ARM the ISA flag list is not enough: SVE kernels are written against a
   * specific vector width, and it is 128 bits on one part and 256 on the next.
   * The kernel reports the width the thread would actually get. */
  {
    long vl = prctl(PR_SVE_GET_VL);
    if (vl > 0) {
      unsigned bits = (unsigned)(vl & PR_SVE_VL_LEN_MASK) * 8u;
      if (bits >= 128) c.features.push_back("SVE_VL=" + std::to_string(bits));
    }
  }
#endif
#if defined(__linux__) && defined(__aarch64__)
  {
    /* SME uses a different vector register file; report its width too. */
    std::string svl = read_file("/proc/sys/abi/sme_default_vector_length", 64);
    if (!svl.empty()) {
      long bits = std::atol(svl.c_str());
      if (bits >= 128) c.features.push_back("SME_VL=" + std::to_string(bits));
    }
  }
#endif

  /* The raw kernel list carries cpu-vendor trivia; keep what a runtime uses. */
  canonical_features(c.features, c.arch);

  /* Turbo ceiling. The brand string only ever gives the base clock, and on a
   * core-limited part the gap between the two is most of the performance. */
  {
    static const char* kMax[] = {
      "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq",
      "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
    };
    for (const char* p : kMax) {
      std::string v = read_file(p, 64);
      if (v.empty()) continue;
      long khz = std::atol(v.c_str());
      if (khz > 0) { c.clock_max_ghz = Num{(double)khz / 1e6, true, false}; break; }
    }
  }

  /* DIMM speed and population. The DMI table is root-only on most distros, so
   * this is best-effort and stays unset rather than guessed when unreadable. */
  {
    std::string dmi = read_file("/sys/firmware/dmi/tables/DMI", 1 << 20);
    if (!dmi.empty()) {
      unsigned modules = 0, best_configured = 0, best_rated = 0;
      size_t off = 0;
      const size_t n = dmi.size();
      while (off + 4 <= n) {
        const unsigned char type = (unsigned char)dmi[off];
        const unsigned char len = (unsigned char)dmi[off + 1];
        if (len < 4) break;
        if (type == 0x7F) break;
        if (type == 17 && off + len <= n) {
          auto u16 = [&](size_t at) -> unsigned {
            if (at + 1 >= len) return 0;
            return (unsigned)(unsigned char)dmi[off + at] |
                   ((unsigned)(unsigned char)dmi[off + at + 1] << 8);
          };
          const unsigned msize = u16(0x08);
          if (msize != 0 && msize != 0xFFFF) {
            ++modules;
            const unsigned rated = u16(0x15);
            const unsigned configured = u16(0x20);
            if (rated > best_rated) best_rated = rated;
            if (configured > best_configured) best_configured = configured;
          }
        }
        size_t p = off + len;
        while (p + 1 < n && !(dmi[p] == 0 && dmi[p + 1] == 0)) ++p;
        off = p + 2;
      }
      c.mem_modules = modules;
      const unsigned speed = best_configured ? best_configured : best_rated;
      if (speed) c.mem_speed_mts = Num{(double)speed, true, false};
    }
  }

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

/* ================================================================ NUMA */
void probe_numa(Report& r) {
  std::vector<std::string> nodes;
  for (int i = 0; i < 64; ++i) {
    std::string d = "/sys/devices/system/node/node" + std::to_string(i);
    if (::access((d + "/cpulist").c_str(), R_OK) != 0) continue;
    nodes.push_back(d);
  }
  if (nodes.empty()) return;
  r.numa.nodes = (unsigned)nodes.size();

  /* Count CPUs per node from cpulist ("0-47" or "0-23,48-71"). */
  std::string cols;
  for (const auto& d : nodes) {
    std::string list = one_line(d + "/cpulist");
    unsigned n = 0;
    size_t p = 0;
    while (p < list.size()) {
      size_t comma = list.find(',', p);
      std::string tok = list.substr(p, comma == std::string::npos ? std::string::npos : comma - p);
      size_t dash = tok.find('-');
      if (dash != std::string::npos) {
        unsigned a = (unsigned)std::strtoul(tok.c_str(), nullptr, 10);
        unsigned b = (unsigned)std::strtoul(tok.c_str() + dash + 1, nullptr, 10);
        if (b >= a) n += b - a + 1;
      } else if (!tok.empty()) {
        ++n;
      }
      if (comma == std::string::npos) break;
      p = comma + 1;
    }
    if (!cols.empty()) cols += "+";
    cols += std::to_string(n);
  }
  r.numa.layout = std::to_string(r.numa.nodes) + " nodes: " + cols + " cores";
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
    read_pcie_link(d.path, g);
    r.gpus.push_back(std::move(g));
  }
}

/* ================================================================ NPU */
void probe_npu(Report& r) {
  probe_ascend(r);
  probe_accel_nodes(r);
}

}  /* namespace devinfo */
