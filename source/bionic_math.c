/* bionic_math.c -- the <math.h> imports, on the VFP unit where it matters.
 *
 * devkitARM's newlib/libm is a soft-float build: every double operation in it
 * is a libgcc __aeabi_d* call. The calling convention is the same one the game
 * uses (softfp: VFP instructions, arguments in core registers), so newlib's
 * functions are *correct* to hand over; they are just slow. The ones the engine
 * and Mono's Math icalls hit per frame and that the Cortex-A57 does in a single
 * instruction (sqrt, abs, the ARMv8 VRINT rounding family, min/max) are done
 * here in VFP; transcendental functions pass straight through to newlib.
 *
 * Every function is an explicit instruction, not a __builtin: builtins keep the
 * errno-setting library call for the NaN case unless -fno-math-errno, and the
 * game does not read errno after math. MIT.
 */
#include <math.h>

#define VF64_1(name, insn)                                            \
  double b_##name(double x) {                                         \
    double r;                                                         \
    __asm__(insn ".f64 %P0, %P1" : "=w"(r) : "w"(x));                 \
    return r;                                                         \
  }
#define VF32_1(name, insn)                                            \
  float b_##name(float x) {                                           \
    float r;                                                          \
    __asm__(insn ".f32 %0, %1" : "=t"(r) : "t"(x));                   \
    return r;                                                         \
  }

VF64_1(sqrt, "vsqrt")
VF32_1(sqrtf, "vsqrt")
VF32_1(fabsf, "vabs")
VF64_1(floor, "vrintm")
VF32_1(floorf, "vrintm")
VF64_1(ceil, "vrintp")
VF32_1(ceilf, "vrintp")
VF64_1(trunc, "vrintz")
VF32_1(truncf, "vrintz")
VF64_1(round, "vrinta")   /* ties away from zero, as C round() */
VF32_1(roundf, "vrinta")
VF64_1(rint, "vrintx")    /* current rounding mode, raises inexact */
VF32_1(rintf, "vrintx")

float b_fmaxf(float a, float b) {
  float r;
  __asm__("vmaxnm.f32 %0, %1, %2" : "=t"(r) : "t"(a), "t"(b)); /* NaN-ignoring, as fmaxf */
  return r;
}
float b_fminf(float a, float b) {
  float r;
  __asm__("vminnm.f32 %0, %1, %2" : "=t"(r) : "t"(a), "t"(b));
  return r;
}

long b_lroundf(float x) {
  float t;
  int r;
  __asm__("vcvta.s32.f32 %1, %2\n\tvmov %0, %1" : "=r"(r), "=&t"(t) : "t"(x));
  return r;
}

double b_modf(double x, double *ip) {
  double i = b_trunc(x);
  *ip = i;
  return isinf(x) ? copysign(0.0, x) : x - i;
}
float b_modff(float x, float *ip) {
  float i = b_truncf(x);
  *ip = i;
  return isinf(x) ? copysignf(0.0f, x) : x - i;
}

/* bionic's libm: the isfinite() of a float (NDK r5-r8 <math.h> calls it). */
int b___isfinitef(float f) { return isfinite(f); }
