/* rt_pad.c -- the controller pieces every port had its own copy of.
 *
 * WHICH SLOTS: which player slots may take a controller is the application's
 * call (hid SetSupportedNpadIdType); a controller with no slot to go to --
 * Joy-Cons taken off the console, a Pro Controller -- keeps searching.
 * padConfigureInput() sends that list, but libnx32 before 4.12.0 was built
 * for arm-none-eabi, whose enums are as small as their values allow
 * (Tag_ABI_enum_size: small): HidNpadIdType was one byte, so the list {No1..
 * No8, Handheld} left as 9 bytes, which hid read as two 32-bit IDs,
 * 0x03020100 and 0x07060504 -- no real player slot. The call still
 * succeeded, so attached Joy-Cons (the handheld slot) kept working and
 * nothing else could connect (hardware 2026-09-25). rt_pad_setup sends the
 * list again as 32-bit IDs, which is right with any libnx32.
 *
 * ONE SLOT: a PadState made by padInitializeDefault reads player 1 and the
 * attached Joy-Cons as one; rt_pad_slot reads exactly one slot, so the port
 * decides who plays (the first connected in its own order, one per player).
 *
 * A LONE JOY-CON (local multiplayer hands one to each player) is held
 * sideways with its rail up, as the console's own games take it: its face
 * buttons and stick turn with it. MIT.
 */
#include <stdio.h>

#include "rt_pad.h"
#include "util.h"

HidNpadIdType rt_pad_slot_id(int slot) {
  return slot >= 0 && slot < 8 ? (HidNpadIdType)(HidNpadIdType_No1 + slot) : HidNpadIdType_Handheld;
}

/* hid SetSupportedNpadIdType (command 102), with 32-bit IDs */
static Result set_supported_npad_ids(const u32 *ids, u32 n) {
  u64 aruid = appletGetAppletResourceUserId();
  return serviceDispatchIn(hidGetServiceSession(), 102, aruid,
                           .buffer_attrs = {SfBufferAttr_HipcPointer | SfBufferAttr_In},
                           .buffers = {{ids, n * sizeof(u32)}}, .in_send_pid = true);
}

Result rt_pad_setup(int max_players, int handheld) {
  if (max_players <= 0)
    max_players = RT_PAD_MAX_PLAYERS;
  if (max_players > 8)
    max_players = 8;
  if (max_players < 1)
    max_players = 1;
  u32 style = HidNpadStyleSet_NpadStandard;
  if (!handheld)
    style &= ~(u32)HidNpadStyleTag_NpadHandheld;
  padConfigureInput((u32)max_players, style);
  /* players 1..n = 0..n-1, handheld = 0x20: the IDs as hid reads them */
  u32 ids[9];
  u32 n = 0;
  for (int i = 0; i < max_players; i++)
    ids[n++] = (u32)i;
  if (handheld)
    ids[n++] = 0x20;
  Result rc = set_supported_npad_ids(ids, n);
  char who[24];
  if (max_players > 1)
    snprintf(who, sizeof who, "players 1-%d", max_players);
  else
    snprintf(who, sizeof who, "player 1");
  debugPrintf("[input] controllers allowed: %s%s, any style (%s)\n", who, handheld ? " and handheld" : "",
              R_SUCCEEDED(rc) ? "ok" : "hid refused it");
  if (R_FAILED(rc))
    debugPrintf("[input]   SetSupportedNpadIdType failed 0x%x: only the attached Joy-Cons may work\n",
                (unsigned)rc);
  return rc;
}

void rt_pad_slot(PadState *pad, int slot) {
  padInitializeWithMask(pad, slot >= 0 && slot < 8 ? BITL(slot) : BITL(HidNpadIdType_Handheld));
}

int rt_pad_is_single(u64 st) {
  return (st & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) &&
         !(st & (HidNpadStyleTag_NpadHandheld | HidNpadStyleTag_NpadJoyDual | HidNpadStyleTag_NpadFullKey));
}

u64 rt_pad_single_buttons(u64 st, u64 b) {
  if (!rt_pad_is_single(st))
    return b;
  u64 o = b & ~(HidNpadButton_A | HidNpadButton_B | HidNpadButton_X | HidNpadButton_Y | HidNpadButton_Up |
                HidNpadButton_Down | HidNpadButton_Left | HidNpadButton_Right | HidNpadButton_Minus |
                HidNpadButton_StickL | HidNpadButton_StickR);
  if (st & HidNpadStyleTag_NpadJoyLeft) { /* Down at the right, Left below, Up at the left, Right on top */
    if (b & HidNpadButton_Down) o |= HidNpadButton_A;
    if (b & HidNpadButton_Left) o |= HidNpadButton_B;
    if (b & HidNpadButton_Up) o |= HidNpadButton_Y;
    if (b & HidNpadButton_Right) o |= HidNpadButton_X;
  } else { /* X at the right, A below, B at the left, Y on top */
    if (b & HidNpadButton_X) o |= HidNpadButton_A;
    if (b & HidNpadButton_A) o |= HidNpadButton_B;
    if (b & HidNpadButton_B) o |= HidNpadButton_Y;
    if (b & HidNpadButton_Y) o |= HidNpadButton_X;
  }
  /* SL / SR under the index fingers: L / R; its one of + and -: +; the
   * stick's click: ZL */
  if (b & (HidNpadButton_LeftSL | HidNpadButton_RightSL)) o |= HidNpadButton_L;
  if (b & (HidNpadButton_LeftSR | HidNpadButton_RightSR)) o |= HidNpadButton_R;
  if (b & (HidNpadButton_Minus | HidNpadButton_Plus)) o |= HidNpadButton_Plus;
  if (b & (HidNpadButton_StickL | HidNpadButton_StickR)) o |= HidNpadButton_ZL;
  return o;
}

void rt_pad_sideways(u64 st, float *x, float *y) {
  const float cx = *x, cy = *y;
  if (st & HidNpadStyleTag_NpadJoyLeft) /* turned anticlockwise */
    *x = -cy, *y = cx;
  else /* the right one, clockwise */
    *x = cy, *y = -cx;
}

u64 rt_pad_read(PadState *pad, float sticks[4]) {
  const u64 st = padGetStyleSet(pad);
  const int single = rt_pad_is_single(st);
  u64 b = padGetButtons(pad);
  if (single)
    b = rt_pad_single_buttons(st, b);
  if (sticks) {
    /* a lone right Joy-Con's one stick is its "right" one */
    const int right_alone = single && (st & HidNpadStyleTag_NpadJoyRight);
    HidAnalogStickState l = padGetStickPos(pad, right_alone ? 1 : 0), r = padGetStickPos(pad, 1);
    sticks[0] = (float)l.x / (float)JOYSTICK_MAX, sticks[1] = (float)l.y / (float)JOYSTICK_MAX;
    sticks[2] = single ? 0.0f : (float)r.x / (float)JOYSTICK_MAX;
    sticks[3] = single ? 0.0f : (float)r.y / (float)JOYSTICK_MAX;
    if (single)
      rt_pad_sideways(st, &sticks[0], &sticks[1]);
  }
  return b;
}

const char *rt_pad_style_name(u64 st) {
  return (st & HidNpadStyleTag_NpadFullKey)    ? "Pro Controller"
         : (st & HidNpadStyleTag_NpadHandheld) ? "Joy-Cons, attached"
         : (st & HidNpadStyleTag_NpadJoyDual)  ? "Joy-Con pair"
         : (st & HidNpadStyleTag_NpadJoyLeft)  ? "left Joy-Con"
         : (st & HidNpadStyleTag_NpadJoyRight) ? "right Joy-Con"
         : (st & HidNpadStyleTag_NpadGc)       ? "GameCube controller"
         : st                                  ? "controller"
                                               : "none";
}
