/* table.cpp - the builtin spec database.
 *
 * Some numbers simply do not exist behind any public API (Tensor Core counts,
 * NPU TOPS, Apple memory bandwidth, peak clocks on Apple Silicon).  Rather than
 * pretend otherwise we keep a small curated table and the emit layer marks
 * every value that came from here as "estimated": true.
 *
 * Figures below are vendor-published nominal specs.
 */
#include "devinfo_core.hpp"
#include <cstring>
#include <cstdlib>

namespace devinfo {

/* ----------------------------------------------------------------- App */
/* name, P-core GHz, E-core GHz, memory GB/s, ANE TOPS, ANE cores */
static const AppleChip kApple[] = {
  {"M1 Ultra",  3.22, 2.06, 800.0, 22.0, 32},
  {"M1 Max",    3.22, 2.06, 400.0, 11.0, 16},
  {"M1 Pro",    3.22, 2.06, 200.0, 11.0, 16},
  {"M1",        3.20, 2.06,  68.25, 11.0, 16},
  {"M2 Ultra",  3.49, 2.42, 800.0, 31.6, 32},
  {"M2 Max",    3.49, 2.42, 400.0, 15.8, 16},
  {"M2 Pro",    3.49, 2.42, 200.0, 15.8, 16},
  {"M2",        3.49, 2.42, 100.0, 15.8, 16},
  {"M3 Ultra",  4.05, 2.75, 800.0, 36.0, 32},
  {"M3 Max",    4.05, 2.75, 400.0, 18.0, 16},
  {"M3 Pro",    4.05, 2.75, 150.0, 18.0, 16},
  {"M3",        4.05, 2.75, 100.0, 18.0, 16},
  {"M4 Max",    4.51, 2.89, 546.0, 38.0, 16},
  {"M4 Pro",    4.51, 2.89, 273.0, 38.0, 16},
  {"M4",        4.46, 2.89, 120.0, 38.0, 16},
  {"M5 Max",    4.61, 3.00, 546.0, 40.0, 16},
  {"M5 Pro",    4.61, 3.00, 307.0, 40.0, 16},
  {"M5",        4.61, 3.00, 153.0, 40.0, 16},
  {"A18 Pro",   3.90, 2.10,  60.0, 35.0, 16},
  {"A18",       3.90, 2.10,  60.0, 35.0, 16},
};

const AppleChip* apple_chip(const std::string& soc) {
  /* longest match wins, so "M1 Ultra" is preferred over "M1" */
  const AppleChip* best = nullptr;
  size_t bestlen = 0;
  for (const auto& e : kApple) {
    if (soc.find(e.name) != std::string::npos) {
      size_t n = std::strlen(e.name);
      if (n > bestlen) { bestlen = n; best = &e; }
    }
  }
  return best;
}

/* GPU clock is not readable on Apple Silicon either. */
double apple_gpu_clock_ghz(const std::string& soc) {
  if (soc.find("M5") != std::string::npos) return 1.62;
  if (soc.find("M4") != std::string::npos) return 1.40;
  if (soc.find("M3") != std::string::npos) return 1.38;
  if (soc.find("M2") != std::string::npos) return 1.40;
  if (soc.find("M1") != std::string::npos) return 1.28;
  if (soc.find("A18") != std::string::npos) return 1.20;
  return 0.0;
}

/* ----------------------------------------------------------------- NVR */
/* cc, arch, fp32/SM, TC/SM, FP16 FLOPS per SM per clock (consumer),
 *                                          (datacenter)                     */
static const NvArch kNv[] = {
  {5, 0, "Maxwell",           128,  0,    0,    0},
  {5, 2, "Maxwell",           128,  0,    0,    0},
  {6, 0, "Pascal GP100",       64,  0,    0,    0},
  {6, 1, "Pascal",            128,  0,    0,    0},
  {6, 2, "Pascal",            128,  0,    0,    0},
  {7, 0, "Volta",              64,  8, 1024, 1024},
  {7, 2, "Volta",              64,  8, 1024, 1024},
  {7, 5, "Turing",             64,  8, 1024, 1024},
  {8, 0, "Ampere GA100",       64,  4, 1024, 2048},
  {8, 6, "Ampere GA10x",      128,  4, 1024, 1024},
  {8, 7, "Ampere Orin",       128,  4,  512,  512},
  {8, 9, "Ada Lovelace",      128,  4, 1024, 1024},
  {9, 0, "Hopper",            128,  4, 4096, 4096},
  {10, 0, "Blackwell",        128,  4, 4096, 4096},
  {12, 0, "Blackwell",        128,  4, 4096, 4096},
};

const NvArch* nv_arch(unsigned major, unsigned minor) {
  for (const auto& e : kNv)
    if (e.major == major && e.minor == minor) return &e;
  /* unknown minor: fall back to the closest known minor of the same major */
  const NvArch* best = nullptr;
  for (const auto& e : kNv)
    if (e.major == major && e.minor <= minor) best = &e;
  if (!best)
    for (const auto& e : kNv) if (e.major == major) best = &e;
  return best;
}

}  /* namespace devinfo */
