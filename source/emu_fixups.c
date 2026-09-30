/* emu_fixups.c -- instructions an emulator cannot run, rewritten for it.
 *
 * EMULATOR ONLY: hardware (a Cortex-A57) runs all of this as it is.
 *
 * Ryujinx 1.1.1098's A32 decoder has no VCVT between floating point and fixed
 * point (VCVT.F32.S32 Sd, Sd, #fbits and back): the first one kills the process
 * with UndefinedInstructionException. This program's Mesa has them (the
 * sampler state's LOD values: float to fixed; from the Asphalt 8 port, whose
 * engine had 23 more; libssasr.so, built without VFP, has none).
 * Each is replaced by a branch, with the instruction's own condition, to a
 * stub doing the same in steps the emulator knows:
 *
 *   vpush {dT}                 a scratch register (not the operand's)
 *   vldr  sT|dT, =2^(-+fbits)
 *   vcvt.f32.s32 Sd, Sd        fixed -> float: convert, then scale
 *   vmul.f32 Sd, Sd, sT        (float -> fixed: scale, then vcvt.s32.f32,
 *                               which rounds toward zero, as the fixed form does)
 *   vpop  {dT}
 *   b     <next instruction>
 *
 * Scaling by a power of two is exact. Only 32-bit fixed point is handled (all
 * there is); a double-precision float -> fixed form is left alone and logged.
 * In the engine only instructions inside a function symbol's extent are
 * touched; in this program's own text, where no symbols are loaded, the
 * encoding itself is the guard (as data it would be a float of magnitude
 * 2^94: never a literal). MIT.
 */
#include <elf.h>
#include <string.h>

#include "so_util.h"
#include "util.h"

static uint32_t branch(uint32_t cond, uint32_t from, uint32_t to) {
  int32_t off = (int32_t)(to - (from + 8)) >> 2;
  return cond << 28 | 0x0A000000u | ((uint32_t)off & 0x00FFFFFFu);
}

static int in_function(so_module *m, uint32_t off) {
  for (int i = 0; i < m->num_syms; i++) {
    const Elf32_Sym *s = &m->syms[i];
    if (s->st_shndx == SHN_UNDEF || ELF32_ST_TYPE(s->st_info) != STT_FUNC || (s->st_value & 1) || !s->st_size)
      continue;
    if (off >= s->st_value && off < s->st_value + s->st_size)
      return 1;
  }
  return 0;
}

typedef struct {
  uint32_t *pool;
  size_t words, used;
  int fixed, skipped;
} Pool;

/* w is a VCVT (fp <-> fixed) at *site: write its stub, point the site at it. */
static void fix_one(uint32_t *site, uint32_t w, Pool *p) {
  if (p->used + 8 > p->words) {
    p->skipped++;
    return;
  }
  const uint32_t cond = w >> 28, to_fixed = (w >> 18) & 1, U = (w >> 16) & 1, sf = (w >> 8) & 1;
  const uint32_t D = (w >> 22) & 1, Vd = (w >> 12) & 0xF;
  const uint32_t imm5 = (w & 0xF) << 1 | ((w >> 5) & 1), fbits = 32 - imm5;
  uint32_t *s = p->pool + p->used;
  const uint32_t stub_at = (uint32_t)(uintptr_t)s, back = (uint32_t)(uintptr_t)(site + 1);
  if (!sf) {
    const uint32_t d = Vd << 1 | D;           /* Sd */
    const uint32_t T = (d >> 1) == 0 ? 1 : 0; /* dT, not the one holding Sd */
    const uint32_t sT = T << 1;               /* its low half */
    float k = 1.0f;
    for (uint32_t i = 0; i < fbits; i++)
      k *= to_fixed ? 2.0f : 0.5f;
    const uint32_t vmul = 0xEE200A00u | D << 22 | Vd << 16 | Vd << 12 | D << 7 | (sT & 1) << 5 | (sT >> 1);
    s[0] = 0xED2D0B02u | T << 12;                          /* vpush {dT} */
    s[1] = 0xED9F0A03u | (sT & 1) << 22 | (sT >> 1) << 12; /* vldr sT, [pc, #12] */
    if (!to_fixed) {
      /* vcvt.f32.<s|u>32 Sd, Sd: op (bit 7) = 1 for a signed source */
      s[2] = 0xEEB80A40u | D << 22 | Vd << 12 | (U ? 0u : 1u) << 7 | D << 5 | Vd;
      s[3] = vmul;
    } else {
      s[2] = vmul;
      /* vcvt.<s|u>32.f32 Sd, Sd, round toward zero: opc2 101 signed, 100 unsigned */
      s[3] = (U ? 0xEEBC0AC0u : 0xEEBD0AC0u) | D << 22 | Vd << 12 | D << 5 | Vd;
    }
    s[4] = 0xECBD0B02u | T << 12; /* vpop {dT} */
    s[5] = branch(0xE, stub_at + 20, back);
    memcpy(&s[6], &k, 4);
    s[7] = 0;
  } else {
    const uint32_t d = D << 4 | Vd; /* Dd */
    if (d >= 16 || to_fixed) {
      p->skipped++;
      return;
    }
    const uint32_t T = d == 0 ? 1 : 0;
    double k = 1.0;
    for (uint32_t i = 0; i < fbits; i++)
      k *= 0.5;
    s[0] = 0xED2D0B02u | T << 12;                                /* vpush {dT} */
    s[1] = 0xED9F0B03u | T << 12;                                /* vldr dT, [pc, #12] */
    s[2] = 0xEEB80B40u | d << 12 | (U ? 0u : 1u) << 7 | d;       /* vcvt.f64.<s|u>32 Dd, S2d */
    s[3] = 0xEE200B00u | d << 16 | d << 12 | T;                  /* vmul.f64 Dd, Dd, dT */
    s[4] = 0xECBD0B02u | T << 12;                                /* vpop {dT} */
    s[5] = branch(0xE, stub_at + 20, back);
    memcpy(&s[6], &k, 8);
  }
  *site = branch(cond, (uint32_t)(uintptr_t)site, stub_at);
  p->used += 8;
  p->fixed++;
}

/* VCVT (between floating-point and fixed-point), A1, 32-bit fixed:
 * cond 1110 1D11 1op1U Vd 101 sf 1 1 i 0 imm4 (sx = 1) */
static int is_vcvt_fixed(uint32_t w) {
  return (w & 0x0FBA0ED0u) == 0x0EBA0AC0u && (w >> 28) != 0xF;
}

/* The engine: pool of at least 8 words per site, within 32 MB of its code. */
int dcr_emu_fix_vcvt(so_module *m, uint32_t *pool, size_t pool_words) {
  Pool p = {pool, pool_words, 0, 0, 0};
  uint8_t *base = m->load_base;
  for (int i = 0; i < m->phnum; i++) {
    const Elf32_Phdr *ph = &m->phdr[i];
    if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X))
      continue;
    for (uint32_t off = 0; off + 4 <= ph->p_filesz; off += 4) {
      uint32_t *site = (uint32_t *)(base + ph->p_vaddr + off);
      if (is_vcvt_fixed(*site) && in_function(m, ph->p_vaddr + off))
        fix_one(site, *site, &p);
    }
  }
  if (p.fixed || p.skipped)
    debugPrintf("[emu] %s: %d fixed-point VCVT(s) -> stubs (Ryujinx A32 decoder gap)%s\n", m->base_name,
                p.fixed, p.skipped ? ", some NOT fixed" : "");
  return p.fixed;
}

/* This program's own text (Mesa). Under the emulator its pages are
 * writable (crt0_reloc.c writes relocations into them directly there). */
extern char _start[];
extern char __rodata_start[] __attribute__((visibility("hidden")));
static uint32_t g_self_pool[64 * 8] __attribute__((aligned(16)));

int dcr_emu_fix_self(void) {
  Pool p = {g_self_pool, sizeof g_self_pool / 4, 0, 0, 0};
  for (uint32_t *w = (uint32_t *)_start; w < (uint32_t *)__rodata_start; w++)
    if (is_vcvt_fixed(*w))
      fix_one(w, *w, &p);
  if (p.fixed || p.skipped)
    debugPrintf("[emu] labyrinth2_nx: %d fixed-point VCVT(s) -> stubs%s\n", p.fixed, p.skipped ? ", some NOT fixed" : "");
  return p.fixed;
}
