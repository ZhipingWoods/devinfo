# devinfo

One binary. Any machine. Answers: **what silicon is in this box, how fast is it,
how many cores, how much bandwidth.**

C++17, zero third-party dependencies, no subprocesses.

`~
$ devinfo
devinfo - 1.94 ms  (fast)
macos 26.6.2 | arm64 | native
------------------------------------------------------------------
CPU  Apple M5
     10 cores (4P + 6E)            clock 4.610 GHz~  arm64
     L1i 1.5 MiB  L1d 896 KiB  L2 22.0 MiB  line 128B
     RAM 16.0 GiB  bw 153.0 GB/s~
     peak FP32 0.878 TFLOPS~
     isa NEON FP16 AES PMULL SHA1 SHA256 SHA512 SHA3 CRC32 DotProd I8MM BF16...
GPU0 Apple M5
     8 GPU core(s)
     unified 16.0 GiB  clock 1.620 GHz~
     peak FP32 3.318 TF~  FP16 3.318 TF~
NPU0 Apple Neural Engine (ANE)  16 cores  40.00 TOPS~
------------------------------------------------------------------
accelerators
     opencl                      runtime
     metal                       runtime
     -- not present: cuda --
     ~ = estimated from chip spec table, not read from hardware
`~

## Why it is fast

| rule | consequence |
|---|---|
| never spawn a process | `nvidia-smi`, `npu-smi`, `lspci`, `wmic` are excluded by design; each costs 200-800 ms and needs a PATH |
| never link a vendor SDK | CUDA / NVML / cuDNN / TensorRT / NCCL / ROCm / CANN / OpenCL are resolved with `dlopen` / `LoadLibrary` at run time. A box without a driver is a normal box, not a crash |
| never create a device context | CUDA is asked only for `cuDeviceGetAttribute`; no context, no Metal device, no D3D12 device |
| one pass, no polling | this is a snapshot, not a monitor |

Typical run: **2-7 ms**. Binary: **~110 KB**.

## Build

`
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
`

Produces three artifacts:

| target | artifact | use |
|---|---|---|
| `devinfo_core` | static library | embed the C++ model directly |
| `devinfo` | `devinfo.dll` / `libdevinfo.dylib` / `libdevinfo.so` | the plugin, a C ABI |
| `devinfo-cli` | `devinfo` | the command line |

## CLI

`
devinfo            # table on a tty, JSON when piped
devinfo --json     # force JSON
devinfo --table    # force the table
devinfo --bench    # add a memory bandwidth micro-benchmark
devinfo --version
`

## The plugin ABI

Four functions. The JSON document is the contract, so the header never has to
change:

`c
#include "devinfo.h"

char* j = devinfo_probe_json(DEVINFO_FAST);   /* or DEVINFO_BENCH */
puts(j);
devinfo_free(j);                              /* not free(): on Windows they differ */
`

From Python, no bindings required:

`python
import ctypes, json
lib = ctypes.CDLL("./build/libdevinfo.dylib")
lib.devinfo_probe_json.restype = ctypes.c_void_p
lib.devinfo_free.argtypes = [ctypes.c_void_p]
p = lib.devinfo_probe_json(0)
print(json.dumps(json.loads(ctypes.string_at(p)), indent=2))
lib.devinfo_free(p)
`

## Output schema

`
devinfo/1
  level          "fast" | "bench"
  probe_ms       wall time of the probe itself
  host           os, os_version, kernel, arch, model, env, hostname
                   env = native | container | vm | wsl
  cpu            vendor, model, arch
                 cores{physical, logical, p, e, p_tier, e_tier}
                 clock_ghz, cache_bytes{l1i,l1d,l2,l3,line}
                 memory{total_bytes, bandwidth_gbs, bandwidth_measured_gbs}
                 peak{fp32_tflops, fp64_tflops, int8_tops}
                 features[]           NEON / AVX512F / I8MM / SME ...
                 estimated[]          which of the above are table guesses
  gpus[]         vendor, name, arch, core_unit (SM/CU/GPU core/EU)
                 cores, tensor_cores, tensor_cores_per_SM
                 clock_ghz, vram_bytes, unified_memory, mem_bandwidth_gbs
                 peak{fp64,fp32,tf32,bf16,fp16,fp16_sparse,fp8,int8}
                 method, estimated[]
  npus[]         vendor, name, arch, cores, peak{int8_tops,fp16_tflops}
  accelerators{} cuda, cuda_runtime, nvml, cudnn, tensorrt, nccl, rocm,
                 cann, openvino, onnxruntime, directml, vulkan, opencl,
                 sycl, metal, nvcc, hipcc, npu-smi
                 -> {available, version?, category?, detail?}
  warnings[]     anything the probe could not resolve
`

### estimated

The single most important field. Vendors do not publish Tensor Core counts or
NPU TOPS through any API, so those are **derived**:

`
cores        read from hardware (IOKit / DXGI / CUDA / sysfs)
clock        read when available, table otherwise
fp32 peak    cores x lanes-per-core x 2 x clock
Tensor Core  cores x TC-per-core x flops-per-TC-per-clock x clock
bandwidth    PCIe bus width x memory clock, or chip table
`

Everything that came from the builtin table is listed in `estimated[]`.
Nothing is presented as measured when it is not.

## Platform coverage

| | CPU | GPU | NPU | accelerators |
|---|---|---|---|---|
| **macOS** | `sysctlbyname` (perflevel P/E split, caches) | IOKit `IOAccelerator` | IOKit ANE class | dlopen + Metal |
| **Linux** | `/proc/cpuinfo`, sysfs topology/cache/cpufreq | sysfs DRM + `libcuda` attributes | `/dev/davinci*`, `/dev/accel*` | dlopen + `/proc/driver/nvidia` + CANN cfg |
| **Windows** | CPUID + `GetLogicalProcessorInformationEx` + registry | DXGI + NVML | SetupAPI PCI scan | LoadLibrary |

x86_64 and arm64 on all three.

## Verification status

| platform | status | hardware |
|---|---|---|
| macOS arm64 | **built and run** | Apple M5, macOS 26.6 |
| Linux x86_64 (big) | **built and run** | 2x EPYC 7402 (96 threads), 8x RTX 4090, Ubuntu 22.04, driver 580.119.02 |
| Linux x86_64 (hybrid) | **built and run** | Intel i5-1335U (2P + 8E), Ubuntu 24.04 |
| Windows win64 | written, CI-compiled | CI runner |
| Windows win32 | written, CI-compiled | CI runner |

The Linux run is a real acceptance, not a smoke test: it reported 48 cores /
96 threads, L1d 1.5 MiB, L2 24 MiB, L3 256 MiB, and all eight GPUs at 128 SM /
512 Tensor Cores / 1008 GB/s with `sm_8.9`. Those are the datasheet numbers,
which is the point.

`.github/workflows/ci.yml` builds ubuntu-latest, macos-latest, macos-13 and
windows-latest in both bitnesses (`-A x64` and `-A Win32`), and validates
the JSON schema on each.

## Hybrid CPUs (P/E cores and split caches)

A 13th-gen mobile part is not a uniform machine, and the naive approach gets
it wrong twice.

**Core counts.** `/sys/devices/cpu_core/cpus` holds *logical* CPUs, so an
i5-1335U lists `0-3` for two hyperthreaded P cores. Counting entries reports
4 P cores. The fix is to resolve each CPU's `(physical_package_id, core_id)`
and count distinct pairs:

```
 10 cores (2P + 8E) / 12 threads     <- 2 physical P + 8 physical E
```

**Caches.** P cores and E cores ship different L1 and L2 sizes, so scaling
one core's cache by the core count is wrong. Instead every cache *instance*
is summed once, keyed by `indexN + shared_cpu_list`:

| | P core | E core | total |
|---|---|---|---|
| L1d | 48 KiB x2 | 32 KiB x8 | 352 KiB |
| L1i | 32 KiB x2 | 64 KiB x8 | 576 KiB |
| L2 | 1280 KiB x2 | 2048 KiB x2 clusters | 6.5 MiB |
| L3 | shared by all 12 | | 12 MiB |

Verified against `sysfs` on both the hybrid laptop and a 2-socket EPYC.

On Windows the same split comes from `GetSystemCpuSetInformation`, which
tags each logical CPU with an `EfficiencyClass`.

A non-hybrid CPU reports no P/E split at all: inventing "48 P cores" on a
homogeneous EPYC would be a fiction.

## Measured against a second machine

    # 2x AMD EPYC 7402, 8x RTX 4090, 501 GiB, Ubuntu 22.04
    devinfo - 139 ms  (fast)
    linux Ubuntu 22.04.5 LTS | x86_64 | native
    CPU  AMD EPYC 7402 24-Core Processor
         48 cores / 96 threads         clock 2.800 GHz  x86_64
         L1i 1.5 MiB  L1d 1.5 MiB  L2 24.0 MiB  L3 256.0 MiB  line 64B
         RAM 501.3 GiB
         peak FP32 2.150 TFLOPS~
    GPU0 NVIDIA GeForce RTX 4090
         128 SM(s)  512 Tensor Cores  sm_8.9
         vram 23.5 GiB  clock 2.520 GHz  bw 1008.1 GB/s
         peak FP32 82.58 TF~  FP16 330.3 TF~  BF16 330.3 TF~  (sparse 660.6 TF)~
    ...
    accelerators
         cuda          13.0          driver
         cuda_runtime  13.0          runtime
         nvml          580.119.02    lib
         vulkan                      runtime
         opencl                      runtime
         nvcc                        toolchain

Note which values lack the `~`: GPU clock, VRAM, bandwidth, SM and Tensor
Core counts and the CUDA driver/toolkit versions all came off the machine.
Only the throughput peaks are derived.

### Why Linux takes ~140 ms and macOS ~5 ms

Enumerating an NVIDIA GPU through the driver costs `cuInit`: measured
**139 ms** on this 8-GPU node versus **28 ms** for `nvmlInit`. But NVML cannot
report SM counts on this driver, and SM count is what Tensor Core figures come
from, so the CUDA driver is the price of the headline number. For comparison,
shelling out to `nvidia-smi` costs 200-800 ms and returns less.

On macOS there is no driver to initialise, hence single-digit milliseconds.

## What this deliberately is not

- **not a benchmark suite** - peaks are theoretical. `--bench` measures a
  4-thread copy bandwidth. Four is deliberate: an EPYC sweep at 1/2/4/8
  threads gives 29.5 / 40.2 / 44.2 / 33.7 GB/s, so scaling with the core count
  makes the number *worse*. Expect roughly 70-80% of theoretical peak.
  First touch of the buffers costs ~60 ms per 128 MB; that happens before the
  clock starts, so it makes `--bench` slower, not wrong.
- **the measured number is a snapshot, and the machine's state is part of it.**
  A 15 W laptop with 29 of 31 GB in use reports ~20 GB/s; an independent
  pthread memcpy implementation built for comparison reports 19.2-20.8 GB/s on
  the same box at the same time. Agreeing to 1% is the point - the tool is
  reporting the machine, not flattering it.
- **not a monitor** - no sampling, no daemon, no utilisation.
- **not 100% precise** - a chip missing from `table.cpp` yields a warning
  and omitted fields rather than invented numbers.

## Adding a chip

`src/table.cpp` holds two small arrays: `kApple[]` (per-SoC clock,
bandwidth, ANE) and `kNv[]` (per compute-capability lanes, Tensor Cores,
throughput). One line each.
