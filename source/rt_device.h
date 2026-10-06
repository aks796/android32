/* rt_device.h -- this console's id, for a game's online account, and SHA-256.
 *
 * rt_device_id() is 8-16 lower-case hex digits (16 when it is made here),
 * the same on every start: a game's server knows the player by it. In order:
 *   1. port_device_id_override(), when it returns 8-16 hex digits (lab2:
 *      config.ini [online] device_id), lower-cased;
 *   2. <game folder>/data/device_id, its first line, when that is 8-16 hex
 *      digits, as it is;
 *   3. made, then kept in data/device_id: the first 8 bytes of SHA-256 of
 *      RT_DEVICE_ID_SALT ":" the console's serial number (when the console
 *      shows it), else RT_DEVICE_ID_SALT ":user:" and the console user's id,
 *      else a random number.
 * Byte for byte Labyrinth 2's lab_online_device_id(): players' accounts
 * depend on it. Thread-safe; the first call does the work. MIT.
 */
#ifndef RT_DEVICE_H
#define RT_DEVICE_H
#include <stddef.h>
#include <stdint.h>

const char *rt_device_id(void);

/* SHA-256 (FIPS 180-4) of n bytes. libnx32 has none (the 64-bit libnx's
 * uses AArch64 crypto instructions). */
void rt_sha256(const void *data, size_t n, uint8_t out[32]);

/* CALLBACK (weak, default NULL): an id the player set, used when it is 8-16
 * hex digits. */
const char *port_device_id_override(void);

#endif
