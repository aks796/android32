/* so_util.c -- AArch32 Android .so loader for the Switch. See so_util.h.
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds (MIT); 32-bit port + ARM REL handling
 * for the Disney Crossy Road wrapper, informed by vita2hos source/load.c.
 *
 * WHY THIS DIFFERS FROM THE ARM64 SIBLING
 * ---------------------------------------
 *  - Elf32, and relocations are SHT_REL (Elf32_Rel): there is no r_addend
 *    field, so the addend is the value ALREADY stored at the target word. Every
 *    case below reads *ptr where the arm64 code read rels[j].r_addend.
 *  - The import/hook stub is an 8-byte ARM absolute branch (LDR PC,[PC,#-4];
 *    .word dst) instead of arm64's LDR X17/BR X17. LDR into PC interworks, so
 *    a Thumb target (low bit set) is handled for free -- which matters because
 *    Unity 2017.4's code is Thumb-2.
 *  - Executable memory still comes from svcMapProcessCodeMemory +
 *    svcSetProcessMemoryPermission (Atmosphere lifts the owner check); the
 *    libnx32 svc wrappers marshal the 64-bit addresses across register pairs.
 */
#include <assert.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "so_util.h"
#include "util.h"
#include "error.h"
#include "selfproc.h"
#include "code_flush.h"
#include "gl_layer.h"

/* ARM relocation types (a subset -- exactly what Unity 2017.4's libs emit). */
#ifndef R_ARM_ABS32
#define R_ARM_ABS32     2
#endif
#ifndef R_ARM_GLOB_DAT
#define R_ARM_GLOB_DAT  21
#endif
#ifndef R_ARM_JUMP_SLOT
#define R_ARM_JUMP_SLOT 22
#endif
#ifndef R_ARM_RELATIVE
#define R_ARM_RELATIVE  23
#endif

#ifndef ALIGN_MEM
#define ALIGN_MEM(x, a) (((x) + ((a) - 1)) & ~((uintptr_t)(a) - 1))
#endif

static so_module *so_list = NULL;

static uintptr_t so_resolve_symbol(so_module *mod, DynLibFunction *funcs,
                                   int num_funcs, const char *name);

/* 8-byte absolute-branch stub. LDR PC,[PC,#-4] loads the word at stub+4 into PC;
 * on ARMv5T+ that interworks, so a Thumb dst (bit0 = 1) switches state. */
void hook_arm(uintptr_t addr, uintptr_t dst) {
  if (!addr)
    return;
  uint32_t stub[2] = {0xe51ff004u, (uint32_t)dst};
  so_patch_code((void *)addr, stub, sizeof stub);
}

void so_flush_caches(so_module *mod) {
  /* libnx32's armICacheInvalidate is a no-op (AArch32 EL0 cannot do cache
   * maintenance): see code_flush.c. */
  armDCacheFlush(mod->load_virtbase, mod->load_size);
  dcr_icache_invalidate();
}

void so_free_temp(so_module *mod) {
  free(mod->so_base);
  mod->so_base = NULL;
}

/* ------------------------------------------------------------------------- */
/* Exported-symbol index. Runtime hookers look symbols up by name in bulk (the
 * PvZ mod dlsym()s ~2,160 names out of libGameMain's 15,500 exports), so a
 * linear scan per lookup is not good enough. Stores symbol indices, which stay
 * valid when syms is rebased from load_base to load_virtbase. */
static uint32_t name_hash(const char *s) {
  uint32_t h = 2166136261u;
  while (*s)
    h = (h ^ (uint8_t)*s++) * 16777619u;
  return h;
}

static void build_index(so_module *mod) {
  int ndef = 0;
  for (int i = 0; i < mod->num_syms; i++)
    if (mod->syms[i].st_shndx != SHN_UNDEF && mod->syms[i].st_name)
      ndef++;
  uint32_t size = 16;
  while (size < (uint32_t)ndef * 2)
    size <<= 1;
  mod->hidx = calloc(size, sizeof(uint32_t));
  if (!mod->hidx)
    return; /* lookups fall back to the linear scan */
  mod->hmask = size - 1;
  for (int i = 0; i < mod->num_syms; i++) {
    const Elf32_Sym *s = &mod->syms[i];
    if (s->st_shndx == SHN_UNDEF || !s->st_name)
      continue;
    uint32_t h = name_hash(mod->dynstrtab + s->st_name) & mod->hmask;
    while (mod->hidx[h])
      h = (h + 1) & mod->hmask;
    mod->hidx[h] = (uint32_t)i + 1;
  }
}

/* index of the defined symbol `name` (any binding), -1 if none */
static int find_def(so_module *mod, const char *name) {
  if (mod->hidx) {
    uint32_t h = name_hash(name) & mod->hmask;
    for (uint32_t k; (k = mod->hidx[h]) != 0; h = (h + 1) & mod->hmask)
      if (!strcmp(mod->dynstrtab + mod->syms[k - 1].st_name, name))
        return (int)k - 1;
    return -1;
  }
  for (int i = 0; i < mod->num_syms; i++)
    if (mod->syms[i].st_shndx != SHN_UNDEF && !strcmp(mod->dynstrtab + mod->syms[i].st_name, name))
      return i;
  return -1;
}

/* ------------------------------------------------------------------------- */
int so_load(so_module *mod, const char *filename, void *base, size_t max_size) {
  int res = 0;
  memset(mod, 0, sizeof(*mod));
  strncpy(mod->name, filename, sizeof(mod->name) - 1);
  mod->base_name = strrchr(mod->name, '/') ? strrchr(mod->name, '/') + 1 : mod->name;

  FILE *fd = fopen(filename, "rb");
  if (!fd)
    return -1;
  fseek(fd, 0, SEEK_END);
  mod->so_size = ftell(fd);
  fseek(fd, 0, SEEK_SET);
  mod->so_base = malloc(mod->so_size);
  if (!mod->so_base) {
    fclose(fd);
    return -2;
  }
  if (fread(mod->so_base, mod->so_size, 1, fd) != 1) {
    fclose(fd);
    res = -2;
    goto err_free_so;
  }
  fclose(fd);

  if (memcmp(mod->so_base, ELFMAG, SELFMAG) != 0) {
    res = -1;
    goto err_free_so;
  }
  mod->elf_hdr = (Elf32_Ehdr *)mod->so_base;
  if (mod->elf_hdr->e_ident[EI_CLASS] != ELFCLASS32 ||
      mod->elf_hdr->e_machine != EM_ARM) {
    res = -1;
    goto err_free_so;
  }
  mod->prog_hdr = (Elf32_Phdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_phoff);
  mod->sec_hdr = (Elf32_Shdr *)((uintptr_t)mod->so_base + mod->elf_hdr->e_shoff);
  mod->shstrtab =
      (char *)((uintptr_t)mod->so_base + mod->sec_hdr[mod->elf_hdr->e_shstrndx].sh_offset);

  if (mod->elf_hdr->e_phnum > SO_MAX_SEGMENTS * 2) {
    res = -4;
    goto err_free_so;
  }
  mod->phnum = mod->elf_hdr->e_phnum;
  memcpy(mod->phdr, mod->prog_hdr, mod->phnum * sizeof(Elf32_Phdr));

  /* size of the LOAD zone = highest p_vaddr+p_memsz over PT_LOAD */
  mod->load_size = 0;
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    if (mod->prog_hdr[i].p_type == PT_LOAD) {
      const size_t seg_end = mod->prog_hdr[i].p_vaddr + mod->prog_hdr[i].p_memsz;
      if (seg_end > mod->load_size)
        mod->load_size = seg_end;
    }
  }
  mod->load_size = ALIGN_MEM(mod->load_size, 0x1000);
  if (mod->load_size > max_size) {
    res = -3;
    goto err_free_so;
  }

  /* The staging buffer becomes the SOURCE of svcMapProcessCodeMemory, which
   * requires plain heap memory (state Normal, RW) and donates it: it must be
   * page-aligned and never freed or reused afterwards. */
  mod->load_base = base ? base : memalign(0x1000, mod->load_size);
  if (!mod->load_base) {
    res = -2;
    goto err_free_so;
  }
  memset(mod->load_base, 0, mod->load_size);

  /* Reserve the RX virtual range. Emulator: run the code in place from the
   * staging buffer instead -- Ryujinx maps MapProcessCodeMemory destinations
   * with permission None and refuses the follow-up permission change, so the
   * mapped copy is unreadable there, while it does not enforce no-execute on
   * heap pages. Deciding here, before relocation, keeps every address the
   * relocations compute consistent with where the code actually runs. */
  if (dcr_is_emulator()) {
    mod->load_virtbase = mod->load_base;
    mod->load_memrv = NULL;
  } else {
    virtmemLock();
    mod->load_virtbase = virtmemFindCodeMemory(mod->load_size, 0x1000);
    mod->load_memrv = virtmemAddReservation(mod->load_virtbase, mod->load_size);
    virtmemUnlock();
    if (!mod->load_virtbase) {
      res = -2;
      goto err_free_so;
    }
  }

  /* copy segments into the RW staging base; rebase the runtime phdrs */
  for (int i = 0; i < mod->elf_hdr->e_phnum; i++) {
    Elf32_Phdr *p = &mod->prog_hdr[i];
    if (p->p_type == PT_LOAD)
      memcpy((void *)((uintptr_t)mod->load_base + p->p_vaddr),
             (void *)((uintptr_t)mod->so_base + p->p_offset), p->p_filesz);
    p->p_vaddr += (Elf32_Addr)(uintptr_t)mod->load_virtbase;
  }

  mod->syms = NULL;
  mod->dynstrtab = NULL;
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    const char *sh = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (!strcmp(sh, ".dynsym")) {
      mod->syms = (Elf32_Sym *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      mod->num_syms = mod->sec_hdr[i].sh_size / sizeof(Elf32_Sym);
    } else if (!strcmp(sh, ".dynstr")) {
      mod->dynstrtab = (char *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
    }
  }
  if (!mod->syms || !mod->dynstrtab) {
    res = -2;
    goto err_free_load;
  }
  build_index(mod);

  mod->next = NULL;
  if (!so_list) {
    so_list = mod;
  } else {
    so_module *m = so_list;
    while (m->next)
      m = m->next;
    m->next = mod;
  }
  return 0;

err_free_load:
  virtmemLock();
  if (mod->load_memrv)
    virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();
err_free_so:
  free(mod->so_base);
  mod->so_base = NULL;
  return res;
}

/* ------------------------------------------------------------------------- */
/* Apply one Elf32_Rel table. REL has no addend field: the addend is the value
 * already at the target word (`*ptr`). Split out so it can be unit-tested on
 * the host against a pyelftools oracle. Returns unresolved-import count when
 * `funcs` is given (resolve pass); 0 for the relocate pass (funcs == NULL). */
static int apply_rel(so_module *mod, Elf32_Rel *rels, int n,
                     DynLibFunction *funcs, int num_funcs, int taint) {
  const uintptr_t bias = (uintptr_t)mod->load_virtbase;
  int missing = 0;
  for (int j = 0; j < n; j++) {
    uint32_t *ptr = (uint32_t *)((uintptr_t)mod->load_base + rels[j].r_offset);
    const int type = ELF32_R_TYPE(rels[j].r_info);
    const int symidx = ELF32_R_SYM(rels[j].r_info);
    Elf32_Sym *sym = &mod->syms[symidx];

    /* Two passes over the same table: so_relocate (funcs == NULL) applies
     * everything local; so_resolve (funcs != NULL) touches ONLY undefined
     * imports. Re-applying a RELATIVE or a defined-symbol fixup in the second
     * pass would add the load bias twice. */
    if (funcs && (type == R_ARM_RELATIVE || sym->st_shndx != SHN_UNDEF))
      continue;

    switch (type) {
    case R_ARM_RELATIVE:
      /* B(S) + A: A is the in-image offset already stored at *ptr. */
      *ptr += bias;
      break;

    case R_ARM_ABS32:
      if (sym->st_shndx != SHN_UNDEF) {
        *ptr = bias + sym->st_value + *ptr;   /* S + A */
      } else if (funcs) {
        const char *name = mod->dynstrtab + sym->st_name;
        uintptr_t s = (uintptr_t)so_resolve_symbol(mod, funcs, num_funcs, name);
        if (s) {
          *ptr = s + *ptr;
        } else if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
          *ptr = 0;                    /* weak undefined resolves to NULL */
        } else {
          missing++;
          debugPrintf("%s: unresolved import: %s\n", mod->name, name);
          if (taint)
            *ptr = rels[j].r_offset;   /* poison: fault names the site */
        }
      }
      break;

    case R_ARM_GLOB_DAT:
    case R_ARM_JUMP_SLOT:
      /* S (+ A). GOT/PLT slots normally carry addend 0. */
      if (sym->st_shndx != SHN_UNDEF) {
        *ptr = bias + sym->st_value;
      } else if (funcs) {
        const char *name = mod->dynstrtab + sym->st_name;
        uintptr_t s = (uintptr_t)so_resolve_symbol(mod, funcs, num_funcs, name);
        if (s) {
          *ptr = s;
        } else if (ELF32_ST_BIND(sym->st_info) == STB_WEAK) {
          /* Weak undefined = NULL. This is load-bearing, not a nicety: Mono's
           * Boehm GC reads __data_start/data_start (weak) and, when they are
           * NULL, takes libmono's own .data as its static root range. A
           * poisoned non-zero value would send it to scan garbage, and a
           * missing one would make it probe memory with signals. */
          *ptr = 0;
        } else {
          missing++;
          debugPrintf("%s: unresolved import: %s\n", mod->name, name);
          if (taint)
            *ptr = rels[j].r_offset;
        }
      }
      break;

    default:
      if (!funcs)
        fatal_error("so_util: unhandled ARM reloc type %d in %s", type, mod->name);
      break;
    }
  }
  return missing;
}

/* Walk .rel.dyn / .rel.plt applying local + relative fixups (no imports yet). */
int so_relocate(so_module *mod) {
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    const char *sh = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (!strcmp(sh, ".rel.dyn") || !strcmp(sh, ".rel.plt")) {
      Elf32_Rel *rels = (Elf32_Rel *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      int n = mod->sec_hdr[i].sh_size / sizeof(Elf32_Rel);
      apply_rel(mod, rels, n, NULL, 0, 0);
    }
  }
  return 0;
}

/* look up an exported (defined, non-local) symbol in one module -> runtime address or 0 */
static uintptr_t so_lookup_export(so_module *mod, const char *name) {
  int i = find_def(mod, name);
  if (i < 0 || ELF32_ST_BIND(mod->syms[i].st_info) == STB_LOCAL)
    return 0;
  return (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
}

/* prefer a shim; else another loaded module's export; else, for gl*, the
 * GL layer (the PvZ engine imports its GLES 1/2 entry points directly). */
static uintptr_t so_resolve_symbol(so_module *mod, DynLibFunction *funcs, int num_funcs,
                                   const char *name) {
  for (int k = 0; k < num_funcs; k++)
    if (!strcmp(name, funcs[k].symbol))
      return funcs[k].func;
  for (so_module *m = so_list; m; m = m->next) {
    if (m == mod)
      continue;
    uintptr_t a = so_lookup_export(m, name);
    if (a)
      return a;
  }
  if (name[0] == 'g' && name[1] == 'l' && name[2] >= 'A' && name[2] <= 'Z')
    return dcr_gl_lookup(name);
  return 0;
}

int so_resolve(so_module *mod, DynLibFunction *funcs, int num_funcs, int taint) {
  int missing = 0;
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    const char *sh = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (!strcmp(sh, ".rel.dyn") || !strcmp(sh, ".rel.plt")) {
      Elf32_Rel *rels = (Elf32_Rel *)((uintptr_t)mod->load_base + mod->sec_hdr[i].sh_addr);
      int n = mod->sec_hdr[i].sh_size / sizeof(Elf32_Rel);
      missing += apply_rel(mod, rels, n, funcs, num_funcs, taint);
    }
  }
  if (missing)
    debugPrintf("%s: %d unresolved imports\n", mod->name, missing);
  return missing;
}

void *so_resolve_external(const char *name) {
  if (!name)
    return NULL;
  for (so_module *m = so_list; m; m = m->next) {
    uintptr_t a = so_lookup_export(m, name);
    if (a)
      return (void *)a;
  }
  return NULL;
}

/* ------------------------------------------------------------------------- */
/* EMULATOR ONLY. Ryujinx 1.1.1098's T32 decoder has no DMB/DSB/ISB, and
 * libunity's statically linked libc++ (Thumb) issues 188 of them. On hardware
 * the Cortex-A57 executes them and this never runs. Replaced with NOP.W so an
 * emulator boot can proceed; the lost ordering is an emulator-test risk only.
 * Only halfword-aligned T32 encodings F3BF 8F4x/8F5x/8F6x are touched. */
static void emu_strip_thumb_barriers(so_module *mod) {
  int n = 0;
  for (int i = 0; i < mod->phnum; i++) {
    const Elf32_Phdr *ph = &mod->phdr[i];
    if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X))
      continue;
    uint16_t *p = (uint16_t *)((uintptr_t)mod->load_virtbase + ph->p_vaddr);
    size_t cnt = ph->p_filesz / 2;
    for (size_t k = 0; k + 1 < cnt; k++)
      if (p[k] == 0xF3BF && (p[k + 1] & 0xFFC0) == 0x8F40) {
        p[k] = 0xF3AF; /* NOP.W */
        p[k + 1] = 0x8000;
        n++;
        k++;
      }
  }
  if (n)
    debugPrintf("[emu] %s: %d Thumb barrier(s) -> nop.w (Ryujinx T32 decoder gap)\n",
                strrchr(mod->name, '/') ? strrchr(mod->name, '/') + 1 : mod->name, n);
}

static Result alias_blocks(bool map, void *alias, uintptr_t src, size_t len, size_t *done);

/* Point syms/dynstrtab at the image's final address (load_base is about to be
 * donated to a code mapping and fault on access). */
static void syms_go_live(so_module *mod) {
  if (mod->syms_live)
    return;
  const uintptr_t delta = (uintptr_t)mod->load_virtbase - (uintptr_t)mod->load_base;
  if (mod->syms)
    mod->syms = (Elf32_Sym *)((uintptr_t)mod->syms + delta);
  if (mod->dynstrtab)
    mod->dynstrtab = (char *)((uintptr_t)mod->dynstrtab + delta);
  mod->syms_live = 1;
}

/* ------------------------------------------------------------------------
 * THE WRITABLE PHASE (for a module another module hooks at load time)
 *
 * Horizon never maps a page writable and executable, and Mesosphere never
 * lets a code page that has been made writable become executable again
 * (SetProcessMemoryPermission turns Code/AliasCode+W into *CodeData, which has
 * no FlagCode). A hooker that mprotect()s text RWX, writes branches into it and
 * mprotect()s it back RX therefore cannot work in place. But it does not need
 * the text to be executable while it writes -- only readable and writable at
 * the addresses it will later run at. So:
 *   so_map_writable:  staging --MapProcessCodeMemory--> T (temporary, AliasCode)
 *                     T --MapProcessMemory--> V = load_virtbase   (SharedCode, RW)
 *   ... the hooker reads and writes the image at V ...
 *   so_finalize:      unmap V, unmap T (the pages return to staging, with every
 *                     write in them), then the normal MapProcessCodeMemory at V
 *                     and per-segment RX/RW.
 * Every syscall here is one the Crossy Road port uses on hardware already
 * (crt0_reloc.c, so_patch_code, so_finalize); only the sequence is new.
 * Break codes on failure: 0xDC20 map T, 0xDC21 alias V, 0xDC22 unmap V,
 * 0xDC23 unmap T (kernel result logged first). */
static NORETURN void seal_fail(uint32_t code, const char *what, Result rc, so_module *mod) {
  debugPrintf("[so] %s: %s failed 0x%x (V %p T %p size 0x%x)\n", mod->base_name, what, rc,
              mod->load_virtbase, mod->tmp_code, (unsigned)mod->load_size);
  log_flush_ring();
  fatal_error("Loading %s failed: %s returned 0x%x (code %lx).", mod->base_name, what, rc,
              (unsigned long)code);
}

int so_map_writable(so_module *mod) {
  if (mod->state != SO_STAGED)
    return -1;
  if (dcr_is_emulator()) {
    /* The image already lives at its final address, in RW heap memory. */
    mod->state = SO_WRITABLE;
    return 0;
  }
  virtmemLock();
  mod->tmp_code = virtmemFindCodeMemory(mod->load_size, 0x1000);
  mod->tmp_rv = mod->tmp_code ? virtmemAddReservation(mod->tmp_code, mod->load_size) : NULL;
  virtmemUnlock();
  if (!mod->tmp_rv)
    fatal_error("%s: no address space for the writable phase", mod->base_name);
  Result rc = svcMapProcessCodeMemory(dcr_self_process(), (u64)(uintptr_t)mod->tmp_code,
                                      (u64)(uintptr_t)mod->load_base, mod->load_size);
  if (R_FAILED(rc))
    seal_fail(0xDC20, "svcMapProcessCodeMemory (temporary)", rc, mod);
  size_t done = 0;
  rc = alias_blocks(true, mod->load_virtbase, (uintptr_t)mod->tmp_code, mod->load_size, &done);
  if (R_FAILED(rc))
    seal_fail(0xDC21, "svcMapProcessMemory (RW at the final address)", rc, mod);
  syms_go_live(mod);
  mod->state = SO_WRITABLE;
  debugPrintf("[so] %s writable at %p (via %p) for the hook phase\n", mod->base_name,
              mod->load_virtbase, mod->tmp_code);
  return 0;
}

static void end_writable(so_module *mod) {
  size_t done = 0;
  Result rc = alias_blocks(false, mod->load_virtbase, (uintptr_t)mod->tmp_code, mod->load_size, &done);
  if (R_FAILED(rc))
    seal_fail(0xDC22, "svcUnmapProcessMemory (RW alias)", rc, mod);
  rc = svcUnmapProcessCodeMemory(dcr_self_process(), (u64)(uintptr_t)mod->tmp_code,
                                 (u64)(uintptr_t)mod->load_base, mod->load_size);
  if (R_FAILED(rc))
    seal_fail(0xDC23, "svcUnmapProcessCodeMemory (temporary)", rc, mod);
  virtmemLock();
  virtmemRemoveReservation(mod->tmp_rv);
  virtmemUnlock();
  mod->tmp_rv = NULL;
  mod->tmp_code = NULL;
  /* The pages are staging (heap) again, with the hooker's writes in them. */
  mod->syms_live = 0;
  const uintptr_t delta = (uintptr_t)mod->load_virtbase - (uintptr_t)mod->load_base;
  mod->syms = (Elf32_Sym *)((uintptr_t)mod->syms - delta);
  mod->dynstrtab = (char *)((uintptr_t)mod->dynstrtab - delta);
}

void so_finalize(so_module *mod) {
  if (mod->state == SO_SEALED)
    return;
  if (dcr_is_emulator()) {
    /* Code runs from the staging buffer (see so_load); nothing to map. */
    emu_strip_thumb_barriers(mod);
    so_flush_caches(mod);
    mod->state = SO_SEALED;
    return;
  }
  if (mod->state == SO_WRITABLE)
    end_writable(mod);
  Result rc = svcMapProcessCodeMemory(dcr_self_process(),
                                      (u64)(uintptr_t)mod->load_virtbase,
                                      (u64)(uintptr_t)mod->load_base, mod->load_size);
  if (R_FAILED(rc))
    fatal_error("svcMapProcessCodeMemory failed: 0x%08x", rc);

  /* RX must be set first from the freshly-mapped state (the kernel refuses
   * W->X). Build a per-page executable map so each page gets one permission. */
  const size_t npages = mod->load_size / 0x1000;
  uint8_t *is_x = calloc(npages, 1);
  if (!is_x)
    fatal_error("so_finalize: out of memory");
  for (int i = 0; i < mod->phnum; i++) {
    const Elf32_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X))
      continue;
    /* mod->phdr is the pristine copy: link-time p_vaddr, i.e. module offsets */
    const uintptr_t v = p->p_vaddr;
    size_t first = v / 0x1000;
    size_t last = (ALIGN_MEM(v + p->p_memsz, 0x1000) / 0x1000) - 1;
    for (size_t pg = first; pg <= last && pg < npages; pg++)
      is_x[pg] = 1;
  }
  for (int want_x = 1; want_x >= 0; want_x--) {
    size_t pg = 0;
    while (pg < npages) {
      if (is_x[pg] != want_x) { pg++; continue; }
      size_t end = pg;
      while (end < npages && is_x[end] == want_x)
        end++;
      const u64 addr = (u64)(uintptr_t)mod->load_virtbase + (u64)pg * 0x1000;
      const u64 size = (u64)(end - pg) * 0x1000;
      rc = svcSetProcessMemoryPermission(dcr_self_process(), addr, size,
                                         want_x ? Perm_Rx : Perm_Rw);
      if (R_FAILED(rc)) {
        /* Say what the kernel thinks is there: a permission change fails on
         * state/attribute, and the numbers are the only way to tell which. */
        MemoryInfo mi;
        u32 pi = 0;
        svcQueryMemory(&mi, &pi, (uintptr_t)addr);
        debugPrintf("[so] perm change failed: want %s at 0x%08x+0x%x; kernel says "
                    "0x%08x+0x%x type=0x%x perm=%x attr=%x\n",
                    want_x ? "RX" : "RW", (uint32_t)addr, (uint32_t)size,
                    (uint32_t)mi.addr, (uint32_t)mi.size, (unsigned)mi.type,
                    (unsigned)mi.perm, (unsigned)mi.attr);
        if (!dcr_is_emulator())
          fatal_error("so_finalize: perm %s at 0x%08x failed: 0x%08x",
                      want_x ? "RX" : "RW", (uint32_t)addr, rc);
        /* Emulator: it does not enforce guest page permissions, so the mapped
         * code runs regardless. Hardware never takes this branch. */
        debugPrintf("[so] emulator: continuing without the permission change\n");
      }
      pg = end;
    }
  }
  free(is_x);

  /* load_base is now donated to the code mapping and faults on access; rebase
   * the symbol pointers to the live RX image for post-finalize lookups. */
  syms_go_live(mod);
  mod->state = SO_SEALED;
}

int g_so_trace_ctors; /* log each constructor before it runs (bring-up) */

void so_execute_init_array(so_module *mod) {
  if (mod->inited)
    return;
  mod->inited = 1;
  for (int i = 0; i < mod->elf_hdr->e_shnum; i++) {
    const char *sh = mod->shstrtab + mod->sec_hdr[i].sh_name;
    if (!strcmp(sh, ".init_array")) {
      void (**arr)(void) = (void *)((uintptr_t)mod->load_virtbase + mod->sec_hdr[i].sh_addr);
      int n = (int)(mod->sec_hdr[i].sh_size / sizeof(void *));
      for (int j = 0; j < n; j++)
        if (arr[j] && (uintptr_t)arr[j] != 0xffffffffu) {
          if (g_so_trace_ctors)
            debugPrintf("[so] %s constructor %d/%d at +0x%lx\n", mod->base_name, j + 1, n,
                        (unsigned long)((uintptr_t)arr[j] - (uintptr_t)mod->load_virtbase));
          arr[j]();
        }
    }
  }
}

/* ------------------------------------------------------------------------- */
uintptr_t so_try_find_addr_rx(so_module *mod, const char *symbol) {
  int i = find_def(mod, symbol);
  return i < 0 ? 0 : (uintptr_t)mod->load_virtbase + mod->syms[i].st_value;
}

uintptr_t so_find_addr_rx(so_module *mod, const char *symbol) {
  uintptr_t a = so_try_find_addr_rx(mod, symbol);
  if (!a)
    fatal_error("so_util: symbol not found: %s", symbol);
  return a;
}

so_module *so_first(void) { return so_list; }

so_module *so_find_module_by_name(const char *name) {
  if (!name)
    return NULL;
  const char *base = strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
  for (so_module *m = so_list; m; m = m->next)
    if (!strcmp(m->base_name, base))
      return m;
  return NULL;
}

const Elf32_Phdr *so_segment_of(so_module *mod, uintptr_t addr) {
  if (!mod)
    return NULL;
  const uintptr_t off = addr - (uintptr_t)mod->load_virtbase;
  for (int i = 0; i < mod->phnum; i++) {
    const Elf32_Phdr *p = &mod->phdr[i];
    if (p->p_type == PT_LOAD && off >= (p->p_vaddr & ~0xFFFu) &&
        off < ALIGN_MEM(p->p_vaddr + p->p_memsz, 0x1000))
      return p;
  }
  return NULL;
}

DynLibFunction *so_find_import(DynLibFunction *funcs, int n, const char *name) {
  for (int i = 0; i < n; i++)
    if (!strcmp(funcs[i].symbol, name))
      return &funcs[i];
  return NULL;
}

so_module *so_find_module_by_addr(const void *addr) {
  const uintptr_t a = (uintptr_t)addr;
  for (so_module *m = so_list; m; m = m->next) {
    const uintptr_t b = (uintptr_t)m->load_virtbase;
    if (a >= b && a < b + m->load_size)
      return m;
  }
  return NULL;
}

/* /proc/self/maps-style listing for the Boehm GC's dynamic-root scan. */
int so_dump_maps(char *buf, size_t cap) {
  if (!buf || !cap)
    return 0;
  /* One line per PT_LOAD, with its real permissions: Mono's Boehm GC reads
   * this file (USE_PROC_FOR_LIBRARIES) and registers every writable mapping
   * as a root set, so .data/.bss of each module must appear as "rw-p". */
  size_t off = 0;
  for (so_module *m = so_list; m; m = m->next) {
    const char *name = strrchr(m->name, '/') ? strrchr(m->name, '/') + 1 : m->name;
    for (int i = 0; i < m->phnum; i++) {
      const Elf32_Phdr *ph = &m->phdr[i];
      if (ph->p_type != PT_LOAD || !ph->p_memsz)
        continue;
      uint32_t s = (uint32_t)(uintptr_t)m->load_virtbase + (ph->p_vaddr & ~0xFFFu);
      uint32_t e = ((uint32_t)(uintptr_t)m->load_virtbase + ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFu;
      int n = snprintf(buf + off, cap - off, "%08lx-%08lx %c%c%cp %08lx 00:00 0 /data/app/lib/arm/%s\n",
                       (unsigned long)s, (unsigned long)e, (ph->p_flags & PF_R) ? 'r' : '-',
                       (ph->p_flags & PF_W) ? 'w' : '-', (ph->p_flags & PF_X) ? 'x' : '-',
                       (unsigned long)(ph->p_offset & ~0xFFFu), name);
      if (n < 0 || (size_t)n >= cap - off)
        goto done;
      off += (size_t)n;
    }
  }
done:
  buf[off < cap ? off : cap - 1] = '\0';
  return (int)off;
}

struct so_dl_phdr_info {
  Elf32_Addr dlpi_addr;
  const char *dlpi_name;
  const Elf32_Phdr *dlpi_phdr;
  Elf32_Half dlpi_phnum;
};

int so_dl_iterate_phdr(int (*cb)(void *info, size_t size, void *data), void *data) {
  int ret = 0;
  for (so_module *m = so_list; m; m = m->next) {
    struct so_dl_phdr_info info = {
        .dlpi_addr = (Elf32_Addr)(uintptr_t)m->load_virtbase,
        .dlpi_name = m->name,
        .dlpi_phdr = m->phdr,
        .dlpi_phnum = m->phnum,
    };
    ret = cb(&info, sizeof(info), data);
    if (ret)
      break;
  }
  return ret;
}

/* svcMapProcessMemory / svcUnmapProcessMemory of our own [src, src+len) at
 * alias, one kernel memory block per call: the kernel only builds a page group
 * from a range whose blocks share one state, permission and attribute set
 * (KPageTableBase::CheckMemoryState), so a range spanning, e.g., an RX and an
 * RW block must be split (crt0_reloc.c hit exactly this). Returns the kernel
 * result; *done is how many bytes were (un)mapped before any failure. */
static Result alias_blocks(bool map, void *alias, uintptr_t src, size_t len, size_t *done) {
  Result rc = 0;
  *done = 0;
  while (*done < len) {
    MemoryInfo mi;
    u32 pi;
    const uintptr_t a = src + *done;
    rc = svcQueryMemory(&mi, &pi, a);
    if (R_FAILED(rc))
      break;
    const u64 block_end = (u64)mi.addr + mi.size;
    size_t n = len - *done;
    if (block_end > a && block_end - a < n)
      n = (size_t)(block_end - a);
    void *d = (uint8_t *)alias + *done;
    rc = map ? svcMapProcessMemory(d, dcr_self_process(), (u64)a, n)
             : svcUnmapProcessMemory(d, dcr_self_process(), (u64)a, n);
    if (R_FAILED(rc))
      break;
    *done += n;
  }
  return rc;
}

Result so_alias_map(void *dst, uintptr_t src, size_t len) {
  size_t done = 0;
  return alias_blocks(true, dst, src, len, &done);
}

Result so_alias_unmap(void *dst, uintptr_t src, size_t len) {
  size_t done = 0;
  return alias_blocks(false, dst, src, len, &done);
}

/* Runtime code patcher: RX code is not writable (and must never be made
 * writable: Mesosphere turns writable code into *Data state, which can never
 * be executable again), so map a temporary RW alias of the containing pages
 * with svcMapProcessMemory, write through it, unmap, and flush. */
int so_patch_code(void *dst, const void *src, size_t len) {
  so_module *owner = so_find_module_by_addr(dst);
  if (owner && owner->state == SO_WRITABLE) {
    memcpy(dst, src, len); /* the writable phase: nothing is executable yet */
    return 0;
  }
  if (dcr_is_emulator()) {
    /* Code lives in RW heap pages there (see so_load). */
    memcpy(dst, src, len);
    dcr_code_flush(dst, len);
    return 0;
  }
  uintptr_t start = (uintptr_t)dst & ~0xFFFu;
  size_t maplen = ((((uintptr_t)dst + len) - start) + 0xFFF) & ~0xFFFu;
  size_t off = (uintptr_t)dst - start;
  virtmemLock();
  void *alias = virtmemFindAslr(maplen, 0);
  VirtmemReservation *rv = alias ? virtmemAddReservation(alias, maplen) : NULL;
  virtmemUnlock();
  if (!alias) {
    debugPrintf("[patch] %p: no free address range for the alias\n", dst);
    return -1;
  }
  size_t mapped = 0, unmapped = 0;
  Result rc = alias_blocks(true, alias, start, maplen, &mapped);
  if (R_SUCCEEDED(rc))
    memcpy((uint8_t *)alias + off, src, len);
  else
    debugPrintf("[patch] %p: svcMapProcessMemory failed: 0x%x at +0x%x\n", dst, rc,
                (unsigned)mapped);
  Result urc = mapped ? alias_blocks(false, alias, start, mapped, &unmapped) : 0;
  if (R_FAILED(urc))
    debugPrintf("[patch] %p: svcUnmapProcessMemory failed: 0x%x\n", dst, urc);
  virtmemLock();
  if (rv && R_SUCCEEDED(urc))
    virtmemRemoveReservation(rv); /* keep a range that is still mapped reserved */
  virtmemUnlock();
  if (R_FAILED(rc))
    return -2;
  dcr_code_flush(dst, len);
  return 0;
}

int so_unload(so_module *mod) {
  if (!mod->load_base)
    return -1;
  if (mod->so_base)
    so_free_temp(mod);
  for (int i = 0; i < mod->phnum; i++) {
    const Elf32_Phdr *p = &mod->phdr[i];
    if (p->p_type != PT_LOAD || !(p->p_flags & PF_X))
      continue;
    const u64 s = (u64)(uintptr_t)mod->load_virtbase + p->p_vaddr;
    const u64 e = ALIGN_MEM((u64)s + p->p_memsz, 0x1000);
    svcSetProcessMemoryPermission(dcr_self_process(), s & ~0xFFFull, e - (s & ~0xFFFull), Perm_Rw);
  }
  svcUnmapProcessCodeMemory(dcr_self_process(), (u64)(uintptr_t)mod->load_virtbase,
                            (u64)(uintptr_t)mod->load_base, mod->load_size);
  virtmemLock();
  if (mod->load_memrv)
    virtmemRemoveReservation(mod->load_memrv);
  virtmemUnlock();
  if (so_list == mod) {
    so_list = mod->next;
  } else {
    for (so_module *m = so_list; m; m = m->next)
      if (m->next == mod) {
        m->next = mod->next;
        break;
      }
  }
  return 0;
}
