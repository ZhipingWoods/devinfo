#!/usr/bin/env bash
# Build the release artifacts from dist/.
#
# Naming is the interface here: one glance has to answer "does this run on my
# machine", so every file is devinfo-<version>-<os>-<arch>.<ext> and the arch
# is what the kernel reports, not a marketing name.
set -euo pipefail

VERSION="${1:-1.0.0}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="$ROOT/dist"
OUT="$ROOT/release"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

rm -rf "$OUT"
mkdir -p "$OUT"

readme() {
  cat <<EOF
devinfo $VERSION - one probe, the whole machine
==================================================

Platform: $1
Files:    $2

Run it
------
  ./devinfo                 table on a terminal, JSON when piped
  ./devinfo --table         force the table
  ./devinfo --json          force JSON
  ./devinfo --bench         add a real memory-bandwidth measurement
  ./devinfo --version

No installer and no runtime: it is one static binary. Nothing is written
anywhere except devinfo-report.txt / .json, and only when you double-click
it on Windows.

What it reports
---------------
CPU cores, caches, ISA extensions; GPU SM count, tensor cores, VRAM and
bandwidth; NPU throughput; PCIe link; installed stacks (CUDA, cuDNN,
TensorRT, CANN, ROCm); and what model sizes the machine can actually run.

Anything printed with a leading tilde (~) was derived from a chip spec
table, not read from the hardware. The JSON marks the same values in the
"estimated" array, so nothing is silently invented.

Plugin use
----------
A C ABI shared library ships alongside. It exports four symbols:

  uint32_t devinfo_abi_version(void);
  const char* devinfo_version(void);
  char* devinfo_probe_json(uint32_t flags);   /* 0 = fast, 1 = bench */
  void devinfo_free(char* p);

Full details: https://github.com/ZhipingWoods/devinfo
EOF
}

# ---------------------------------------------------------------- macOS
mkdir -p "$STAGE/macos/devinfo-$VERSION-macos-universal"
cp "$DIST/macos/devinfo" "$STAGE/macos/devinfo-$VERSION-macos-universal/"
cp "$DIST/macos/libdevinfo.dylib" "$STAGE/macos/devinfo-$VERSION-macos-universal/"
readme "macOS 11 or newer, universal (Apple Silicon arm64 + Intel x86_64, one binary)" \
       "devinfo, libdevinfo.dylib" \
  > "$STAGE/macos/devinfo-$VERSION-macos-universal/README.txt"
chmod +x "$STAGE/macos/devinfo-$VERSION-macos-universal/devinfo"

# The dmg is for handing to a person; the zip is for anything scripted.
if command -v hdiutil >/dev/null 2>&1; then
  hdiutil create -quiet -volname "devinfo $VERSION" \
    -srcfolder "$STAGE/macos/devinfo-$VERSION-macos-universal" \
    -fs HFS+ -format UDZO -ov \
    "$OUT/devinfo-$VERSION-macos-universal.dmg"
fi
( cd "$STAGE/macos" && zip -qry "$OUT/devinfo-$VERSION-macos-universal.zip" "devinfo-$VERSION-macos-universal" )

# --------------------------------------------------------------- Windows
for arch in x64 x86; do
  dir="$STAGE/win-$arch/devinfo-$VERSION-windows-$arch"
  mkdir -p "$dir"
  cp "$DIST/windows/devinfo-$arch.exe" "$dir/devinfo.exe"
  cp "$DIST/windows/devinfo-$arch.dll" "$dir/devinfo.dll"
  if [ "$arch" = "x64" ]; then
    line="Windows 10 or newer, 64-bit (x86_64)"
  else
    line="Windows 7 or newer, 32-bit (i686), also runs on 64-bit Windows"
  fi
  readme "$line" "devinfo.exe, devinfo.dll" > "$dir/README.txt"
  ( cd "$STAGE/win-$arch" && zip -qry "$OUT/devinfo-$VERSION-windows-$arch.zip" "devinfo-$VERSION-windows-$arch" )
done

# ----------------------------------------------------------------- Linux
pack_linux() {
  local arch="$1" src="$2" lib="$3" line="$4"
  local dir="$STAGE/linux-$arch/devinfo-$VERSION-linux-$arch"
  mkdir -p "$dir"
  cp "$src" "$dir/devinfo"
  cp "$lib" "$dir/libdevinfo.so"
  chmod +x "$dir/devinfo"
  readme "$line" "devinfo, libdevinfo.so" > "$dir/README.txt"
  tar -czf "$OUT/devinfo-$VERSION-linux-$arch.tar.gz" -C "$STAGE/linux-$arch" "devinfo-$VERSION-linux-$arch"
}
pack_linux x86_64  "$DIST/linux/devinfo"       "$DIST/linux/libdevinfo.so"       "Linux x86_64, glibc, containers included"
pack_linux aarch64 "$DIST/linux/devinfo-arm64" "$DIST/linux/libdevinfo-arm64.so" "Linux aarch64: Jetson Orin, Graviton, Ampere, Kunpeng"

echo
echo "release artifacts:"
ls -lh "$OUT" | awk 'NR>1 {printf "  %-46s %s\n", $9, $5}'
