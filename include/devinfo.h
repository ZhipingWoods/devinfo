/* devinfo.h - stable C ABI. This is the whole plugin surface.
 *
 *   char* j = devinfo_probe_json(DEVINFO_FAST);
 *   puts(j);
 *   devinfo_free(j);
 *
 * The JSON document is the ABI contract, so this header can stay frozen
 * forever: no structs cross the boundary, nothing to version-fight over.
 */
#ifndef DEVINFO_H
#define DEVINFO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVINFO_ABI_VERSION 1u
#define DEVINFO_VERSION_STRING "1.0.0"

/* Probe flags (bitmask). */
#define DEVINFO_FAST  0u  /* structural only, no measurement */
#define DEVINFO_BENCH 1u  /* + memory bandwidth micro-benchmark */

uint32_t    devinfo_abi_version(void);
const char* devinfo_version(void);

/* Returns a malloc'd NUL-terminated UTF-8 JSON document, or NULL on OOM.
 * Free it with devinfo_free() - NOT free(), on Windows they differ. */
char* devinfo_probe_json(uint32_t flags);

void devinfo_free(char* p);

#ifdef __cplusplus
}  /* extern "C" */
#endif
#endif /* DEVINFO_H */
