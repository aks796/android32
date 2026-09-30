/* config.h -- build-wide constants for the Labyrinth 2 Switch wrapper.
 *
 * Labyrinth 2 (se.illusionlabs.labyrinth2 1.29, versionCode 20), armeabi:
 * Illusion Labs' own C++ engine (GLES 1.x), one native library; the menus
 * were Android Views, rebuilt natively here (lab_ui.c). AArch32 host process
 * built against libnx32 (see the Makefile). MIT.
 */
#ifndef DCR_CONFIG_H
#define DCR_CONFIG_H

/* Where on the SD card the game files live, and the module name. Builds
 * before 2026-09-30 used /switch/labyrinth2 (their files are moved over on
 * the first start: main.c). */
#define DCR_ROOT_PATH   "/switch/labyrinth2_nx"
#define DCR_OLD_ROOT_PATH "/switch/labyrinth2"
#define LAB_LIB_GAME    "liblabyrinthii.so"
#define LAB_ABI_DIR     "lib/armeabi/"
#define DCR_APK_NAME    "game.apk"   /* the user's own APK: this name first, else any *.apk that is Labyrinth 2's */
#define DCR_PACKAGE     "se.illusionlabs.labyrinth2"
#define LAB_VERSION     "1.29"
#define LAB_VERSION_CODE 20
#define LAB_NSP_NAME    "labyrinth2_nx.nsp" /* in the launcher's romfs */

/* The reserved region the game module is mapped into. liblabyrinthii.so 1.29:
 * highest p_vaddr+p_memsz = 0x4f448 (~317 KB). */
#define SO_REGION_BYTES (4u * 1024 * 1024)

/* Left outside the heap for kernel-side allocations. GPU buffers come from
 * the heap (libdrm_nouveau memaligns them and hands them to nvmap). */
#define GFX_RESERVE_MB  16u

/* Default window size (config.ini [display] resolution changes it). The
 * game and the menus are portrait: they draw into a 2:3 surface in the
 * middle of it (lab_gfx.c). */
#define DCR_FORCE_SCREEN_W 1280
#define DCR_FORCE_SCREEN_H 720

#define DEBUG_LOG 1

/* The renderer: 1 = mesa/nouveau (gl_mesa.c, portlibs32/ from
 * mesa32), 0 = null GL (gl_null.c: runs the game, draws
 * nothing). Set by the Makefile. */
#ifndef DCR_GL_MESA
#define DCR_GL_MESA 0
#endif

#endif /* DCR_CONFIG_H */
