Prebuilt binaries, one per platform. No compiler, no runtime, no dependencies: unpack, run, read the machine.

| Platform | Architecture | File |
|---|---|---|
| Windows 10/11 | x86_64 | `devinfo-1.0.0-windows-x64.zip` |
| Windows 10/11 | i686, 32-bit | `devinfo-1.0.0-windows-x86.zip` |
| macOS 11+ | universal: arm64 + x86_64 in one binary | `devinfo-1.0.0-macos-universal.dmg` |
| macOS 11+ | same, zipped | `devinfo-1.0.0-macos-universal.zip` |
| Linux | x86_64 | `devinfo-1.0.0-linux-x86_64.tar.gz` |
| Linux | aarch64: Jetson, Graviton, Kunpeng | `devinfo-1.0.0-linux-aarch64.tar.gz` |

Every archive holds the `devinfo` executable, its C ABI library (`devinfo.dll` / `libdevinfo.dylib` / `libdevinfo.so`), and a short `README.txt`.

The macOS build is one universal binary: native on Apple Silicon, native on Intel, no second download.

**What was verified, and where**: every binary was rebuilt from this tree and executed on the machine it targets - an M5 (both slices, the x86_64 one through Rosetta), a 2x EPYC 7402 box with 8x RTX 4090, and a Jetson AGX Orin. The DMG was mounted and the binary run from the volume. The Windows builds are compiled and inspected but not executed: there is no Windows host here, and that caveat stays in the README rather than being rounded off.

Anything printed with a leading tilde is derived from a chip specification table, not read from the hardware, and the JSON lists the same values in an `estimated` array. No number is invented.
