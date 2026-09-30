/* imports_lookup.c -- the search over the import tables (see imports.h).
 *
 * The table itself, dcr_imports[], is generated per port (source/imports.c);
 * the search lives here, in the runtime, so a fix to it reaches every port
 * and the generated file stays pure data. The port's own tables come first:
 * so_resolve took the first match of a table the port built by putting its
 * overrides in front of dcr_imports, and this keeps that order. MIT.
 */
#include <stddef.h>
#include <string.h>

#include "imports.h"

__attribute__((weak)) const DynLibFunction *port_module_imports(const char *module, int *count) {
  if (count)
    *count = 0;
  return NULL;
}

static const DynLibFunction *find_in(const DynLibFunction *t, int n, const char *name) {
  for (int i = 0; t && i < n; i++)
    if (!strcmp(t[i].symbol, name))
      return &t[i];
  return NULL;
}

const DynLibFunction *rt_import_find(const char *module, const char *name) {
  if (!name)
    return NULL;
  const DynLibFunction *e = NULL;
  if (module) {
    int n = 0;
    const DynLibFunction *t = port_module_imports(module, &n);
    e = find_in(t, n, name);
  }
  if (!e && port_imports && &port_imports_count)
    e = find_in(port_imports, port_imports_count, name);
  if (!e)
    e = find_in(dcr_imports, dcr_imports_count, name);
  return e;
}

uintptr_t dcr_import_lookup(const char *name) {
  const DynLibFunction *e = rt_import_find(NULL, name);
  return e ? e->func : 0;
}
