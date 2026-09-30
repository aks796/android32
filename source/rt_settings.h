/* rt_settings.h -- the port's settings, with the runtime's defaults.
 *
 * Every runtime file that depends on the game includes this header instead of
 * a port's config.h. It includes the port's own port_config.h (found first on
 * the include path: runtime.mk puts the port's source/ ahead of the
 * runtime's), then fills in the defaults below. A setting only one runtime
 * file reads has its #ifndef default in that file, next to the code; this
 * header holds the ones several files share. docs/SETTINGS.md lists them all.
 *
 * port_config.h holds macros only: the assembler reads it too (exc32.S).
 *
 * Naming: PORT_* names the game and where it lives; RT_* changes how the
 * runtime behaves; port_*() are weak callbacks a port may define. MIT.
 */
#ifndef RT_SETTINGS_H
#define RT_SETTINGS_H

#include "port_config.h"

/* ---------------------------------------------------------------- required */
#ifndef PORT_TITLE
#error "port_config.h: define PORT_TITLE, the game's name (\"Labyrinth 2\")"
#endif
#ifndef PORT_NAME
#error "port_config.h: define PORT_NAME, the NRO and SD folder name (\"labyrinth2_nx\")"
#endif
#ifndef PORT_PACKAGE
#error "port_config.h: define PORT_PACKAGE, the Android package (\"se.illusionlabs.labyrinth2\")"
#endif
#ifndef PORT_PAYLOAD_NAME
#error "PORT_PAYLOAD_NAME (the 32-bit program's name, the Makefile's TARGET) is set by runtime.mk"
#endif

/* ---------------------------------------------------------------- identity */
/* The game folder on the SD card, without "sdmc:". */
#ifndef PORT_ROOT_PATH
#define PORT_ROOT_PATH "/switch/" PORT_NAME
#endif
/* PORT_OLD_ROOT_PATHS (optional): earlier names of the game folder, as a
 * list of strings, e.g.  #define PORT_OLD_ROOT_PATHS "/switch/labyrinth2"
 * Their contents are moved into PORT_ROOT_PATH on the first start. */

/* The first line of debug.log and the boot console. */
#ifndef PORT_BANNER
#define PORT_BANNER PORT_NAME ": " PORT_TITLE
#endif
/* Under the title on the setup progress screen. */
#ifndef PORT_SETUP_NOTE
#define PORT_SETUP_NOTE "Getting the game ready (after an install or an update)"
#endif
/* The 32-bit program inside the launcher NRO's romfs. */
#ifndef PORT_NSP_NAME
#define PORT_NSP_NAME PORT_PAYLOAD_NAME ".nsp"
#endif
/* Where the game's native libraries are inside the APK. */
#ifndef PORT_ABI_DIR
#define PORT_ABI_DIR "lib/armeabi-v7a/"
#endif
/* The name the game's APK is given (or looked for first) in the folder. */
#ifndef PORT_APK_DEFAULT_NAME
#define PORT_APK_DEFAULT_NAME "game.apk"
#endif
/* The reserved region the game's modules are mapped into: the sum of their
 * highest p_vaddr + p_memsz, rounded up. */
#ifndef PORT_SO_REGION_BYTES
#define PORT_SO_REGION_BYTES (32u * 1024 * 1024)
#endif

/* ---------------------------------------------------------------- features */
/* OpenSL ES: 0 = slCreateEngine refuses (android_ndk.c), so the game falls
 * back to its Java/AudioTrack path; 1 = opensles.c implements it on audout
 * (a8r). */
#ifndef RT_OPENSLES
#define RT_OPENSLES 0
#endif

/* ---------------------------------------------------------------- platform */
/* Left outside the heap for kernel-side allocations. */
#ifndef RT_GFX_RESERVE_MB
#define RT_GFX_RESERVE_MB 16u
#endif
/* The default window size; config.ini [display] resolution changes it. */
#ifndef RT_SCREEN_W
#define RT_SCREEN_W 1280
#endif
#ifndef RT_SCREEN_H
#define RT_SCREEN_H 720
#endif
/* The renderer: 1 = mesa/nouveau (gl_mesa.c, portlibs32/), 0 = null GL
 * (gl_null.c). Set by runtime.mk. */
#ifndef DCR_GL_MESA
#define DCR_GL_MESA 0
#endif

#endif /* RT_SETTINGS_H */
