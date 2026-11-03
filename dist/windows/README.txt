devinfo for Windows
===================

Four files, all self-contained. No installer, no Visual C++ redistributable,
no DLLs to copy alongside them: the only imports are system libraries that
ship with Windows 10 and 11.

  devinfo-x64.exe   64-bit command line  (most machines)
  devinfo-x86.exe   32-bit command line
  devinfo-x64.dll   64-bit C ABI plugin
  devinfo-x86.dll   32-bit C ABI plugin

Use the command line
--------------------
  devinfo-x64.exe              full probe, table output
  devinfo-x64.exe --json       same probe, JSON on stdout
  devinfo-x64.exe --bench      add the memory bandwidth measurement
  devinfo-x64.exe --bench --json

If 64-bit Windows says "This app can't run on your PC", the machine is 32-bit
Windows and you want devinfo-x86.exe instead.

Use the DLL
-----------
  #include "devinfo.h"
  char* j = devinfo_probe_json(DEVINFO_FAST);
  puts(j);
  devinfo_free(j);          /* not free(): the allocation crosses the DLL boundary */

Four exported functions, nothing else in the export table:
  devinfo_abi_version, devinfo_version, devinfo_probe_json, devinfo_free

How these were built
--------------------
Cross-compiled with llvm-mingw (clang, UCRT target), not MSVC. The source is
the same tree; the MSVC project has not been run yet, so treat these binaries
as the first proof that the Windows backend compiles and links at all.

Numbers marked with ~ are estimated from a chip spec table rather than read
from the hardware. Everything else came from CPUID, DXGI, SetupAPI, NVML or
the kernel.
