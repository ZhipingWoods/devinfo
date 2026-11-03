/* Build: cc example.c -I../include -L../build -ldevinfo -o example
 * Run:   ./example [--bench]   (LD_LIBRARY_PATH / DYLD_LIBRARY_PATH may be needed)
 */
#include "devinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char** argv) {
  uint32_t flags = DEVINFO_FAST;
  if (argc > 1 && strcmp(argv[1], "--bench") == 0) flags = DEVINFO_BENCH;

  if (devinfo_abi_version() != DEVINFO_ABI_VERSION) {
    fprintf(stderr, "abi mismatch: header %u, library %u\n",
            DEVINFO_ABI_VERSION, devinfo_abi_version());
    return 1;
  }

  char* json = devinfo_probe_json(flags);
  if (!json) {
    fprintf(stderr, "probe failed\n");
    return 1;
  }
  fputs(json, stdout);
  devinfo_free(json);
  return 0;
}
