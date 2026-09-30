/* rt_pad.h -- the controllers: which player slots may take one, reading one
 * slot alone, and a lone Joy-Con held sideways (rt_pad.c). What the buttons
 * do, touch, tilt and cursors stay the port's. MIT.
 */
#ifndef RT_PAD_H
#define RT_PAD_H
#include <switch.h>

#include "rt_settings.h"

/* Player slots the game takes: 1 to 8. Values: dcr 8, a8r 8, flappy 8,
 * lab2 2, pvz 2, sonic 2, abs 1. */
#ifndef RT_PAD_MAX_PLAYERS
#define RT_PAD_MAX_PLAYERS 8
#endif

/* Slots: 0-7 are players 1-8, RT_PAD_HANDHELD the Joy-Cons on the console. */
#define RT_PAD_HANDHELD 8
#define RT_PAD_SLOTS 9

/* Once, before reading any pad: players 1..max_players (<= 0:
 * RT_PAD_MAX_PLAYERS; clamped to 1-8), plus the attached Joy-Cons when
 * handheld, any standard style. padConfigureInput, then the same list again
 * as 32-bit IDs (hid command 102). The result of the second, logged. */
Result rt_pad_setup(int max_players, int handheld);

/* pad reads one slot alone (libnx's padInitializeDefault / Any OR several
 * controllers together). Then padUpdate as usual. */
void rt_pad_slot(PadState *pad, int slot);
/* A slot's hid ID. */
HidNpadIdType rt_pad_slot_id(int slot);

/* One Joy-Con alone (not attached, not a pair): the player holds it sideways,
 * its rail up. */
int rt_pad_is_single(u64 style);
/* A lone Joy-Con's buttons as that hold has them: the face button at the
 * right is A (then B below, Y left, X on top), SL / SR are L / R, its + or -
 * is +, a click of its stick is ZL. Other styles' buttons unchanged. */
u64 rt_pad_single_buttons(u64 style, u64 buttons);
/* A vector in a lone Joy-Con's own frame (x right, y up, held upright) turned
 * to the sideways hold's: the left one anticlockwise, the right one
 * clockwise. */
void rt_pad_sideways(u64 style, float *x, float *y);
/* A pad's buttons and sticks as its player holds it (sticks -1..1, y up:
 * left x, y, right x, y). A lone Joy-Con: rt_pad_single_buttons, its one
 * stick as the left one turned sideways, no right stick. */
u64 rt_pad_read(PadState *pad, float sticks[4]);

/* For the log: "Pro Controller", "Joy-Cons, attached", "Joy-Con pair",
 * "left Joy-Con", "right Joy-Con", "GameCube controller", "controller", or
 * "none" (0). */
const char *rt_pad_style_name(u64 style);

#endif
