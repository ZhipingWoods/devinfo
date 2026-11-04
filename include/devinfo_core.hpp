/* devinfo_core.hpp - internal C++ model. Not part of the ABI. */
#ifndef DEVINFO_CORE_HPP
#define DEVINFO_CORE_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace devinfo {

/* A number plus where it came from, so nobody mistakes a guess for a fact. */
struct Num {
  double v = 0.0;
  bool   has = false;
  bool   estimated = false;
};

struct Cpu {
  std::string vendor, model, arch;
  unsigned physical = 0, logical = 0;
  unsigned p_cores = 0, e_cores = 0;
  std::string p_name, e_name;
  Num clock_ghz;                       /* WARNING: unreadable on Apple Silicon */
  unsigned l1i = 0, l1d = 0, l2 = 0, l3 = 0, line = 0;  /* bytes, totals */
  Num mem_bytes;
  Num mem_bw_gbs;                      /* theoretical */
  Num mem_bw_measured_gbs;             /* DEVINFO_BENCH only */
  Num peak_fp32_tflops, peak_fp64_tflops, peak_int8_tops;
  std::vector<std::string> features;
};

struct Gpu {
  std::string vendor, name, arch;
  std::string core_unit = "SM";        /* SM / CU / GPU core */
  Num cores;                           /* SM / CU / GPU core count */
  Num tensor_cores;
  unsigned tc_per_core = 0;
  Num vram_bytes;
  bool unified = false;
  Num clock_ghz;
  Num mem_bw_gbs;
  Num fp64_tflops, fp32_tflops, tf32_tflops, bf16_tflops,
      fp16_tflops, fp8_tflops, int8_tops, fp16_tflops_sparse;
  std::string method;
  std::string pci_id;                  /* "8086:4c8a", when the OS tells us */
};

struct Npu {
  std::string vendor, name, arch;
  Num cores;
  Num int8_tops, fp16_tflops;
  std::string method;
};

struct Accel {
  std::string name;      /* cuda / cudnn / tensorrt / cann / rocm / metal ... */
  std::string category;  /* runtime / driver / lib / toolkit */
  bool available = false;
  std::string version;
  std::string detail;
};

struct Report {
  std::string os, os_version, kernel, arch, model_id, env, host;
  std::string level;
  double probe_ms = 0.0;

  Cpu cpu;
  std::vector<Gpu> gpus;
  std::vector<Npu> npus;
  std::vector<Accel> accels;
  std::vector<std::string> warnings;
};

/* ---- implemented once per platform (src/apple.cpp / linux.cpp / win32.cpp) */
void probe_cpu(Cpu& c, Report& r);
void probe_gpu(Report& r);
void probe_npu(Report& r);

/* ---- shared across all platforms */
void collect_accels(Report& r);        /* dlopen/LoadLibrary scanning */
void probe_environment(Report& r);
void canonical_features(std::vector<std::string>& feats, const std::string& arch);
void derive_cpu_peaks(Cpu& c);
void derive_gpu_peaks(Gpu& g);
Num  measure_memory_bandwidth(uint64_t bytes_hint, unsigned threads_hint);

Report probe(uint32_t flags);

/* ---- emit.cpp */
std::string to_json(const Report& r);
std::string to_table(const Report& r);

/* ---- table.cpp : builtin specs for things no API will tell us ---- */
struct AppleChip {
  const char* name;      /* longest-substring match against the SoC name */
  double p_ghz;
  double e_ghz;
  double bw_gbs;
  double ane_tops;
  unsigned ane_cores;
};
const AppleChip* apple_chip(const std::string& soc);
double apple_gpu_clock_ghz(const std::string& soc);

struct NvArch {
  unsigned major, minor;
  const char* arch;
  unsigned fp32_per_sm;      /* FP32 lanes per SM */
  unsigned tc_per_sm;        /* Tensor Cores per SM */
  unsigned tc_flops_c;       /* FP16 dense FLOPS/SM/clock, consumer parts */
  unsigned tc_flops_dc;      /* ... datacenter parts (A100/H100/B200) */
};
const NvArch* nv_arch(unsigned major, unsigned minor);

}  /* namespace devinfo */
#endif
