/* bionic_dl.c -- dlopen / dlsym / dladdr and the ARM EHABI exidx lookup.
 *
 * The game's modules find each other through the dynamic linker: libnative_code
 * dlopen()s the engine and dlsym()s GameInit / GameRender / ..., and the mod
 * (libHomura) dlopen(RTLD_NOLOAD)s libGameMain and libnative_code and
 * dlsym()s ~2,160 engine symbols. So dlopen of a module we loaded returns that
 * module and dlsym reads its exports (hashed, so_util.c).
 *
 *   loaded modules            the module (by base name)
 *   system libraries          our shim table + the GL layer
 *   anything else             not found (libExtAudioDevice.so, an optional Audiere
 *                             output the engine probes for; libfmodex.so and
 *                             libGameRegister.so, deliberately not loaded)
 *
 * __gnu_Unwind_Find_exidx (libgcc unwinder: native_code, the engine) and
 * dl_unwind_find_exidx (LLVM libunwind: the mod) are bionic's hooks for the
 * EHABI unwinder: given a PC they return that module's .ARM.exidx table, so C++
 * exceptions thrown and caught inside the game work. MIT.
 */
#include <elf.h>
#include <stdio.h>
#include <string.h>

#include "bionic.h"
#include "gl_layer.h"
#include "imports.h"
#include "so_util.h"
#include "config.h"
#include "util.h"


static int g_sys_handle, g_default_handle;
static __thread const char *t_dlerror;

static const char *const g_system_libs[] = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libandroid.so", "libz.so",
    "libstdc++.so", "libEGL.so", "libGLESv1_CM.so", "libGLESv2.so", "libGLESv3.so",
    "libjnigraphics.so", "libOpenSLES.so",
};
/* libOpenSLES.so "exists" (the mod links it), but slCreateEngine fails
 * (android_ndk.c), so nothing takes the OpenSL path. */

void *b_dlopen(const char *name, int flags) {
  if (!name)
    return &g_default_handle;
  const char *base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
  so_module *m = so_find_module_by_name(base);
  if (m) {
    /* Android runs a library's constructors inside the dlopen that loads it:
     * libGameMain's in NativeApp::load, after the ANDROID_* variables are
     * set (its PakLib registers the APK from them in a constructor). */
    if (m->init_on_dlopen && !m->inited) {
      debugPrintf("[dl] dlopen(%s): running its constructors now, as Android's loader would\n",
                  base);
      so_execute_init_array(m);
      debugPrintf("[dl] %s constructors done\n", base);
    }
    return m;
  }
  for (unsigned i = 0; i < ARRAY_SIZE(g_system_libs); i++)
    if (!strcmp(base, g_system_libs[i]))
      return &g_sys_handle;
  debugPrintf("[dl] dlopen(%s) -> not found (from %p)\n", name, __builtin_return_address(0));
  t_dlerror = "library not found";
  return NULL;
}

int b_dlclose(void *h) { return 0; }

char *b_dlerror(void) {
  const char *e = t_dlerror;
  t_dlerror = NULL;
  return (char *)e;
}

static uintptr_t sys_lookup(const char *name) {
  uintptr_t a = dcr_import_lookup(name);
  if (!a)
    a = dcr_gl_lookup(name);
  return a;
}

static int is_module(const void *h) {
  for (so_module *m = so_first(); m; m = m->next)
    if (h == m)
      return 1;
  return 0;
}

void *b_dlsym(void *h, const char *name) {
  if (!name)
    return NULL;
  uintptr_t a = 0;
  if (is_module(h)) {
    a = so_try_find_addr_rx((so_module *)h, name);
  } else if (h == &g_sys_handle) {
    a = sys_lookup(name);
  } else {
    /* RTLD_DEFAULT (NULL/0), RTLD_NEXT (-1) and dlopen(NULL): everything. */
    a = (uintptr_t)so_resolve_external(name);
    if (!a)
      a = sys_lookup(name);
  }
  if (!a) {
    static int logged;
    if (logged++ < 64)
      debugPrintf("[dl] dlsym(%s) -> NULL (from %p)\n", name, __builtin_return_address(0));
    t_dlerror = "symbol not found";
  }
  return (void *)a;
}

int b_dladdr(const void *addr, b_Dl_info *info) {
  so_module *m = so_find_module_by_addr(addr);
  if (!m || !info)
    return 0;
  static char names[8][160];
  int idx = 0;
  for (so_module *k = so_first(); k && k != m && idx < 7; k = k->next)
    idx++;
  snprintf(names[idx], sizeof names[idx], "/data/app/" DCR_PACKAGE "-1/lib/arm/%s", m->base_name);
  info->dli_fname = names[idx];
  info->dli_fbase = m->load_virtbase;
  info->dli_sname = NULL;
  info->dli_saddr = NULL;
  /* nearest exported symbol at or below addr */
  uintptr_t off = (uintptr_t)addr - (uintptr_t)m->load_virtbase, best = 0;
  for (int i = 0; i < m->num_syms; i++) {
    const Elf32_Sym *s = &m->syms[i];
    if (s->st_shndx == SHN_UNDEF || !s->st_name)
      continue;
    uintptr_t v = s->st_value & ~1u;
    if (v <= off && v >= best) {
      best = v;
      info->dli_sname = m->dynstrtab + s->st_name;
      info->dli_saddr = (void *)((uintptr_t)m->load_virtbase + s->st_value);
    }
  }
  return 1;
}

/* _Unwind_Ptr __gnu_Unwind_Find_exidx(_Unwind_Ptr pc, int *pcount) */
uintptr_t b___gnu_Unwind_Find_exidx(uintptr_t pc, int *pcount) {
  so_module *m = so_find_module_by_addr((const void *)pc);
  if (m) {
    for (int i = 0; i < m->phnum; i++)
      if (m->phdr[i].p_type == PT_ARM_EXIDX) {
        if (pcount)
          *pcount = (int)(m->phdr[i].p_memsz / 8);
        return (uintptr_t)m->load_virtbase + m->phdr[i].p_vaddr;
      }
  }
  if (pcount)
    *pcount = 0;
  return 0;
}

/* bionic's name for the same lookup (LLVM libunwind in the mod's libc++abi). */
uintptr_t b_dl_unwind_find_exidx(uintptr_t pc, int *pcount) {
  return b___gnu_Unwind_Find_exidx(pc, pcount);
}
