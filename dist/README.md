# devinfo 1.0.0 — prebuilt binaries

Drop-in binaries for the three desktop systems. None of them require anything
to be installed on the machine you run them on: no runtime, no redistributable,
no framework, no package manager.

| file | system | notes |
|---|---|---|
| `windows/devinfo-x64.exe` | Windows 10/11 x64 | double-click it, or use a shell |
| `windows/devinfo-x86.exe` | Windows 10/11 32-bit | |
| `windows/devinfo-x64.dll` | Windows 10/11 x64 | C ABI plugin, 4 exports |
| `windows/devinfo-x86.dll` | Windows 10/11 32-bit | C ABI plugin, 4 exports |
| `linux/devinfo` | Linux x86_64 | needs glibc ≥ 2.17 (2012) |
| `linux/libdevinfo.so` | Linux x86_64 | needs glibc ≥ 2.17 |
| `macos/devinfo` | macOS ≥ 11.0 | universal: arm64 + x86_64 |
| `macos/libdevinfo.dylib` | macOS ≥ 11.0 | universal |

## Why these move between machines

Verified with the dependency tool of each system, not assumed:

- **Windows** — import table holds only `KERNEL32`, `ADVAPI32`, `SETUPAPI`,
  `dxgi`, `version` and the `api-ms-win-crt-*` set that ships with Windows 10
  and 11. libc++ and libunwind are linked in statically.
- **Linux** — `ldd` shows only `libdl`, `libm`, `libpthread`, `libc`.
  libstdc++ and libgcc are static, and the binary is linked against
  glibc 2.17 so a CentOS 7 or Ubuntu 16.04 box still runs it.
- **macOS** — `otool -L` shows only IOKit, CoreFoundation, libc++ and
  libSystem, all part of the OS. Deployment target 11.0, sliced for both
  architectures.

## Running it

Windows, by double-click: prints the report, writes `devinfo-report.txt` and
`devinfo-report.json` next to the exe, waits for Enter.

Anywhere, from a shell:

```
devinfo                 table on a terminal, JSON when piped
devinfo --json          JSON on stdout
devinfo --table         table, even when piped
devinfo --bench         add the memory bandwidth measurement
```

`devinfo --bench` copies ~64 MiB around for a few hundred milliseconds; without
it the probe is a few milliseconds because it only reads what the OS already
knows.

## Virtual displays

Windows boxes running remote-desktop tools (ToDesk, 向日葵, GameViewer and
friends) get extra display adapters, and those adapters report the name of the
GPU they extend. Counting them naively makes one Intel iGPU look like four.

devinfo counts the real PCI display devices with SetupAPI and lets each
(vendor, device) pair account for at most that many adapters, so a single iGPU
shadowed by three virtual monitors reports one, while eight identical RTX 4090s
still report eight. Every GPU carries its PCI id in both outputs.

## What the numbers mean

Values printed with `~` are estimated from a chip specification table, not read
from the hardware. Everything else came from CPUID, sysctl, IOKit, DXGI,
SetupAPI, NVML or the kernel — no subprocesses, no `nvidia-smi`, no `lspci`,
no packages.

## Known limits, stated plainly

- The Windows binaries were produced with llvm-mingw. The MSVC build has never
  been executed — the source is the same but MSVC has not been proven.
- Windows 7/8 need the Universal CRT update installed for the `api-ms-win-crt-*`
  imports to resolve. Windows 10 and 11 need nothing.
- The Linux binaries are x86_64 only. No aarch64 Linux build yet.
- These are unsigned. Windows SmartScreen and macOS Gatekeeper may warn on a
  downloaded copy; that is a signature, not a dependency. On macOS you can clear
  the quarantine attribute with `xattr -d com.apple.quarantine ./devinfo`.
