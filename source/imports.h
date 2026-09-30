/* imports.h -- the table every game-module import is resolved against. */
#ifndef DCR_IMPORTS_H
#define DCR_IMPORTS_H
#include "so_util.h"

extern DynLibFunction dcr_imports[];
extern int dcr_imports_count;

/* Look a shim up by name (0 if absent). Used by dlsym() and by code that must
 * lock engine objects through the SAME shim functions the engine uses. */
uintptr_t dcr_import_lookup(const char *name);

#endif
