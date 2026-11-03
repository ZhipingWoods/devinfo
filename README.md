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

| platform | status |
|---|---|
| macOS arm64 | **built and run**, output verified on Apple M5 |
| Linux | `src/linux.cpp` is POSIX-only and passes `-Wall -Wextra` compilation; exercised by CI on ubuntu-latest |
| Windows | written against documented Win32/DXGI/SetupAPI; exercised by CI on windows-latest |

`.github/workflows/ci.yml` builds and runs the CLI on all three targets,
so the paths that could not be executed locally are still compiled on every push.

## What this deliberately is not

- **not a benchmark suite** - peaks are theoretical. `--bench` measures a
  multi-threaded copy bandwidth (threads pinned to performance cores where
  the count is known), which typically lands at 70-80% of theoretical peak.
- **not a monitor** - no sampling, no daemon, no utilisation.
- **not 100% precise** - a chip missing from `table.cpp` yields a warning
  and omitted fields rather than invented numbers.

## Adding a chip

`src/table.cpp` holds two small arrays: `kApple[]` (per-SoC clock,
bandwidth, ANE) and `kNv[]` (per compute-capability lanes, Tensor Cores,
throughput). One line each.
