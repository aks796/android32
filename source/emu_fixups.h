/* emu_fixups.h -- instructions an emulator cannot run, rewritten for it
 * (emu_fixups.c). EMULATOR ONLY: call these under dcr_is_emulator(), where a
 * module runs from its staging buffer (so_load), before so_finalize. MIT. */
#ifndef DCR_EMU_FIXUPS_H
#define DCR_EMU_FIXUPS_H
#include <stddef.h>
#include <stdint.h>

#include "so_util.h"

/* This program's own text (Mesa's fixed-point VCVTs). Every port's emulator
 * boot runs it before any of Mesa does, right after the log is up. */
int dcr_emu_fix_self(void);

/* An engine module with function symbols: fixed-point VCVT only, inside
 * sized functions. pool: at least 8 words per site, within 32 MB (branch
 * range) of the module's code, never freed. Returns the sites rewritten. */
int dcr_emu_fix_vcvt(so_module *m, uint32_t *pool, size_t pool_words);

/* A stripped engine module (no function symbols): VCVT, VSWP and the NEON
 * forms the decoder lacks, in .text, skipping pc-relative literals. pool: up
 * to 10 words per site, within 32 MB of the code. Returns the sites rewritten. */
int dcr_emu_fix_module(so_module *m, uint32_t *pool, size_t pool_words);

#endif
