/* so_util.h -- AArch32 Android .so loader for the Switch (libnx32).
 *
 * Loads armeabi-v7a shared objects into a libnx32 code-memory reservation,
 * applies their ARM REL relocations, resolves imports against the shim tables
 * and the other loaded modules, and maps the code executable with the
 * Atmosphere code-memory syscalls. From the Crossy Road port (Andy Nguyen /
 * fgsfds so-loader lineage and vita2hos load.c), plus the PvZ port's writable
 * phase at the final address (so_map_writable) for run-time hookers. MIT.
 */
#ifndef DCR_SO_UTIL_H
#define DCR_SO_UTIL_H

#include <elf.h>
#include <stdint.h>
#include <switch.h>

#define SO_MAX_SEGMENTS 8

/* One shim: an imported symbol name mapped to the host function that serves it. */
typedef struct {
  char symbol[128];
  uintptr_t func;
} DynLibFunction;

enum {
  SO_STAGED = 0, /* image in load_base (heap), final addresses computed, not mapped */
  SO_WRITABLE,   /* RW alias of the image AT load_virtbase (so_map_writable): writable,
                    not executable -- the phase in which another module hooks it */
  SO_SEALED,     /* mapped as code: RX text, RW data (so_finalize) */
};

typedef struct so_module {
  struct so_module *next;

  /* file image (freed after finalize) */
  void *so_base;
  size_t so_size;

  /* mapped image: RW staging (load_base) and the final address (load_virtbase) */
  void *load_base;
  void *load_virtbase;
  VirtmemReservation *load_memrv;
  size_t load_size;
  int state;
  int inited;          /* its constructors have run (so_execute_init_array) */
  int init_on_dlopen;  /* run them at the first dlopen of it, as Android's
                          loader does (bionic_dl.c) */

  /* SO_WRITABLE only: the staging pages mapped as code at a temporary address,
   * which is what the RW alias at load_virtbase is made from. */
  void *tmp_code;
  VirtmemReservation *tmp_rv;

  Elf32_Ehdr *elf_hdr;
  Elf32_Phdr *prog_hdr;   /* into so_base (file image) */
  Elf32_Shdr *sec_hdr;
  char *shstrtab;

  Elf32_Phdr phdr[SO_MAX_SEGMENTS * 2];  /* pristine copy for dl_iterate_phdr */
  int phnum;

  Elf32_Sym *syms;
  int num_syms;
  char *dynstrtab;
  int syms_live;          /* syms/dynstrtab point into load_virtbase, not load_base */

  /* exported-symbol hash index (open addressing, symbol indices + 1) */
  uint32_t *hidx;
  uint32_t hmask;

  char name[128];         /* path it was loaded from */
  const char *base_name;  /* "libGameMain.so" (into name) */
} so_module;

/* Load / relocate / resolve / finalize -- call in this order. so_load: base may
 * be NULL, in which case a page-aligned staging buffer is allocated (and then
 * donated to the code mapping by so_finalize -- never freed). */
int  so_load(so_module *mod, const char *filename, void *base, size_t max_size);
int  so_relocate(so_module *mod);
/* Binds every undefined import, first match wins:
 *   1. funcs[0..num_funcs), when it is a table of the port's own (not NULL and
 *      not dcr_imports);
 *   2. the shims, through dcr_import_lookup (imports.h): the port's
 *      port_imports[] overlay, then dcr_imports;
 *   3. another loaded module's export, through port_import_interpose;
 *   4. for gl*, the GL layer (dcr_gl_lookup).
 * Returns the number left unresolved (weak ones become NULL and do not count;
 * with taint_missing_imports the others hold their own r_offset, so a call
 * through one faults at an address that names the site). */
int  so_resolve(so_module *mod, const DynLibFunction *funcs, int num_funcs,
                int taint_missing_imports);
/* Optional, between so_resolve and so_finalize: make the image readable and
 * writable (not executable) at its final address. so_finalize then seals it:
 * the same pages become the RX/RW code mapping at the same address. */
int  so_map_writable(so_module *mod);
void so_finalize(so_module *mod);       /* map RX/RW via the code-memory syscalls */
void so_flush_caches(so_module *mod);
void so_execute_init_array(so_module *mod); /* once: later calls do nothing */
void so_free_temp(so_module *mod);
int  so_unload(so_module *mod);

/* Between so_load and so_finalize (staged or writable): point the literals
 * libgcc's __sync_* keep for the Linux kernel's user helpers (__kuser_cmpxchg
 * at 0xffff0fc0, __kuser_memory_barrier at 0xffff0fa0) at kuser.S. Returns
 * how many it rewrote (cmpxchg + barrier; logged separately), -1 once sealed. */
int  so_fix_kuser_helpers(so_module *mod);

/* The module list, in load order. */
so_module *so_first(void);
so_module *so_find_module_by_name(const char *name); /* basename match */

/* Symbol lookup. so_find_addr* return runtime (load_virtbase) addresses. */
uintptr_t so_find_addr_rx(so_module *mod, const char *symbol);    /* fatal if missing */
uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol);/* 0 if missing */
void     *so_resolve_external(const char *name);                  /* dlsym backing */
so_module *so_find_module_by_addr(const void *addr);
DynLibFunction *so_find_import(DynLibFunction *funcs, int n, const char *name);
/* The PT_LOAD segment of mod holding addr (runtime address), or NULL. */
const Elf32_Phdr *so_segment_of(so_module *mod, uintptr_t addr);

/* Patch loaded (RX) code: writes through a temporary RW alias, then flushes.
 * hook_arm writes an 8-byte absolute-branch stub (ARM/Thumb interworking). */
int  so_patch_code(void *dst, const void *src, size_t len);
void hook_arm(uintptr_t addr, uintptr_t dst);

/* Map / unmap `len` bytes of our own memory at src as a RW alias at dst, one
 * kernel memory block per call (see so_util.c). */
Result so_alias_map(void *dst, uintptr_t src, size_t len);
Result so_alias_unmap(void *dst, uintptr_t src, size_t len);

int  so_dl_iterate_phdr(int (*cb)(void *info, size_t size, void *data), void *data);
int  so_dump_maps(char *buf, size_t cap);

/* Log each constructor before it runs (bring-up); 0 by default. */
extern int g_so_trace_ctors;

/* CALLBACK (weak; the default returns real): an import one module binds to
 * another module's export, and a dlsym() that finds one (bionic_dl.c), goes
 * through here first, so a port can wrap engine functions however the engine
 * reaches them (Crossy Road: libunity imports mono_* from libmono directly and
 * mono_add_internal_call is wrapped). Return the address to use instead. */
void *port_import_interpose(const char *sym, void *real);

#endif /* DCR_SO_UTIL_H */
