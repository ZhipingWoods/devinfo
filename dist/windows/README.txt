devinfo for Windows
===================

Four files, all self-contained. No installer, no Visual C++ redistributable,
no DLLs to copy alongside them. Every imported library is part of the OS:

  KERNEL32, ADVAPI32, dxgi, SETUPAPI, POWRPROF   (all present since Windows 7)
  api-ms-win-crt-*                               (the Universal CRT)

The Universal CRT ships with Windows 10 and 11, so on those there is nothing
to install at all. On Windows 7 and 8 it arrives with Windows Update KB2999226;
without that update the exe will not start. That is a Windows component, not
something this tool asks you to install, but on a stripped-down Windows 7 image
it is worth knowing before you copy the file over.

  devinfo-x64.exe   64-bit command line  (most machines)
  devinfo-x86.exe   32-bit command line
  devinfo-x64.dll   64-bit C ABI plugin
  devinfo-x86.dll   32-bit C ABI plugin


Just double-click it
--------------------
The window prints the report, then:

  report written: C:\where\you\put\it\devinfo-report.txt
  report written: C:\where\you\put\it\devinfo-report.json

  press Enter to close ...

Read devinfo-report.txt, or send the .json somewhere. If the folder is
read-only the files go to %TEMP% instead; the printed path is the truth.

Double-click detection is not a guess: a console program started from Explorer
owns a console with exactly one process attached, while anything started from
cmd or PowerShell shares a console with at least the shell itself. So scripts
get clean output with no pause and no files, and double-clicks get a report
they can actually read.

From a shell
------------
  devinfo-x64.exe              full probe, table output
  devinfo-x64.exe --json       same probe, JSON on stdout
  devinfo-x64.exe --bench      add the memory bandwidth measurement
  devinfo-x64.exe --bench --json > out.json

If 64-bit Windows says "This app can't run on your PC", the machine is 32-bit
Windows and you want devinfo-x86.exe instead.

Use the DLL
-----------
  #include "devinfo.h"
  char* j = devinfo_probe_json(DEVINFO_FAST);
  puts(j);
  devinfo_free(j);      /* not free(): the allocation crosses the DLL boundary */

Four exported functions, nothing else in the export table:
  devinfo_abi_version, devinfo_version, devinfo_probe_json, devinfo_free

How these were built
--------------------
Cross-compiled with llvm-mingw (clang, UCRT target), not MSVC, and linked
statically so the C++ runtime is inside the binary. The MSVC project has not
been run yet, so treat these as the first proof that the Windows backend
compiles, links and runs at all -- not as a signed, shippable installer.

Windows SmartScreen may warn about an unknown publisher on a machine that sees
this file arrive over the network or email. That is a signature issue, not a
dependency issue; there is nothing to install.

Numbers marked with ~ are estimated from a chip spec table rather than read
from the hardware. Everything else came from CPUID, DXGI, SetupAPI, NVML,
SMBIOS, ACPI, sysfs or the kernel.

Reading the report
------------------
  CPU      model, cores, base and turbo clock, caches, ISA extensions
  RAM      total, module speed and count (SMBIOS type 17)
  GPU      SM count, Tensor Cores, VRAM, clock, PCIe link, peak throughput
  accels   CUDA / cuDNN / TensorRT / ROCm / CANN / oneAPI, plus toolkit and
           PyTorch versions, read from files rather than by running anything
  deploy   what all of the above implies for running a model here

The deploy block is derived, not measured, and says so on its own line. It
gives the parameter count that fits at each precision, the precision the
hardware actually supports, a parallel strategy, and the ridge point -- the
arithmetic intensity above which a kernel is compute-bound instead of
bandwidth-bound. Its memory figures assume weights only for inference and
16 bytes per parameter for Adam training; neither includes activations or KV
cache, which depend on batch size and sequence length that this tool cannot
know.

Clocks are reported as base and turbo separately, because they answer
different questions. Memory reports the speed the board actually runs at,
which is often below the module's rating.
