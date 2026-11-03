devinfo for Windows
===================

Four files, all self-contained. No installer, no Visual C++ redistributable,
no DLLs to copy alongside them. The only imports are libraries that ship with
Windows 10 and 11.

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
from the hardware. Everything else came from CPUID, DXGI, SetupAPI, NVML or
the kernel.
