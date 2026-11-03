#!/usr/bin/env python3
"""Call the devinfo C ABI with nothing but the standard library.

Proves the plugin is language-agnostic: no bindings, no build step, just ctypes.
"""
import ctypes
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def find_library():
    patterns = [
        os.path.join(HERE, "..", "build", "libdevinfo.dylib"),
        os.path.join(HERE, "..", "build", "libdevinfo.so"),
        os.path.join(HERE, "..", "build", "devinfo.dll"),
        os.path.join(HERE, "..", "build", "Release", "devinfo.dll"),
        os.path.join(HERE, "..", "build", "libdevinfo.so*"),
    ]
    for p in patterns:
        hits = glob.glob(p)
        if hits:
            return hits[0]
    sys.exit("build the project first: cmake -S . -B build && cmake --build build")


def main():
    lib = ctypes.CDLL(os.path.abspath(find_library()))

    lib.devinfo_abi_version.restype = ctypes.c_uint32
    lib.devinfo_version.restype = ctypes.c_char_p
    lib.devinfo_probe_json.argtypes = [ctypes.c_uint32]
    lib.devinfo_probe_json.restype = ctypes.c_void_p
    lib.devinfo_free.argtypes = [ctypes.c_void_p]
    lib.devinfo_free.restype = None

    print("abi", lib.devinfo_abi_version(), "version", lib.devinfo_version().decode())

    raw = lib.devinfo_probe_json(0)          # DEVINFO_FAST
    if not raw:
        sys.exit("probe returned NULL")
    try:
        doc = json.loads(ctypes.string_at(raw).decode("utf-8"))
    finally:
        lib.devinfo_free(raw)                # must use the library's allocator

    print(json.dumps(doc, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
