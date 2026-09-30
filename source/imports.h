/* imports.h -- the table every game-module import is resolved against.
 *
 * dcr_imports[] is the port's generated source/imports.c
 * (tools/gen_imports.py): every symbol the game's modules import, mapped to
 * the b_* shim or newlib function that serves it. A port may lay two tables
 * of its own over it:
 *   port_imports[]         searched before dcr_imports, for every lookup
 *                          (GL entry points a port wraps, say);
 *   port_module_imports()  a table for one module only (real sockets for a
 *                          mod library, say).
 * dcr_import_lookup() (imports_lookup.c) is the one search the loader
 * (so_util.c) and dlsym (bionic_dl.c) use. MIT.
 */
#ifndef DCR_IMPORTS_H
#define DCR_IMPORTS_H
#include "so_util.h"

/* Generated per port (source/imports.c); "" terminated, count excludes it. */
extern DynLibFunction dcr_imports[];
extern int dcr_imports_count;

/* Optional data the port defines, e.g.
 *   const DynLibFunction port_imports[] = {{"glOrthof", (uintptr_t)my_orthof}};
 *   const int port_imports_count = 1;
 * Weak references: both are NULL (their addresses) when the port has none. */
extern const DynLibFunction port_imports[] __attribute__((weak));
extern const int port_imports_count __attribute__((weak));

/* CALLBACK (weak; the default returns NULL with *count 0): a table searched
 * first for the imports of one module (its base name, "libfoo.so"). */
const DynLibFunction *port_module_imports(const char *module, int *count);

/* An import by name: port_imports first, then dcr_imports (0 if absent).
 * Used by the loader, by dlsym() and by code that must reach engine objects
 * through the SAME shim functions the engine uses. */
uintptr_t dcr_import_lookup(const char *name);

/* The entry serving name for module (NULL: any module): port_module_imports
 * (module), then port_imports, then dcr_imports. NULL if none has it; an
 * entry whose func is 0 is found (bound to NULL on purpose). */
const DynLibFunction *rt_import_find(const char *module, const char *name);

#endif
