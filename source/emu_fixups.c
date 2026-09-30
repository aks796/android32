/* emu_fixups.c -- instructions an emulator cannot run, rewritten for it.
 *
 * EMULATOR ONLY: hardware (a Cortex-A57) runs all of this as it is; the
 * callers run it under dcr_is_emulator() only. From the Asphalt 8 Retry port
 * (VCVT) and the Crossy Road port (VSWP and the NEON forms).
 *
 * Ryujinx 1.1.1098's A32 decoder has no VCVT between floating point and fixed
 * point (VCVT.F32.S32 Sd, Sd, #fbits and back): the first one kills the
 * process with UndefinedInstructionException. This program's Mesa has some
 * (the sampler state's LOD values: float to fixed), and so do some engines
 * (libasphalt8.so: 23). Each is replaced by a branch, with the instruction's
 * own condition, to a stub doing the same in steps the emulator knows:
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
 *
 * Nor has it VSWP (swap two NEON registers): Unity 5.6's libunity has 233,
 * one in a constructor that runs at boot. Each becomes a branch to three
 * VEORs (the XOR swap) and a branch back; the other NEON forms it lacks are
 * listed further down.
 *
 * Which words are code: dcr_emu_fix_vcvt (VCVT only) touches a word only
 * inside a sized function symbol; dcr_emu_fix_module (everything, for a
 * stripped engine with no function symbols) only inside .text and when no
 * pc-relative load reads the word (a literal, not code). In this program's
 * own text the encoding itself is the guard (as data it would be a float of
 * magnitude 2^94: never a literal). Each entry point is the one its port was
 * emulator-tested with. MIT.
 */
#include <elf.h>
#include <string.h>

#include "rt_settings.h"
#include "emu_fixups.h"
#include "so_util.h"
#include "util.h"

static uint32_t branch(uint32_t cond, uint32_t from, uint32_t to) {
  int32_t off = (int32_t)(to - (from + 8)) >> 2;
  return cond << 28 | 0x0A000000u | ((uint32_t)off & 0x00FFFFFFu);
}

/* Is the word at off (module offset) read as data by a pc-relative load --
 * ldr/ldrb/ldrh/ldrd/vldr [pc, #imm] within their 4 KB / 1 KB reach? Then it
 * is a literal-pool entry that merely looks like an instruction. */
static int is_literal(const uint8_t *img, uint32_t text_lo, uint32_t text_hi, uint32_t off) {
  for (int32_t k = -256; k <= 1024; k++) {
    const uint32_t at = off - 4u * (uint32_t)k;
    if (k == 0 || at < text_lo || at + 4 > text_hi)
      continue;
    uint32_t w;
    memcpy(&w, img + at, 4);
    if ((w >> 28) == 0xF)
      continue;
    const uint32_t pc = at + 8;
    if ((w & 0x0E5F0000u) == 0x041F0000u) { /* ldr/ldrb rt, [pc, #+-imm12] */
      const uint32_t imm = w & 0xFFF;
      if (((w >> 23) & 1 ? pc + imm : pc - imm) == off)
        return 1;
    } else if ((w & 0x0E5F00F0u) == 0x004F00B0u || (w & 0x0E5F00D0u) == 0x004F00D0u) { /* ldrh/ldrd [pc] */
      const uint32_t imm = (w >> 4 & 0xF0) | (w & 0xF);
      const uint32_t t = (w >> 23) & 1 ? pc + imm : pc - imm;
      if (t == off || t + 4 == off)
        return 1;
    } else if ((w & 0x0F3F0E00u) == 0x0D1F0A00u) { /* vldr s/d, [pc, #+-imm8*4] */
      const uint32_t imm = (w & 0xFF) * 4;
      const uint32_t t = (w >> 23) & 1 ? pc + imm : pc - imm;
      if (t == off || ((w & 0x100) && t + 4 == off))
        return 1;
    }
  }
  return 0;
}

/* Is off (module offset) inside a sized ARM function symbol? */
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

/* VSWP Dd, Dm / Qd, Qm (A1): 1111 0011 1D11 size 10 Vd 0000 0 Q M 0 Vm. */
static int is_vswp(uint32_t w) { return (w & 0xFFB30F90u) == 0xF3B20000u; }

/* -> b stub; stub: veor d,d,m; veor m,d,m; veor d,d,m; b back (the Q bit
 * carries over: VEOR has the same D/Vd, M/Vm and Q fields). */
static void fix_vswp(uint32_t *site, uint32_t w, Pool *p) {
  if (p->used + 4 > p->words) {
    p->skipped++;
    return;
  }
  const uint32_t D = (w >> 22) & 1, Vd = (w >> 12) & 0xF, M = (w >> 5) & 1, Vm = w & 0xF;
  const uint32_t Q = w & 0x40u;
  uint32_t *s = p->pool + p->used;
  const uint32_t stub_at = (uint32_t)(uintptr_t)s, back = (uint32_t)(uintptr_t)(site + 1);
  /* VEOR Vd, Vn, Vm: 1111 0011 0D00 Vn Vd 0001 N Q M 1 Vm */
  const uint32_t veor_d_d_m = 0xF3000110u | D << 22 | Vd << 16 | Vd << 12 | D << 7 | Q | M << 5 | Vm;
  const uint32_t veor_m_d_m = 0xF3000110u | M << 22 | Vd << 16 | Vm << 12 | D << 7 | Q | M << 5 | Vm;
  if (D == M && Vd == Vm) { /* a swap with itself: nothing */
    *site = 0xE320F000u; /* nop */
    p->fixed++;
    return;
  }
  /* veor m,d,m reads Vn = d: encode Vn = Vd with N = D */
  s[0] = veor_d_d_m;
  s[1] = (veor_m_d_m & ~(0xFu << 16 | 1u << 7)) | Vd << 16 | D << 7;
  s[2] = veor_d_d_m;
  s[3] = branch(0xE, stub_at + 12, back);
  *site = branch(0xE, (uint32_t)(uintptr_t)site, stub_at);
  p->used += 4;
  p->fixed++;
}

/* ---------------------------------------------------------------- NEON --
 * The other NEON forms libunity 5.6 uses that the decoder lacks, each as a
 * stub of instructions it has (all verified against its opcode table and
 * with Capstone on the host):
 *   VADDHN.Ix  Dd, Qn, Qm   -> vadd.I2x T, Qn, Qm;  vshrn.I2x Dd, T, #x
 *   VSRI.x     Vd, Vm, #s   -> vshr.Ux T1, Vm, #s;  vmov.i8 T2, #0xff;
 *                              vshr.Ux T2, T2, #s;  vbit Vd, T1, T2
 *   VSLI.x     Vd, Vm, #s   -> the same with vshl.Ix
 *   VACGT/VACGE.F32         -> vabs.f32 T1, Vn; vabs.f32 T2, Vm; vcgt/vcge Vd, T1, T2
 *   VPADAL.xx  Vd, Vm       -> vpaddl.xx T, Vm;     vadd.I2x Vd, Vd, T
 *   VSHLL.Ix   Qd, Dm, #x   -> vmovl.Ux T, Dm;      vshl.I2x Qd, T, #x
 * T, T1, T2 are Q registers none of the operands touch, saved around the
 * stub (vpush/vpop). All are unconditional (0xF2/0xF3), like the originals. */
static uint32_t dfield(uint32_t d) { return (d >> 4 & 1) << 22 | (d & 15) << 12; } /* D:Vd */
static uint32_t nfield(uint32_t n) { return (n >> 4 & 1) << 7 | (n & 15) << 16; }  /* N:Vn */
static uint32_t mfield(uint32_t m) { return (m >> 4 & 1) << 5 | (m & 15); }        /* M:Vm */
static uint32_t dreg_d(uint32_t w) { return (w >> 22 & 1) << 4 | (w >> 12 & 15); }
static uint32_t dreg_n(uint32_t w) { return (w >> 7 & 1) << 4 | (w >> 16 & 15); }
static uint32_t dreg_m(uint32_t w) { return (w >> 5 & 1) << 4 | (w & 15); }
static uint32_t vpush_q(uint32_t q) { return 0xED2D0B04u | dfield(2 * q); }
static uint32_t vpop_q(uint32_t q) { return 0xECBD0B04u | dfield(2 * q); }

/* A scratch Q register (q15 down to q4) whose D halves none of `used` holds. */
static uint32_t pick_q(uint64_t used) {
  for (uint32_t q = 15; q >= 4; q--)
    if (!(used >> (2 * q) & 3))
      return q;
  return 0;
}
/* D registers an operand covers: 2 for a Q operand (even number), else 1. */
static uint64_t dmask(uint32_t d, int quad) { return (quad ? 3ull : 1ull) << (quad ? (d & ~1u) : d); }

static int neon_kind(uint32_t w) {
  if ((w & 0xFF800F50u) == 0xF2800400u && ((w >> 20) & 3) != 3) return 1; /* vaddhn */
  if ((w & 0xFF800F10u) == 0xF3800410u && (w & 0x00380080u)) return 2;    /* vsri */
  if ((w & 0xFF800F10u) == 0xF3800510u && (w & 0x00380080u)) return 3;    /* vsli */
  if ((w & 0xFF900F10u) == 0xF3000E10u) return 4;                         /* vacge/vacgt .f32 */
  if ((w & 0xFFB30F10u) == 0xF3B00600u && ((w >> 18) & 3) != 3) return 5; /* vpadal */
  if ((w & 0xFFB30FD0u) == 0xF3B20300u && ((w >> 18) & 3) != 3) return 6; /* vshll #esize */
  return 0;
}

static void fix_neon(uint32_t *site, uint32_t w, int kind, Pool *p) {
  if (p->used + 10 > p->words) {
    p->skipped++;
    return;
  }
  const int quad = (w >> 6) & 1;
  uint32_t d = dreg_d(w), n = dreg_n(w), m = dreg_m(w);
  uint32_t *s = p->pool + p->used, k = 0;
  uint64_t used;
  uint32_t t1, t2 = 0;
  switch (kind) {
  case 1: { /* vaddhn: Dd <- high halves of Qn + Qm */
    const uint32_t size = (w >> 20) & 3;
    used = dmask(d, 0) | dmask(n, 1) | dmask(m, 1);
    t1 = pick_q(used);
    s[k++] = vpush_q(t1);
    s[k++] = 0xF2000840u | (size + 1) << 20 | dfield(2 * t1) | nfield(n) | mfield(m);    /* vadd.I, Q */
    s[k++] = 0xF2800810u | (8u << size) << 16 | dfield(d) | mfield(2 * t1);            /* vshrn */
    s[k++] = vpop_q(t1);
    break;
  }
  case 2: case 3: { /* vsri / vsli: Vd <- insert (Vm shifted) under the shifted all-ones mask */
    used = dmask(d, quad) | dmask(m, quad);
    t1 = pick_q(used);
    t2 = pick_q(used | dmask(2 * t1, 1));
    const uint32_t shiftfields = w & 0x003F0080u; /* imm6, L */
    const uint32_t op = kind == 2 ? 0xF3800010u /* vshr.u */ : 0xF2800510u /* vshl.i */;
    const uint32_t qb = (uint32_t)quad << 6;
    s[k++] = vpush_q(t1);
    s[k++] = vpush_q(t2);
    s[k++] = op | shiftfields | qb | dfield(2 * t1) | mfield(m);
    s[k++] = 0xF3800E1Fu | 7u << 16 | qb | dfield(2 * t2);                             /* vmov.i8 t2, #0xff */
    s[k++] = op | shiftfields | qb | dfield(2 * t2) | mfield(2 * t2);
    s[k++] = 0xF3200110u | qb | dfield(d) | nfield(2 * t1) | mfield(2 * t2);           /* vbit */
    s[k++] = vpop_q(t2);
    s[k++] = vpop_q(t1);
    break;
  }
  case 4: { /* vacgt / vacge .f32: compare absolute values */
    const uint32_t qb = (uint32_t)quad << 6, gt = (w >> 21) & 1;
    used = dmask(d, quad) | dmask(n, quad) | dmask(m, quad);
    t1 = pick_q(used);
    t2 = pick_q(used | dmask(2 * t1, 1));
    s[k++] = vpush_q(t1);
    s[k++] = vpush_q(t2);
    s[k++] = 0xF3B90700u | qb | dfield(2 * t1) | mfield(n);                            /* vabs.f32 */
    s[k++] = 0xF3B90700u | qb | dfield(2 * t2) | mfield(m);
    s[k++] = (gt ? 0xF3200E00u : 0xF3000E00u) | qb | dfield(d) | nfield(2 * t1) | mfield(2 * t2);
    s[k++] = vpop_q(t2);
    s[k++] = vpop_q(t1);
    break;
  }
  case 5: { /* vpadal: Vd += pairwise sums of Vm, widened */
    const uint32_t size = (w >> 18) & 3, qb = (uint32_t)quad << 6;
    used = dmask(d, quad) | dmask(m, quad);
    t1 = pick_q(used);
    s[k++] = vpush_q(t1);
    s[k++] = (w & ~0x0040F02Fu & ~0x00000F00u) | 0x200u | dfield(2 * t1) | mfield(m);  /* vpaddl, same size/op/Q */
    s[k++] = 0xF2000800u | (size + 1) << 20 | qb | dfield(d) | nfield(d) | mfield(2 * t1); /* vadd.I */
    s[k++] = vpop_q(t1);
    break;
  }
  default: { /* 6: vshll by the element size: Qd <- (widened Dm) << esize */
    const uint32_t size = (w >> 18) & 3;
    used = dmask(d, 1) | dmask(m, 0);
    t1 = pick_q(used);
    s[k++] = vpush_q(t1);
    s[k++] = 0xF3800A10u | (1u << size) << 19 | dfield(2 * t1) | mfield(m);             /* vmovl.u */
    /* vshl.I(2*esize) Qd, t1, #esize: imm6 = 2*esize + esize for 16/32-bit, L:imm6 = 1:32 for 64 */
    s[k++] = (size == 2 ? 0xF28005D0u | 32u << 16 : 0xF2800550u | ((16u << size) + (8u << size)) << 16) |
             dfield(d) | mfield(2 * t1);
    s[k++] = vpop_q(t1);
    break;
  }
  }
  if (!t1 || (kind >= 2 && kind <= 4 && !t2)) { /* no free scratch register: leave it */
    p->skipped++;
    return;
  }
  const uint32_t stub_at = (uint32_t)(uintptr_t)s, back = (uint32_t)(uintptr_t)(site + 1);
  s[k] = branch(0xE, stub_at + 4 * k, back);
  k++;
  *site = branch(0xE, (uint32_t)(uintptr_t)site, stub_at);
  p->used += k;
  p->fixed++;
}

/* A stripped engine: everything above, in .text (a pool within 32 MB of its
 * code, 4-10 words per site). */
int dcr_emu_fix_module(so_module *m, uint32_t *pool, size_t pool_words) {
  Pool pv = {pool, pool_words, 0, 0, 0}, ps = {0};
  int pn[7] = {0};
  uint8_t *base = m->load_base;
  int literals = 0;
  /* .text only: libunity's executable segment also holds .rodata, whose
   * tables are full of words that look like these instructions (rewriting
   * those broke FMOD: "Cannot create FMOD::Sound instance"). */
  uint32_t tlo = 0, thi = 0;
  for (int i = 0; i < m->elf_hdr->e_shnum; i++)
    if (!strcmp(m->shstrtab + m->sec_hdr[i].sh_name, ".text")) {
      tlo = m->sec_hdr[i].sh_addr;
      thi = tlo + m->sec_hdr[i].sh_size;
    }
  for (int i = 0; i < m->phnum && thi; i++) {
    const Elf32_Phdr *ph = &m->phdr[i];
    if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X))
      continue;
    uint32_t lo = ph->p_vaddr, hi = lo + ph->p_filesz; /* phdr: the pristine (file) copy */
    if (lo < tlo)
      lo = tlo;
    if (hi > thi)
      hi = thi;
    for (uint32_t off = lo; off + 4 <= hi; off += 4) {
      uint32_t *site = (uint32_t *)(base + off);
      const uint32_t w = *site;
      const int nk = neon_kind(w);
      if (!is_vcvt_fixed(w) && !is_vswp(w) && !nk)
        continue;
      if (is_literal(base, lo, hi, off)) {
        literals++;
        continue;
      }
      if (is_vswp(w)) {
        fix_vswp(site, w, &pv);
        ps.fixed++;
      } else if (nk) {
        fix_neon(site, w, nk, &pv);
        pn[nk]++;
      } else {
        fix_one(site, w, &pv);
      }
    }
  }
  const char *nm = strrchr(m->name, '/') ? strrchr(m->name, '/') + 1 : m->name;
  const int nneon = pn[1] + pn[2] + pn[3] + pn[4] + pn[5] + pn[6];
  debugPrintf("[emu] %s: stubs for Ryujinx A32 decoder gaps: %d VSWP, %d VADDHN, %d VSRI, %d VSLI, %d VACGT/GE, "
              "%d VPADAL, %d VSHLL #esize, %d fixed-point VCVT; %d look-alike literal(s) left; pool %u/%u words%s\n",
              nm, ps.fixed, pn[1], pn[2], pn[3], pn[4], pn[5], pn[6], pv.fixed - ps.fixed - nneon, literals,
              (unsigned)pv.used, (unsigned)pv.words, pv.skipped ? "; SOME NOT FIXED" : "");
  return pv.fixed;
}

/* An engine with function symbols: VCVT only, inside functions (a pool of at
 * least 8 words per site, within 32 MB of its code). */
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
    debugPrintf("[emu] " PORT_PAYLOAD_NAME ": %d fixed-point VCVT(s) -> stubs%s\n", p.fixed,
                p.skipped ? ", some NOT fixed" : "");
  return p.fixed;
}
