/* rt_boot.h -- the game program's start-up: the steps of the runtime's
 * main() that a port's own main.c can call too (rt_boot.c), and the
 * callbacks main() calls. MIT.
 */
#ifndef RT_BOOT_H
#define RT_BOOT_H

#include "rt_apkfind.h"
#include "rt_migrate.h"

/* ---------------------------------------------------------------- helpers */
/* The first lines of debug.log: PORT_BANNER, the text relocations, the heap,
 * the services (and port_report_boot()). */
void dcr_report_boot(void);

/* The old game folders (PORT_OLD_ROOT_PATHS) moved into dcr_game_root().
 * Before anything writes there (before log_init): the result is kept for
 * rt_boot_migrate_report(), which logs it once the log is open. */
int rt_boot_migrate(void);
void rt_boot_migrate_report(void);
const RtMigrateResult *rt_boot_migrate_result(void);

/* The APKs in the game folder, by the port's role table (port_apk_roles()),
 * adopted (renamed) where a role says so. The game's (role 0) becomes
 * dcr_apk_path(). 0 when every role but the optional ones has one. Run again
 * after anything adds files to the folder. */
int rt_boot_find_apks(void);

/* Refuses to run in a forwarder icon made for another NRO (sphaira's own, say,
 * where an older launcher installed the game): removes the game from that
 * icon and restarts it. Returns when the icon is this game's, or cannot be
 * told. */
void rt_boot_check_title(void);
/* The NRO this forwarder icon starts (its romfs /nextNroPath), as an SD path
 * without "sdmc:", e.g. "/switch/x/x.nro". 0, or -1 when the running title
 * is not a forwarder or names none. */
int rt_boot_icon_nro(char *out, size_t cap);
/* Role i's APK ("" when none), and what the folder holds, for messages
 * ("a.apk (the game), b.apk (not the game)"; "no APK at all"). */
const char *dcr_apk_role_path(int role);
const char *dcr_apk_summary(void);

/* ---------------------------------------------------------------- the port */
/* REQUIRED with the runtime's main(). port_load: everything between the APK
 * and the game's first code -- setup from the APK, loading the modules,
 * patches, the port's own files. 0 to go on; non-zero to exit quietly (a
 * start screen's "exit"). port_run: the constructors and the game, until it
 * ends. */
int port_load(const char *apk);
void port_run(void);

/* CALLBACKS (weak; the defaults do nothing / say the generic thing): */
void port_report_boot(void);     /* more boot lines, after the relocation line */
void port_before_update(void);   /* before the NRO self-update (a8r: new zips -> the launcher) */
int port_after_apk_find(void);   /* after the APK search: 1 = files were added, search again
                                    (pvz: the English APK out of the NRO) */
const char *port_apk_help(void); /* "Copy the APK of your own ..." for the error screens */
/* The APK roles (default: PORT_APK_ROLES, else RT_APK_ROLE_DEFAULT). */
const RtApkRole *port_apk_roles(int *count);

#endif /* RT_BOOT_H */
