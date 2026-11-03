/* main.cpp - thin CLI over the same core the plugin uses. */
#include "devinfo.h"
#include "devinfo_core.hpp"

#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <io.h>
#define ISATTY(f) _isatty(_fileno(f))
#else
#include <unistd.h>
#define ISATTY(f) isatty(fileno(f))
#endif

namespace {

void usage() {
  std::fputs(
      "devinfo " DEVINFO_VERSION_STRING " - hardware & accelerator census\n"
      "\n"
      "usage: devinfo [options]\n"
      "  --json        machine-readable output (default when not a tty)\n"
      "  --table       human-readable output (default on a tty)\n"
      "  --bench       add a memory bandwidth micro-benchmark\n"
      "  -v, --version print version\n"
      "  -h, --help    this text\n",
      stdout);
}

}  /* namespace */

int main(int argc, char** argv) {
  uint32_t flags = DEVINFO_FAST;
  int mode = 0;  /* 0 auto, 1 json, 2 table */

  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (!std::strcmp(a, "--json"))         mode = 1;
    else if (!std::strcmp(a, "--table"))   mode = 2;
    else if (!std::strcmp(a, "--bench"))   flags |= DEVINFO_BENCH;
    else if (!std::strcmp(a, "-v") || !std::strcmp(a, "--version")) {
      std::printf("devinfo %s (abi %u)\n", DEVINFO_VERSION_STRING, DEVINFO_ABI_VERSION);
      return 0;
    } else if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
      usage();
      return 0;
    } else {
      std::fprintf(stderr, "devinfo: unknown option '%s'\n", a);
      usage();
      return 2;
    }
  }

  if (mode == 0) mode = ISATTY(stdout) ? 2 : 1;

  devinfo::Report r = devinfo::probe(flags);
  std::string out = (mode == 1) ? devinfo::to_json(r) : devinfo::to_table(r);
  std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}
