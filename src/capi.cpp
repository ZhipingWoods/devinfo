/* capi.cpp - the exported plugin surface. Four functions, no structs. */
#define DEVINFO_BUILD  /* this is the TU that exports the ABI */
#include "devinfo.h"
#include "devinfo_core.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {

uint32_t devinfo_abi_version(void) { return DEVINFO_ABI_VERSION; }

const char* devinfo_version(void) { return DEVINFO_VERSION_STRING; }

char* devinfo_probe_json(uint32_t flags) {
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

void devinfo_free(char* p) { std::free(p); }

}  /* extern "C" */
