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

/* Exactly one translation unit defines DEVINFO_BUILD before including this
 * header: the one that compiles the plugin. The attribute has to be on the
 * declaration, not just the definition, or clang warns about adding dllexport
 * to a symbol that was already declared without it. */
#if defined(_WIN32)
#  if defined(DEVINFO_BUILD)
#    define DEVINFO_API __declspec(dllexport)
#  else
#    define DEVINFO_API
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define DEVINFO_API __attribute__((visibility("default")))
#else
#  define DEVINFO_API
#endif

/* Probe flags (bitmask). */
#define DEVINFO_FAST  0u  /* structural only, no measurement */
#define DEVINFO_BENCH 1u  /* + memory bandwidth micro-benchmark */

DEVINFO_API uint32_t    devinfo_abi_version(void);
DEVINFO_API const char* devinfo_version(void);

/* Returns a malloc'd NUL-terminated UTF-8 JSON document, or NULL on OOM.
 * Free it with devinfo_free() - NOT free(), on Windows they differ. */
DEVINFO_API char* devinfo_probe_json(uint32_t flags);

DEVINFO_API void devinfo_free(char* p);

#ifdef __cplusplus
}  /* extern "C" */
#endif
#endif /* DEVINFO_H */
