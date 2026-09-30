# Group A (loader): notes

Files: `so_util.c/h`, `crt0_reloc.c`, `exc_handler.c` + new `exc_handler.h`,
`exc32.S`, `code_flush.c/h`, `codespace.h` + new `codespace.c`,
`selfproc.c/h`, `nx_init.c/h`, `nx32_virtmem.c`, `emu_fixups.c` + new
`emu_fixups.h`, `kuser.S`, `dcr32.ld`, `dcr32.specs`.
All compile warning-free with `tools/check.sh` (default settings). The
non-default variants (`RT_OWN_EXC_ENTRY=0`, `RT_OWN_VIRTMEM=0`,
`RT_OWN_WINDOW=0`, and the pre-4.12.0 fallback types in `exc_handler.h`) were
compiled once by hand in the same container.

What changed, per file:

| file | base | change |
| --- | --- | --- |
| so_util.c/h | non-dcr (identical x6) | `so_resolve` goes through `dcr_import_lookup`; weak `port_import_interpose`; new `so_fix_kuser_helpers`; `funcs` is `const`; `g_so_trace_ctors` declared in the header; `config.h` dropped (nothing used it) |
| crt0_reloc.c, selfproc.c/h, dcr32.ld, dcr32.specs | identical x7 | verbatim |
| exc_handler.c | lab2 + a8r's function names in `where()` | `port_exception_hook`, `port_code_region`, `rt_exc_reg`; label = `PORT_PAYLOAD_NAME`; crash.log = `PORT_ROOT_PATH`; w1/w2 160 bytes; symbol table readability check; `__libnx_exception_handler32` when the fork's header exists |
| exc_handler.h | new | fork types (or a same-layout fallback), `EXC_*`, exports, callbacks |
| exc32.S | non-dcr | `dcr_sigreturn` left out (it goes to dcr); body inside `#if RT_OWN_EXC_ENTRY` (default 1) |
| code_flush.c/h | identical x6 | weak `port_code_flush` first in `dcr_code_flush` |
| codespace.h / codespace.c | header x6 / new | `cs_rw_alias` added; weak defaults for every hook and for `g_cs_armed` |
| nx_init.c/h | identical x7 | `RT_GFX_RESERVE_MB`; the retry-size fix; `RT_OWN_WINDOW`; `dcr_vi_display` declared in the header |
| nx32_virtmem.c | identical x7 | inside `#if RT_OWN_VIRTMEM` (default 1) |
| emu_fixups.c / .h | dcr's + lab2's `in_function`/`dcr_emu_fix_vcvt` / new | self label `PORT_PAYLOAD_NAME`; both module entry points kept |
| kuser.S | pvz (= sonic = flappy) | comment only |

## 1. Settings and callbacks

### Settings

| name | where | default | ports | what it does |
| --- | --- | --- | --- | --- |
| `RT_GFX_RESERVE_MB` | rt_settings.h (shared) | 16 | all 16 | memory left outside the heap by `__libnx_initheap` |
| `PORT_ROOT_PATH` | rt_settings.h | `"/switch/" PORT_NAME` | per port | `crash.log` goes in this folder (sonic: must stay `/switch/sonic_allstars_nx`) |
| `PORT_PAYLOAD_NAME` | runtime.mk (TARGET) | none | dcrsea_nx, labyrinth2_nx, abspace_nx, pvz_nx, sonicracing_nx, fbf_nx, a8retry_nx | label for our own text in crash reports, `dcr_addr_name` and the emulator log. These are exactly today's hard-coded labels, so the output is unchanged. It names the ELF you symbolize against. |
| `RT_OWN_VIRTMEM` | nx32_virtmem.c | 1 | all 1 | 0 = empty file; the libnx32 fork's virtmem.c (4.12.0+) is linked instead |
| `RT_OWN_WINDOW` | nx_init.c | 1 | all 1 | 0 = the fork's default_window.c; `dcr_vi_display()` returns `nwindowGetDefaultDisplay()` |
| `RT_OWN_EXC_ENTRY` | exc32.S, **assembler command line** | 1 | all 1 | 0 = exc32.S assembles to nothing; the fork's weak entry (32 KiB stack) calls `__libnx_exception_handler32` -> the same handler. The assembler cannot see port_config.h, so this has to be passed by the build (`ASFLAGS += -DRT_OWN_EXC_ENTRY=0`). If a port sets it only in port_config.h, nothing changes: exc32.S's entry stays, which is the safe way to fail. |

### Callbacks (weak defaults in the runtime; a port defines the strong one)

| callback | declared in | default | who overrides | what it does |
| --- | --- | --- | --- | --- |
| `void *port_import_interpose(const char *sym, void *real)` | so_util.h | returns `real` | dcr (was `dcr_icall_interpose`) | module-to-module import binding in `so_resolve`, and B's `dlsym` |
| `int port_exception_hook(uint32_t type, ThreadExceptionInfo32 *, ThreadExceptionFrame32 *)` | exc_handler.h | 0 | dcr | runs before the crash report for every fault; 1 = handled, resume |
| `const char *port_code_region(uint32_t addr)` | exc_handler.h | NULL | pvz "hook trampoline", dcr "JIT code" | label in `where()`; non-NULL also counts as code for the stack scans |
| `int port_code_flush(void *code, size_t size)` | code_flush.h | 0 | dcr | first in `dcr_code_flush`; 1 = the port did the whole flush |
| `cs_mmap`, `cs_munmap`, `cs_mprotect`, `cs_write`, `cs_rw_alias`, `g_cs_armed` | codespace.h, defaults in codespace.c | NULL / 0 / own-module pages 1 / 0 / `addr` / 0 | pvz (all but `cs_rw_alias`); dcr (JIT arena, group B's design) | B's bionic_mem.c calls them |

New signature: `void *cs_rw_alias(void *addr, size_t len)` returns where to write
`len` bytes that belong at `addr`. The default returns `addr`; dcr returns
`jit_rw(addr)`.

`cs_mprotect`'s default is lab2/abs/sonic's: 1 for a loaded module's pages.
flappy and a8r returned 0, which gave the same result for the game and only
added "EXEC outside the game's code" log lines.

### New or newly exported API

- `int so_fix_kuser_helpers(so_module *)`. pvz's version: pre-filter
  0xffff0f60..fff, rewrite 0xffff0fc0/0fa0, report the others (first 4).
  - Works on a STAGED module (load_base) or a WRITABLE one (load_virtbase);
    returns -1 once sealed.
  - Returns cmpxchg + barrier. It logs only when it finds something.
- `uint32_t *rt_exc_reg(info, frame, n)` (was the static `reg_slot`).
- exc_handler.h declares `dcr_exception_dispatch`, `dcr_addr_name`,
  `dcr_is_code_addr`, `dcr_readable` and the `EXC_*` type codes. Callers
  declare these locally today and can include the header instead.
- `__libnx_exception_handler32`. Defined when `<switch/arm/exception32.h>`
  exists; it is unused (and gc'd) while exc32.S is the entry.
- emu_fixups.h: `dcr_emu_fix_self`, `dcr_emu_fix_vcvt` (a8r),
  `dcr_emu_fix_module` (dcr).
- nx_init.h: `ViDisplay *dcr_vi_display(void)`.
- `so_resolve(mod, funcs, n, taint)`. The lookup order is:
  1. `funcs`, if it is a table of the port's own (not NULL and not
     `dcr_imports`);
  2. `dcr_import_lookup` (port_imports, then dcr_imports);
  3. other modules' exports, through `port_import_interpose`;
  4. `dcr_gl_lookup` for `gl[A-Z]*`.

  Passing `dcr_imports` or NULL means "the default tables".

Dropped: `so_find_addr` (dcr only; it resolved against load_base, which
faults after finalize; nothing calls it). Also dcr's `dcr_sigreturn`, JIT
store emulation and signal delivery, which move into dcr (below).

## 2. Migration per port

**Every port**
- Delete the port's copies of all the files above. runtime.mk must link with
  the runtime's `dcr32.specs`/`dcr32.ld` and keep the special rule for
  crt0_reloc.o: `-fno-builtin -fno-tree-loop-distribute-patterns $(CRT0_EXTRA)`.
- `GFX_RESERVE_MB` becomes `RT_GFX_RESERVE_MB`. All ports use 16, the
  default, so there is nothing to set.
- Delete the codespace stubs and `volatile int g_cs_armed;` from:
  - lab_loader.c:193-200
  - abs_loader.c:75-
  - ssr_loader.c:166-
  - fbf_loader.c:37-
  - a8r_loader.c:45-

  Strong copies left behind still link and still win. For flappy and a8r,
  that would keep the old `cs_mprotect` = 0.
- Delete the `dcr_in_code_pool` stubs from lab_loader.c, abs_loader.c and
  ssr_loader.c. Nothing references them now.
- Replace the static `fix_kuser_helpers` with `so_fix_kuser_helpers` in
  pvz_loader.c, ssr_loader.c, fbf_loader.c and a8r_loader.c, and drop the
  local `dcr_kuser_*` prototypes.
  - Sonic keeps its check as `if (so_fix_kuser_helpers(m) != 47) debugPrintf(... "not the counts of 1.0.1's library")`.
  - Sonic's and a8r's unconditional summary line is only printed when something was found.
- The emulator boot (group F's main.c) should call `dcr_emu_fix_self()` under
  `dcr_is_emulator()` right after log init, for every port. abs, pvz and
  flappy gain it.
- so_resolve callers need no change. lab2 (lab_gl_overrides + dcr_imports)
  and pvz's libHomura table (pvz_net_imports + dcr_imports) are "own
  tables", so they are still searched first.
  - lab2 may move `lab_gl_overrides` into `port_imports[]` and pass
    `dcr_imports`.
  - pvz must NOT move `pvz_net_imports` there: `port_imports` applies to
    every module and to dlsym, and only libHomura may get real sockets.

**pvz**
- `const char *port_code_region(uint32_t a) { return pvz_in_pool((const void *)a) ? "hook trampoline" : NULL; }`
- Keep its strong `cs_mmap/cs_munmap/cs_mprotect/cs_write` and `g_cs_armed`.
- crash.log uses `PORT_ROOT_PATH` (was `PVZ_ROOT_PATH`).

**a8r**: `#include "emu_fixups.h"` for `dcr_emu_fix_vcvt`. Its function
names in crash reports are now in the runtime.

**lab2, abs, sonic, flappy**: only the "every port" items. The crash report
now names the nearest exported function, as a8r's did.

**dcr** (the odd lineage). These move into dcr's own files:
1. **so_util**: the shared one adds the export hash index, the
   STAGED/WRITABLE/SEALED states, `base_name`, `name[128]` (was 64), and
   the gl* fallback.
   - Rename `dcr_icall_interpose` (dcr_icall_hooks.c:299) to
     `port_import_interpose`, or add a strong one that calls it.
   - Drop the local prototypes in so_util.c and bionic_dl.c (B's shared
     dlsym calls it).
2. **New port file, e.g. `dcr_exc_mono.c`**, moved out of dcr's
   exc_handler.c:
   - What moves:
     - the types `LSigContext`, `LUContext`, `LVfpFrame`, `LSigInfo` and
       `SigFrame`;
     - `jit_put` and `emulate_store`;
     - `dcr_signal_run`, `deliver_signal`, `do_sigreturn` and `t_in_signal`;
     - `g_jit_stores`/`dcr_exc_jit_stores()` and `g_signals`/`dcr_signal_count()`.
   - It includes exc_handler.h. `ExcInfo32`/`ExcFrame` become
     `ThreadExceptionInfo32`/`ThreadExceptionFrame32`: same fields, except
     `lr_copy` is now `lr_entry`. `reg_slot`/`get_r` become `rt_exc_reg`.
   - The strong hook, **in this order**:
     ```c
     int port_exception_hook(uint32_t type, ThreadExceptionInfo32 *info, ThreadExceptionFrame32 *frame) {
       if (info->pc == (uint32_t)(uintptr_t)dcr_sigreturn && !(info->pstate & 0x20) && do_sigreturn(info, frame))
         return 1;
       if (type == EXC_DATA_ABORT && !(info->pstate & 0x20) && jit_is_split() &&
           jit_contains((const void *)info->far) && emulate_store(info, frame)) {
         info->pc += 4;
         g_jit_stores++;
         return 1;
       }
       return deliver_signal(type, info, frame);
     }
     const char *port_code_region(uint32_t a) { return jit_contains((const void *)a) ? "JIT code" : NULL; }
     ```
3. **New port asm, e.g. `dcr_sigreturn.S`**: the `dcr_sigreturn` block from
   dcr's exc32.S (section `.text.dcr_sigreturn`, `udf #0xdc05`), unchanged.
4. **jit_arena.c/h**: delete `ic_page_init`, `g_ic_*`,
   `dcr_icache_invalidate` and `dcr_code_flush` (they would be duplicate
   definitions), plus their prototypes, and include code_flush.h. Add:
   ```c
   int port_code_flush(void *code, size_t size) {
     if (!jit_contains(code)) return 0;
     if (g_emu_backing) { emu_flush((uintptr_t)code, size); return 1; }
     Result rc = svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u64)(uintptr_t)jit_rw(code), size);
     if (R_FAILED(rc)) { static int warned; if (!warned++) debugPrintf("[jit] data-cache flush ... 0x%x\n", rc); }
     dcr_icache_invalidate();
     return 1;
   }
   ```
   The flip-page log tag becomes "[code]" (was "[jit]").
5. **Codespace**: dcr becomes a provider. Group B's notes fix the exact
   split.
   - `cs_mmap` (PROT_EXEC -> `jit_alloc`); `cs_munmap` (arena -> `jit_free`);
     `cs_mprotect` (EXEC inside the arena -> 1);
   - `cs_write` (`jit_range_contains` -> write via `jit_rw`, 1);
     `cs_rw_alias` (`jit_contains(addr)` -> `jit_rw(addr)`);
   - `g_cs_armed = 1` once the arena is up.
6. **main.c**: include emu_fixups.h instead of the local prototypes. The
   self log line changes from "dcrsea_nx (Mesa)" to "dcrsea_nx".
   dcr_vsync.c can include nx_init.h for `dcr_vi_display`.
7. **watchdog**: `dcr_exc_jit_stores()`/`dcr_signal_count()` now live in
   dcr's port file. The shared watchdog needs a stats callback (group E).
8. **Behaviour changes for dcr** (all planned as GENERIC):
   - The crash report uses `vsnprintf` (was `b_vsnprintf`); the report
     formats never pass NULL strings.
   - The crash.log path is `PORT_ROOT_PATH "/crash.log"`, which is the same
     file with `PORT_NAME "dcr_sea_nx"`.
   - Unresolved `gl*` imports now bind to `dcr_gl_lookup` (before: tainted).
   - Export lookups use the hash index.
   - dcr gets code_flush.c, codespace.c and kuser.S from the runtime; it
     had none of them.

## 3. Open questions and risks

1. **`dcr_import_lookup` (group C)** must answer only from `port_imports` +
   `dcr_imports`. No system or fallback lookup: so_resolve now binds
   through it. Weak imports left unresolved on purpose must stay NULL: dcr's
   `__data_start`/`data_start` decide where Boehm GC's static roots come
   from. If C makes `dcr_imports` const, so_resolve already takes
   `const DynLibFunction *`.
2. **`cs_rw_alias` signature** is new, defined here as
   `void *cs_rw_alias(void *addr, size_t len)`. Group B's bionic_mem.c must
   call it that way.
3. **`RT_OWN_EXC_ENTRY` is an assembler-level switch**, not a port_config.h
   setting. runtime.mk (group E) would pass it. DESIGN/SETTINGS should say so.
4. **Function names in `where()`** are new for six ports:
   - Cost: a linear scan of the module's dynsym per call (crash report,
     watchdog, profiler reports; not a hot path).
   - I added a `dcr_readable` check on `m->syms`. Inside so_finalize, `syms`
     briefly points at donated staging pages, and a watchdog dump at that
     moment would have faulted.
   - Callers with 64-byte buffers get truncated text: watchdog.c,
     bionic_printf.c `nm[64]`, the profilers. 160 bytes fits.
   - `port_code_region` runs for every scanned stack word, so a port's
     version must be lock-free and cheap.
5. **bionic_printf.c (group B)** filters its own frames with
   `strncmp(nm, "dcrsea_nx", 9)`. It should compare against
   `PORT_PAYLOAD_NAME`, which is the label the runtime prints for our text.
6. **dcr moves to the shared so_util**, which is new on hardware for dcr:
   hash index on libunity/libmono, state field, gl fallback.
   - `so_lookup_export` returns 0 if the first definition of a name in the
     index is STB_LOCAL, even when a global one follows. dcr's linear scan
     skipped locals.
   - dynsym rarely repeats names, so this is low risk. dcr's first hardware
     boot confirms it.
7. **The fork alternatives are untested on hardware**
   (`RT_OWN_EXC_ENTRY/VIRTMEM/WINDOW=0`). Each needs one boot per engine
   family, plus, for the exception entry, a deliberate fault and a dcr
   managed NullReferenceException (32 KiB stack instead of 64). After that,
   the files can go.
8. **nx_init retry fix** changes `fake_heap_end`/`g_nxinit.heap` only on the
   retry path. Hardware never takes that path (the size is clamped first).
9. **Kept, unused by any port**: `so_unload` (its RX->RW flip would turn code
   into CodeData), `so_find_import`, `so_dl_iterate_phdr`.
10. **Not adopted** (the plan says keep the runtime's own):
    - the fork's `armICacheInvalidate`: it makes a second process handle and
      has no emulator skip;
    - `envAcquireOwnProcessHandle` for selfproc.c: crt0_reloc already holds
      a handle;
    - the fork's `__libnx_initheap`: no GFX reserve, no g_nxinit;
    - the fork's `__nx_dynamic`: it cannot patch relocations in RX pages.
11. **For group F**: main.c should still log `__dcr_reloc_path`,
    `__dcr_reloc_diag` and `g_nxinit`, and call `dcr_emu_fix_self()` (see
    Migration, every port).
12. **For group E**: `appletSetFocusHandlingMode` belongs in
    `rt_applet_start`, not in `__appInit`.
13. **For group D**: gl_layer.h must keep `uintptr_t dcr_gl_lookup(const char *)`.
