/* capi.cpp - the exported plugin surface. Four functions, no structs. */
#include "devinfo.h"
#include "devinfo_core.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#define DEVINFO_EXPORT __declspec(dllexport)
#else
#define DEVINFO_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

DEVINFO_EXPORT uint32_t devinfo_abi_version(void) { return DEVINFO_ABI_VERSION; }

DEVINFO_EXPORT const char* devinfo_version(void) { return DEVINFO_VERSION_STRING; }

DEVINFO_EXPORT char* devinfo_probe_json(uint32_t flags) {
  try {
    std::string j = devinfo::to_json(devinfo::probe(flags));
    char* p = (char*)std::malloc(j.size() + 1);
    if (!p) return nullptr;
    std::memcpy(p, j.c_str(), j.size() + 1);
    return p;
  } catch (...) {
    return nullptr;
  }
}

DEVINFO_EXPORT void devinfo_free(char* p) { std::free(p); }

}  /* extern "C" */
