/* crt0_reloc.c -- a text-relocation-aware replacement for libnx32's __nx_dynamic.
 *
 * WHY THIS EXISTS
 * ---------------
 * The Switch loads our NSO at an ASLR'd base, so the executable is a PIE and
 * crt0 relocates itself (__nx_dynamic) before anything else runs. On AArch32
 * the prebuilt devkitARM target libraries (newlib's libc/libm, libsysbase,
 * libstdc++) are NOT built -fPIC: their literal pools hold absolute addresses,
 * so the PIE link carries R_ARM_RELATIVE relocations whose targets are in .text
 * and .rodata -- pages the loader maps as Code (RX / R).
 *
 * HOW THEY ARE APPLIED: THROUGH A WRITABLE ALIAS, NEVER BY UNPROTECTING CODE
 * -------------------------------------------------------------------------
 * Mesosphere's SetProcessMemoryPermission turns a Code page that is made
 * writable into CodeData, and CodeData lacks KMemoryState_FlagCode, so it can
 * never be made executable again (a hardware boot died exactly there, svcBreak
 * 0xDC03). Instead:
 *   1. get a real handle to this process: send CUR_PROCESS_HANDLE as a copy
 *      handle over a session to ourselves; IPC translation turns the pseudo-
 *      handle into a real one (hbloader's technique), all with raw SVCs and a
 *      static stack, since libnx is not initialised yet;
 *   2. for each kernel memory block of [image base, __relro_start) -- .text
 *      (Code, RX), then .rodata (Code, R) -- svcMapProcessMemory it to a free
 *      range (Code has FlagCanMapProcess; the alias is RW SharedCode), apply
 *      the relocations that fall in it through the alias, and unmap it.
 *      One block per call is REQUIRED: the kernel only builds a page group
 *      from a range whose blocks all share the first block's state,
 *      permission and attributes (KPageTableBase::CheckMemoryState; Ryujinx's
 *      CheckRange is the same). Mapping .text + .rodata in one call is
 *      InvalidCurrentMemory -- a hardware boot died there (svcBreak 0xDC12,
 *      rc 0xD401).
 * The code pages keep their RX / R permissions throughout. The patched words
 * are literal-pool data, and the data cache is physically indexed, so the RX
 * view reads them coherently with no maintenance. The handle is kept for the
 * module loader (__dcr_self_handle -> selfproc.c).
 *
 * Emulators (Ryujinx) refuse the current-process pseudo-handle in
 * SetProcessMemoryPermission and do not enforce page permissions; a no-op
 * RX->RX call on page 0 detects that (hardware: success, nothing changes) and
 * the emulator path writes the pages directly.
 *
 * CONSTRAINTS: this runs before BSS/TLS/libnx exist. Only this file's code
 * (pc-relative, -fno-builtin), raw SVCs and hidden linker symbols are used.
 * MIT.
 */
#include <elf.h>
#include <stdint.h>

/* ".crt0.dcr", NOT ".crt0": the NSO begins executing at text offset 0, and the
 * loader finds MOD0 through the word at _start+4 -- libnx32's crt0 (section
 * .crt0) must stay first. dcr32.ld places .crt0.dcr immediately after it, on
 * the same page. */
#define DCR_CRT0 __attribute__((section(".crt0.dcr"), used, noinline))
/* The svc helpers are forced inline so their code lands inside __nx_dynamic,
 * which is already on page 0; they never exist as separate functions. */
#define DCR_CRT0_INLINE __attribute__((always_inline))

typedef struct {
  uint32_t magic_mod0;
  int32_t dyn_offset;
  int32_t bss_start_offset;
  int32_t bss_end_offset;
  int32_t eh_frame_hdr_start_offset;
  int32_t eh_frame_hdr_end_offset;
  int32_t unused;
  uint32_t magic_lny0;
  int32_t got_start_offset;
  int32_t got_end_offset;
  uint32_t magic_lny1;
  int32_t relro_start_offset;
  int32_t relro_end_offset;
} Mod0Header;

/* Hidden linker symbols (dcr32.ld): referenced pc-relatively, no relocation. */
extern char __dcr_page0_end[] __attribute__((visibility("hidden")));
extern char __rodata_start[] __attribute__((visibility("hidden")));
extern char __relro_start[] __attribute__((visibility("hidden")));

#define PERM_R  1u
#define PERM_RW 3u
#define PERM_RX 5u
#define CUR_PROCESS_PSEUDO 0xFFFF8001u
#define KERNEL_RESULT_INVALID_HANDLE 0xE401u   /* MAKERESULT(Module_Kernel, 114) */

/* Which path relocation took, for the boot log (main.c reports it):
 *   0 = no text relocations, 1 = patched through a writable alias (hardware),
 *   2 = direct writes after InvalidHandle (emulator). Lives in .data, which is
 *   RW from the start; referenced pc-relatively (hidden), so no relocation. */
__attribute__((visibility("hidden"), section(".data"))) volatile uint32_t __dcr_reloc_path = 0xFFFFFFFFu;

/* svcSetProcessMemoryPermission (0x73), AArch32 register ABI (libnx32 svc32.s):
 *   r0 = process handle, r2:r3 = address, r1 = size lo, r4 = size hi, r5 = perm */
static inline uint32_t DCR_CRT0_INLINE svc_set_proc_perm(uint32_t addr, uint32_t size, uint32_t perm) {
  register uint32_t r0 __asm__("r0") = CUR_PROCESS_PSEUDO;
  register uint32_t r1 __asm__("r1") = size;
  register uint32_t r2 __asm__("r2") = addr;
  register uint32_t r3 __asm__("r3") = 0;
  register uint32_t r4 __asm__("r4") = 0;
  register uint32_t r5 __asm__("r5") = perm;
  __asm__ volatile("svc 0x73"
                   : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5)
                   :
                   : "memory", "r12", "lr");
  return r0;
}

/* svcSetMemoryPermission (0x02): r0 = addr, r1 = size, r2 = perm */
static inline uint32_t DCR_CRT0_INLINE svc_set_mem_perm(uint32_t addr, uint32_t size, uint32_t perm) {
  register uint32_t r0 __asm__("r0") = addr;
  register uint32_t r1 __asm__("r1") = size;
  register uint32_t r2 __asm__("r2") = perm;
  __asm__ volatile("svc 0x02" : "+r"(r0), "+r"(r1), "+r"(r2) : : "memory", "r3", "r12", "lr");
  return r0;
}

/* svcSetMemoryAttribute (0x03): r0 = addr, r1 = size, r2 = mask, r3 = attr */
static inline uint32_t DCR_CRT0_INLINE svc_set_mem_attr(uint32_t addr, uint32_t size, uint32_t mask,
                                                 uint32_t attr) {
  register uint32_t r0 __asm__("r0") = addr;
  register uint32_t r1 __asm__("r1") = size;
  register uint32_t r2 __asm__("r2") = mask;
  register uint32_t r3 __asm__("r3") = attr;
  __asm__ volatile("svc 0x03" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "r12", "lr");
  return r0;
}


/* ---- raw SVCs for the self-handle and alias mapping (AArch32 kernel ABI,
 * as in libnx32's svc32.s) ---- */
static inline uint32_t DCR_CRT0_INLINE svc_create_session(uint32_t *server, uint32_t *client) {
  register uint32_t r0 __asm__("r0") = 0;
  register uint32_t r1 __asm__("r1") = 0;
  register uint32_t r2 __asm__("r2") = 0; /* is_light */
  register uint32_t r3 __asm__("r3") = 0; /* name */
  __asm__ volatile("svc 0x40" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "r12", "lr");
  *server = r1;
  *client = r2;
  return r0;
}

static inline uint32_t DCR_CRT0_INLINE svc_create_thread(uint32_t *out, void (*entry)(void *), void *arg,
                                                  void *stack_top, uint32_t prio, int32_t core) {
  register uint32_t r0 __asm__("r0") = prio;
  register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)entry;
  register uint32_t r2 __asm__("r2") = (uint32_t)(uintptr_t)arg;
  register uint32_t r3 __asm__("r3") = (uint32_t)(uintptr_t)stack_top;
  register uint32_t r4 __asm__("r4") = (uint32_t)core;
  __asm__ volatile("svc 0x08" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4) : : "memory", "r12", "lr");
  *out = r1;
  return r0;
}

#define DCR_SVC1(name, num)                                                          \
  static inline uint32_t DCR_CRT0_INLINE name(uint32_t a) {                           \
    register uint32_t r0 __asm__("r0") = a;                                          \
    __asm__ volatile("svc " #num : "+r"(r0) : : "memory", "r1", "r2", "r3", "r12", "lr"); \
    return r0;                                                                       \
  }
DCR_SVC1(svc_start_thread, 0x09)
DCR_SVC1(svc_close_handle, 0x16)
DCR_SVC1(svc_send_sync_request, 0x21)

static inline void DCR_CRT0_INLINE svc_exit_thread(void) { __asm__ volatile("svc 0x0A" ::: "memory"); }

/* svcReplyAndReceive: r0 = timeout lo, r1 = handles, r2 = count, r3 = reply target,
 * r4 = timeout hi; out r1 = index */
static inline uint32_t DCR_CRT0_INLINE svc_reply_and_receive(const uint32_t *handles, uint32_t count) {
  register uint32_t r0 __asm__("r0") = 0xFFFFFFFFu;
  register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)handles;
  register uint32_t r2 __asm__("r2") = count;
  register uint32_t r3 __asm__("r3") = 0; /* no reply */
  register uint32_t r4 __asm__("r4") = 0xFFFFFFFFu;
  __asm__ volatile("svc 0x43" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4) : : "memory", "r12", "lr");
  return r0;
}

/* svcWaitSynchronization: r0 = timeout lo, r1 = handles, r2 = count, r3 = timeout hi */
static inline uint32_t DCR_CRT0_INLINE svc_wait_one(const uint32_t *handle) {
  register uint32_t r0 __asm__("r0") = 0xFFFFFFFFu;
  register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)handle;
  register uint32_t r2 __asm__("r2") = 1;
  register uint32_t r3 __asm__("r3") = 0xFFFFFFFFu;
  __asm__ volatile("svc 0x18" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "memory", "r12", "lr");
  return r0;
}

/* svcMap/UnmapProcessMemory: r0 = dst, r1 = process, r2:r3 = src, r4 = size */
static inline uint32_t DCR_CRT0_INLINE svc_process_memory(int map, uint32_t dst, uint32_t proc,
                                                   uint32_t src, uint32_t size) {
  register uint32_t r0 __asm__("r0") = dst;
  register uint32_t r1 __asm__("r1") = proc;
  register uint32_t r2 __asm__("r2") = src;
  register uint32_t r3 __asm__("r3") = 0;
  register uint32_t r4 __asm__("r4") = size;
  if (map)
    __asm__ volatile("svc 0x74" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4) : : "memory", "r12", "lr");
  else
    __asm__ volatile("svc 0x75" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4) : : "memory", "r12", "lr");
  return r0;
}

/* svcQueryMemory: r0 = MemoryInfo*, r2 = address; out r1 = page info */
typedef struct {
  uint64_t addr, size;
  uint32_t type, attr, perm, ipc_refcount, device_refcount, padding;
} DcrMemInfo;
static inline uint32_t DCR_CRT0_INLINE svc_query_memory(DcrMemInfo *mi, uint32_t addr) {
  register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)mi;
  register uint32_t r1 __asm__("r1") = 0;
  register uint32_t r2 __asm__("r2") = addr;
  __asm__ volatile("svc 0x06" : "+r"(r0), "+r"(r1), "+r"(r2) : : "memory", "r3", "r12", "lr");
  return r0;
}

static inline volatile uint32_t *DCR_CRT0_INLINE thread_tls(void) {
  uint32_t v;
  __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
  return (volatile uint32_t *)(uintptr_t)v;
}

/* A real handle to this process, for svcMapProcessMemory here and for the
 * module loader later (selfproc.c). 0 until obtained. */
__attribute__((visibility("hidden"), section(".data"))) volatile uint32_t __dcr_self_handle = 0;
__attribute__((visibility("hidden"), section(".data"))) volatile uint32_t __dcr_reloc_diag[16] = {0};

static uint8_t g_recv_stack[0x2000] __attribute__((aligned(16)));

/* Receives our own request: IPC copies CUR_PROCESS_HANDLE into a real handle
 * in the message. Closing the server session ends the sender's wait. */
static void DCR_CRT0 __attribute__((noreturn)) self_handle_receiver(void *arg) {
  uint32_t server = (uint32_t)(uintptr_t)arg;
  if (svc_reply_and_receive(&server, 1) == 0) {
    volatile uint32_t *msg = thread_tls();
    uint32_t w1 = msg[1];
    if (w1 & 0x80000000u) {
      uint32_t special = msg[2];
      uint32_t ncopy = (special >> 1) & 0xF;
      uint32_t first = 3 + ((special & 1) ? 2 : 0); /* skip a sent pid */
      if (ncopy)
        __dcr_self_handle = msg[first];
    }
  }
  svc_close_handle(server);
  svc_exit_thread();
  for (;;) {
  }
}

static uint32_t DCR_CRT0 get_self_handle(void) {
  uint32_t server = 0, client = 0, thread = 0;
  if (svc_create_session(&server, &client))
    return 0;
  if (svc_create_thread(&thread, self_handle_receiver, (void *)(uintptr_t)server,
                        g_recv_stack + sizeof g_recv_stack, 0x2C, -2)) {
    svc_close_handle(server);
    svc_close_handle(client);
    return 0;
  }
  svc_start_thread(thread);
  volatile uint32_t *msg = thread_tls();
  msg[0] = 4;               /* request; no buffers */
  msg[1] = 0x80000000u;     /* no data words; special header follows */
  msg[2] = 1u << 1;         /* one copy handle, no pid */
  msg[3] = CUR_PROCESS_PSEUDO;
  svc_send_sync_request(client); /* returns when the receiver closes the session */
  svc_close_handle(client);
  svc_wait_one(&thread);
  svc_close_handle(thread);
  return __dcr_self_handle;
}

/* A free range of at least `size` bytes in the 32-bit code region (where
 * SharedCode mappings may live, outside the heap and alias regions), with a
 * guard page, whose address is congruent to `base` modulo 64 KiB. The kernel
 * needs no such congruence (4 KiB pages), but emulators on 16 KiB-page hosts
 * (Ryujinx on Apple silicon) can only alias pages at the same offset within a
 * host page, and it lets the test build exercise this path there. */
static uint32_t DCR_CRT0 find_free_range(uint32_t size, uint32_t base) {
  uint32_t a = 0x00200000u;
  while (a < 0x40000000u) {
    DcrMemInfo mi;
    if (svc_query_memory(&mi, a))
      return 0;
    uint64_t end = mi.addr + mi.size;
    if (mi.type == 0 /* unmapped */) {
      /* >= a + one guard page, then base's offset within 64 KiB */
      uint64_t start = (((uint64_t)a + 0x1000u + 0xFFFFu) & ~0xFFFFull) | (base & 0xFFFFu);
      if (end > start && end - start >= (uint64_t)size + 0x1000u)
        return (uint32_t)start;
    }
    if (end <= a || end > 0x40000000ull)
      return 0;
    a = (uint32_t)end;
  }
  return 0;
}

/* Apply the R_ARM_RELATIVE relocations whose targets lie in [lo, hi), writing
 * each through `view`, the address at which `lo` is writable. */
static void DCR_CRT0 apply_relative(const Elf32_Rel *rel, uint32_t relsz, uint32_t base,
                                    uint32_t lo, uint32_t hi, uint32_t view) {
  for (uint32_t i = 0; i < relsz; i++) {
    if (ELF32_R_TYPE(rel[i].r_info) != R_ARM_RELATIVE)
      continue;
    uint32_t tgt = base + rel[i].r_offset;
    if (tgt >= lo && tgt < hi)
      *(volatile uint32_t *)(view + (tgt - lo)) += base;
  }
}

/* svcBreak (0x26): r0 = reason, r1 = address, r2 = size. A failed relocation is
 * unrecoverable and nothing (not even the log) exists yet, so stop loudly.
 * Atmosphere's crash report shows r1 / r2 as "Break Address" / "Break Size";
 * r3 is not an svcBreak argument but appears as X[03] in its register dump. */
static void DCR_CRT0 __attribute__((noreturn)) reloc_abort3(uint32_t code, uint32_t detail,
                                                            uint32_t extra) {
  register uint32_t r0 __asm__("r0") = 0;
  register uint32_t r1 __asm__("r1") = code;
  register uint32_t r2 __asm__("r2") = detail;
  register uint32_t r3 __asm__("r3") = extra;
  __asm__ volatile("svc 0x26" : : "r"(r0), "r"(r1), "r"(r2), "r"(r3) : "memory");
  for (;;) {
  }
}
#define reloc_abort2(code, detail) reloc_abort3((code), (detail), 0)
/* Codes: 0xDC05 RELRO protect, 0xDC10 no self handle, 0xDC11 no free range
 * (size, block), 0xDC12 map (rc, block), 0xDC13 unmap (rc, block), 0xDC14
 * query (rc, address). "Break Size" = the rc or size, X[03] = the address. */
#define reloc_abort(code) reloc_abort2((code), 0)

void DCR_CRT0 __nx_dynamic(uintptr_t base, const Mod0Header *mod0) {
  if (mod0->magic_mod0 != 0x30444f4du) /* "MOD0" */
    return;

  /* Clear BSS (a plain loop: memset lives on a page that may be non-exec). */
  volatile uint8_t *bss = (uint8_t *)((uintptr_t)mod0 + mod0->bss_start_offset);
  volatile uint8_t *bss_end = (uint8_t *)((uintptr_t)mod0 + mod0->bss_end_offset);
  while (bss < bss_end)
    *bss++ = 0;

  /* Find DT_REL / DT_RELSZ. */
  const Elf32_Dyn *dyn = (const Elf32_Dyn *)((uintptr_t)mod0 + mod0->dyn_offset);
  const Elf32_Rel *rel = 0;
  uint32_t relsz = 0;
  for (; dyn->d_tag != DT_NULL; dyn++) {
    if (dyn->d_tag == DT_REL)
      rel = (const Elf32_Rel *)(base + dyn->d_un.d_ptr);
    else if (dyn->d_tag == DT_RELSZ)
      relsz = dyn->d_un.d_val / sizeof(Elf32_Rel);
  }

  const uint32_t ro_hi = (uint32_t)(uintptr_t)__relro_start;

  /* Writable targets (.data, .got, relro -- CodeData, RW from the loader)
   * are patched directly; count the ones in Code pages (.text / .rodata). */
  uint32_t n_code = 0;
  for (uint32_t i = 0; i < relsz; i++) {
    if (ELF32_R_TYPE(rel[i].r_info) != R_ARM_RELATIVE)
      continue;
    uint32_t tgt = base + rel[i].r_offset;
    if (tgt >= ro_hi)
      *(uint32_t *)tgt += base;
    else
      n_code++;
  }

  if (!n_code) {
    __dcr_reloc_path = 0;
#ifndef DCR_TEST_ALIAS_RELOC
  } else if (svc_set_proc_perm(base, 0x1000, PERM_RX) == KERNEL_RESULT_INVALID_HANDLE) {
#else
  } else if (0) { /* test build: take the hardware path under an emulator too */
#endif
    /* Emulator: no pseudo-handle support, no page-permission enforcement. */
    apply_relative(rel, relsz, base, (uint32_t)base, ro_hi, (uint32_t)base);
    __dcr_reloc_path = 2;
  } else {
    /* Hardware: patch the Code pages through writable aliases, one kernel
     * memory block (.text RX, then .rodata R) at a time -- see the header. */
#ifdef DCR_TEST_ALIAS_RELOC
    /* Test build: record each step; on failure, finish with direct writes
     * (emulators do not enforce page permissions) so the boot goes on. */
    uint32_t fb_lo = (uint32_t)base;
#define RELOC_FAIL(code, rc, at, resume)                                             \
  do {                                                                               \
    __dcr_reloc_diag[0] = (code);                                                    \
    __dcr_reloc_diag[2] = (rc);                                                      \
    __dcr_reloc_diag[3] = (at);                                                      \
    fb_lo = (resume);                                                                \
    goto test_fallback;                                                              \
  } while (0)
#else
#define RELOC_FAIL(code, rc, at, resume) reloc_abort3((code), (rc), (at))
#endif
    uint32_t self = get_self_handle();
    if (!self)
      RELOC_FAIL(0xDC10, 0, 0, (uint32_t)base);
#ifdef DCR_TEST_ALIAS_RELOC
    __dcr_reloc_diag[1] = self;
#endif
#ifdef DCR_TEST_ALIAS_RELOC
    uint32_t nblk = 0;
#endif
    for (uint32_t a = (uint32_t)base; a < ro_hi;) {
      DcrMemInfo mi;
      uint32_t rc = svc_query_memory(&mi, a);
      if (rc)
        RELOC_FAIL(0xDC14, rc, a, a);
      uint64_t block_end = mi.addr + mi.size;
      if (block_end <= a) /* cannot happen: the block holding a ends after it */
        RELOC_FAIL(0xDC14, 0, a, a);
      uint32_t e = block_end < (uint64_t)ro_hi ? (uint32_t)block_end : ro_hi;
      uint32_t alias = find_free_range(e - a, a);
      if (!alias)
        RELOC_FAIL(0xDC11, e - a, a, a);
#ifdef DCR_TEST_ALIAS_RELOC
      if (nblk < 3) {
        __dcr_reloc_diag[4 + 3 * nblk] = a;
        __dcr_reloc_diag[5 + 3 * nblk] = e - a;
        __dcr_reloc_diag[6 + 3 * nblk] = alias;
      }
      nblk++;
      /* The first relocated word of the block, read through the RX view. */
      uint32_t probe = 0, probe_old = 0;
      for (uint32_t i = 0; i < relsz && !probe; i++) {
        uint32_t tgt = base + rel[i].r_offset;
        if (ELF32_R_TYPE(rel[i].r_info) == R_ARM_RELATIVE && tgt >= a && tgt < e) {
          probe = tgt;
          probe_old = *(volatile uint32_t *)tgt;
        }
      }
#endif
      rc = svc_process_memory(1, alias, self, a, e - a);
      if (rc)
        RELOC_FAIL(0xDC12, rc, a, a);
      apply_relative(rel, relsz, base, a, e, alias);
      rc = svc_process_memory(0, alias, self, a, e - a);
      if (rc)
        RELOC_FAIL(0xDC13, rc, a, e);
#ifdef DCR_TEST_ALIAS_RELOC
      /* The write through the alias must be visible through the RX view. */
      if (probe)
        __dcr_reloc_diag[*(volatile uint32_t *)probe == probe_old + base ? 13 : 14]++;
#endif
      a = e;
    }
    __dcr_reloc_path = 1;
#ifdef DCR_TEST_ALIAS_RELOC
    /* Test build under an emulator: the alias path worked; run the rest of the
     * boot the emulator way (code in heap pages, no permission changes). */
    __dcr_reloc_diag[0] = 0xA11A5u; /* map, patch, unmap: all OK */
    __dcr_reloc_path = 2;
    goto relro;
  test_fallback:
    apply_relative(rel, relsz, base, fb_lo, ro_hi, fb_lo);
    __dcr_reloc_path = 2;
#endif
#undef RELOC_FAIL
  }

#ifdef DCR_TEST_ALIAS_RELOC
relro:
#endif
  /* Same RELRO handling as libnx's __nx_dynamic. */
  if (mod0->magic_lny0 != 0x30594e4cu || mod0->magic_lny1 != 0x31594e4cu)
    return;
  uint32_t relro = (uint32_t)((uintptr_t)mod0 + mod0->relro_start_offset);
  uint32_t relro_end = (uint32_t)((uintptr_t)mod0 + mod0->relro_end_offset);
  if (relro_end > relro) {
    if (svc_set_mem_perm(relro, relro_end - relro, PERM_R))
      reloc_abort(0xDC05);
    svc_set_mem_attr(relro, relro_end - relro, 1u << 4 /* MemAttr_IsPermissionLocked */,
                     1u << 4);
  }
}
