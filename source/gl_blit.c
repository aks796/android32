/* gl_blit.c -- a picture drawn over the game's frame before it is presented
 * (the intro video, the pointer), on the engine's thread with its GLES 1
 * context, everything it changes put back.
 *
 * The way the engine draws its own pictures: a textured quad through
 * glDrawArrays with client arrays, under an orthographic projection of the
 * window. The first version used glDrawTexiOES, and on the Switch's Mesa the
 * screen stayed black: the engine culls clockwise-facing triangles
 * (glFrontFace(GL_CW), a Direct3D-style state manager) and Mesa draws
 * glDrawTex as a counter-clockwise quad -- and with culling off, still black
 * (hardware run 12). So here every state that takes part in a draw is set
 * for it: culling, blending, alpha test, depth, stencil, scissor, fog,
 * lighting, logic op, the colour mask, clip plane 0, the other texture units,
 * the matrices, the viewport and the arrays; the current colour is magenta,
 * which only shows if the texture is not sampled (so the log can tell).
 * glDrawTexiOES stays as the second method.
 *
 * GLES 1 engines only, and only for ports that draw over the frame: the
 * body is compiled with RT_GL_BLIT. MIT.
 */
#include "rt_settings.h"

/* 1: the dcr_blit_* functions (gl_blit.h) are built. Values: pvz 1, sonic 1;
 * the others 0. */
#ifndef RT_GL_BLIT
#define RT_GL_BLIT 0
#endif

#if RT_GL_BLIT
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <string.h>

#include "gl_blit.h"
#include "gl_layer.h"
#include "util.h"

void dcr_window_size(int *w, int *h); /* rt_window.c (group C) */

#define F(ret, name, args) ret(*name) args
static struct {
  F(void, GetIntegerv, (GLenum, GLint *));
  F(void, GetFloatv, (GLenum, GLfloat *));
  F(void, GetBooleanv, (GLenum, GLboolean *));
  F(void, GetPointerv, (GLenum, void **));
  F(GLboolean, IsEnabled, (GLenum));
  F(void, Enable, (GLenum));
  F(void, Disable, (GLenum));
  F(void, MatrixMode, (GLenum));
  F(void, LoadIdentity, (void));
  F(void, LoadMatrixf, (const GLfloat *));
  F(void, Orthof, (GLfloat, GLfloat, GLfloat, GLfloat, GLfloat, GLfloat));
  F(void, Viewport, (GLint, GLint, GLsizei, GLsizei));
  F(void, ActiveTexture, (GLenum));
  F(void, ClientActiveTexture, (GLenum));
  F(void, BindTexture, (GLenum, GLuint));
  F(void, TexEnvi, (GLenum, GLenum, GLint));
  F(void, GetTexEnviv, (GLenum, GLenum, GLint *));
  F(void, TexParameteriv, (GLenum, GLenum, const GLint *));
  F(void, BindBuffer, (GLenum, GLuint));
  F(void, EnableClientState, (GLenum));
  F(void, DisableClientState, (GLenum));
  F(void, VertexPointer, (GLint, GLenum, GLsizei, const void *));
  F(void, TexCoordPointer, (GLint, GLenum, GLsizei, const void *));
  F(void, Color4f, (GLfloat, GLfloat, GLfloat, GLfloat));
  F(void, DrawArrays, (GLenum, GLint, GLsizei));
  F(void, DrawTexiOES, (GLint, GLint, GLint, GLint, GLint));
  F(void, ColorMask, (GLboolean, GLboolean, GLboolean, GLboolean));
  F(void, BlendFunc, (GLenum, GLenum));
  F(void, BlendFuncSeparateOES, (GLenum, GLenum, GLenum, GLenum));
  F(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *));
  F(GLenum, GetError, (void));
} G;
static int g_state; /* 0 not looked up, 1 ready, -1 missing */

#define L(name) (G.name = (void *)dcr_gl_lookup("gl" #name))
int dcr_blit_setup(const char *who) {
  if (g_state)
    return g_state;
  L(GetIntegerv), L(GetFloatv), L(GetBooleanv), L(GetPointerv), L(IsEnabled), L(Enable);
  L(Disable), L(MatrixMode), L(LoadIdentity), L(LoadMatrixf), L(Orthof), L(Viewport);
  L(ActiveTexture), L(ClientActiveTexture), L(BindTexture), L(TexEnvi), L(GetTexEnviv);
  L(TexParameteriv), L(BindBuffer), L(EnableClientState), L(DisableClientState);
  L(VertexPointer), L(TexCoordPointer), L(Color4f), L(DrawArrays), L(DrawTexiOES), L(ColorMask);
  L(BlendFunc), L(BlendFuncSeparateOES), L(ReadPixels), L(GetError);
  /* all but the extensions (the texture units' and blend's are looked up
   * but optional) */
  const void *need[] = {G.GetIntegerv, G.GetFloatv, G.GetBooleanv, G.GetPointerv, G.IsEnabled,
                        G.Enable, G.Disable, G.MatrixMode, G.LoadIdentity, G.LoadMatrixf,
                        G.Orthof, G.Viewport, G.BindTexture, G.TexEnvi, G.GetTexEnviv,
                        G.TexParameteriv, G.EnableClientState, G.DisableClientState,
                        G.VertexPointer, G.TexCoordPointer, G.Color4f, G.DrawArrays,
                        G.ColorMask, G.BlendFunc, G.ReadPixels, G.GetError};
  g_state = 1;
  for (unsigned i = 0; i < sizeof need / sizeof need[0]; i++)
    if (!need[i])
      g_state = -1;
  if (g_state < 0)
    debugPrintf("[%s] the GL driver lacks a basic GLES 1 call: nothing drawn\n", who);
  return g_state;
}

void dcr_blit_crop(GLuint tex, int tex_w, int tex_h) {
  GLint bound = 0;
  G.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
  G.BindTexture(GL_TEXTURE_2D, tex);
  const GLint crop[4] = {0, tex_h, tex_w, -tex_h}; /* the first row at the top */
  G.TexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, crop);
  G.BindTexture(GL_TEXTURE_2D, (GLuint)bound);
}

/* the capabilities turned off for the draw (blend is its own) */
static const GLenum k_off[] = {GL_CULL_FACE,  GL_ALPHA_TEST, GL_DEPTH_TEST,     GL_SCISSOR_TEST,
                               GL_STENCIL_TEST, GL_FOG,      GL_LIGHTING,       GL_COLOR_LOGIC_OP,
                               GL_CLIP_PLANE0,  GL_COLOR_MATERIAL, GL_POLYGON_OFFSET_FILL};
#define NOFF (sizeof k_off / sizeof k_off[0])
#define MAX_UNITS 8

typedef struct {
  GLint size, type, stride, buffer;
  void *ptr;
} Array;

static void get_array(Array *a, GLenum size, GLenum type, GLenum stride, GLenum buffer, GLenum ptr) {
  G.GetIntegerv(size, &a->size);
  G.GetIntegerv(type, &a->type);
  G.GetIntegerv(stride, &a->stride);
  G.GetIntegerv(buffer, &a->buffer);
  G.GetPointerv(ptr, &a->ptr);
}

static float g_alpha = 1.0f; /* dcr_blit_alpha's, for one draw */
static struct { /* dcr_blit_mesh's, for one draw */
  const GLfloat *pos, *uv;
  int n;
  int additive; /* light: added to what is there (a glow) */
  GLfloat rgba[4];
} g_mesh;

GLenum dcr_blit(GLuint tex, int x, int y, int w, int h, int blend, int method) {
  if (g_state <= 0 || w <= 0 || h <= 0)
    return 0;
  if (method == 1 && !G.DrawTexiOES)
    return GL_INVALID_OPERATION;
  int vw, vh;
  dcr_window_size(&vw, &vh);
  while (G.GetError() != GL_NO_ERROR) /* the engine's, not ours */
    ;

  /* ---- save ---- */
  GLint active = GL_TEXTURE0, client = GL_TEXTURE0, units = 1, mode = GL_MODELVIEW, vp[4];
  GLint bound = 0, env = GL_MODULATE, abuf = 0, src = GL_ONE, dst = GL_ZERO;
  GLint srgb = 0, drgb = 0, sa = 0, da = 0;
  GLfloat proj[16], model[16], texm[16], color[4];
  GLboolean mask[4], off_was[NOFF], unit_tex[MAX_UNITS] = {0};
  GLboolean blend_was, tex_was, va, ca, na, ta;
  Array vtx, tc;
  if (G.ActiveTexture) {
    G.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
    G.GetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    if (units > MAX_UNITS)
      units = MAX_UNITS;
    for (int i = 1; i < units; i++) { /* only unit 0 textures the draw */
      G.ActiveTexture(GL_TEXTURE0 + i);
      unit_tex[i] = G.IsEnabled(GL_TEXTURE_2D);
      if (unit_tex[i])
        G.Disable(GL_TEXTURE_2D);
    }
    G.ActiveTexture(GL_TEXTURE0);
  }
  if (G.ClientActiveTexture) {
    G.GetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &client);
    G.ClientActiveTexture(GL_TEXTURE0);
  }
  G.GetIntegerv(GL_MATRIX_MODE, &mode);
  G.GetFloatv(GL_PROJECTION_MATRIX, proj);
  G.GetFloatv(GL_MODELVIEW_MATRIX, model);
  G.GetFloatv(GL_TEXTURE_MATRIX, texm);
  G.GetIntegerv(GL_VIEWPORT, vp);
  G.GetBooleanv(GL_COLOR_WRITEMASK, mask);
  G.GetFloatv(GL_CURRENT_COLOR, color);
  G.GetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
  G.GetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env);
  G.GetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
  for (unsigned i = 0; i < NOFF; i++)
    off_was[i] = G.IsEnabled(k_off[i]);
  blend_was = G.IsEnabled(GL_BLEND);
  tex_was = G.IsEnabled(GL_TEXTURE_2D);
  va = G.IsEnabled(GL_VERTEX_ARRAY);
  ca = G.IsEnabled(GL_COLOR_ARRAY);
  na = G.IsEnabled(GL_NORMAL_ARRAY);
  ta = G.IsEnabled(GL_TEXTURE_COORD_ARRAY);
  get_array(&vtx, GL_VERTEX_ARRAY_SIZE, GL_VERTEX_ARRAY_TYPE, GL_VERTEX_ARRAY_STRIDE,
            GL_VERTEX_ARRAY_BUFFER_BINDING, GL_VERTEX_ARRAY_POINTER);
  get_array(&tc, GL_TEXTURE_COORD_ARRAY_SIZE, GL_TEXTURE_COORD_ARRAY_TYPE,
            GL_TEXTURE_COORD_ARRAY_STRIDE, GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING,
            GL_TEXTURE_COORD_ARRAY_POINTER);
  if (blend) {
    if (G.BlendFuncSeparateOES) {
      G.GetIntegerv(GL_BLEND_SRC_RGB_OES, &srgb);
      G.GetIntegerv(GL_BLEND_DST_RGB_OES, &drgb);
      G.GetIntegerv(GL_BLEND_SRC_ALPHA_OES, &sa);
      G.GetIntegerv(GL_BLEND_DST_ALPHA_OES, &da);
    } else {
      G.GetIntegerv(GL_BLEND_SRC, &src);
      G.GetIntegerv(GL_BLEND_DST, &dst);
    }
  }

  /* ---- draw ---- */
  for (unsigned i = 0; i < NOFF; i++)
    if (off_was[i])
      G.Disable(k_off[i]);
  if (blend) {
    G.Enable(GL_BLEND);
    G.BlendFunc(GL_SRC_ALPHA, g_mesh.n && g_mesh.additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
  } else if (blend_was) {
    G.Disable(GL_BLEND);
  }
  G.Enable(GL_TEXTURE_2D);
  G.BindTexture(GL_TEXTURE_2D, tex);
  G.ColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  if (g_mesh.n) { /* dcr_blit_mesh: tinted */
    G.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    G.Color4f(g_mesh.rgba[0], g_mesh.rgba[1], g_mesh.rgba[2], g_mesh.rgba[3]);
  } else if (g_alpha < 1.0f) { /* dcr_blit_alpha: the picture faded */
    G.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    G.Color4f(1.0f, 1.0f, 1.0f, g_alpha);
  } else {
    G.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    G.Color4f(1.0f, 0.0f, 1.0f, 1.0f); /* shows only if the texture is not sampled */
  }
  GLenum err;
  if (method == 1) {
    G.DrawTexiOES(x, vh - y - h, 0, w, h);
    err = G.GetError();
  } else {
    G.Viewport(0, 0, vw, vh);
    G.MatrixMode(GL_PROJECTION);
    G.LoadIdentity();
    G.Orthof(0.0f, (GLfloat)vw, (GLfloat)vh, 0.0f, -1.0f, 1.0f);
    G.MatrixMode(GL_MODELVIEW);
    G.LoadIdentity();
    G.MatrixMode(GL_TEXTURE);
    G.LoadIdentity();
    const GLfloat x0 = (GLfloat)x, y0 = (GLfloat)y, x1 = (GLfloat)(x + w), y1 = (GLfloat)(y + h);
    const GLfloat pos[8] = {x0, y0, x1, y0, x0, y1, x1, y1};
    const GLfloat uv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    if (G.BindBuffer)
      G.BindBuffer(GL_ARRAY_BUFFER, 0);
    G.EnableClientState(GL_VERTEX_ARRAY);
    G.EnableClientState(GL_TEXTURE_COORD_ARRAY);
    G.DisableClientState(GL_COLOR_ARRAY);
    G.DisableClientState(GL_NORMAL_ARRAY);
    G.VertexPointer(2, GL_FLOAT, 0, g_mesh.n ? g_mesh.pos : pos);
    G.TexCoordPointer(2, GL_FLOAT, 0, g_mesh.n ? g_mesh.uv : uv);
    G.DrawArrays(g_mesh.n ? GL_TRIANGLES : GL_TRIANGLE_STRIP, 0, g_mesh.n ? g_mesh.n : 4);
    err = G.GetError();
  }

  /* ---- restore ---- */
  if (method != 1) {
    if (G.BindBuffer)
      G.BindBuffer(GL_ARRAY_BUFFER, (GLuint)vtx.buffer);
    G.VertexPointer(vtx.size, (GLenum)vtx.type, vtx.stride, vtx.ptr);
    if (G.BindBuffer)
      G.BindBuffer(GL_ARRAY_BUFFER, (GLuint)tc.buffer);
    G.TexCoordPointer(tc.size, (GLenum)tc.type, tc.stride, tc.ptr);
    if (G.BindBuffer)
      G.BindBuffer(GL_ARRAY_BUFFER, (GLuint)abuf);
    (va ? G.EnableClientState : G.DisableClientState)(GL_VERTEX_ARRAY);
    (ta ? G.EnableClientState : G.DisableClientState)(GL_TEXTURE_COORD_ARRAY);
    (ca ? G.EnableClientState : G.DisableClientState)(GL_COLOR_ARRAY);
    (na ? G.EnableClientState : G.DisableClientState)(GL_NORMAL_ARRAY);
    G.MatrixMode(GL_TEXTURE);
    G.LoadMatrixf(texm);
    G.MatrixMode(GL_MODELVIEW);
    G.LoadMatrixf(model);
    G.MatrixMode(GL_PROJECTION);
    G.LoadMatrixf(proj);
    G.MatrixMode((GLenum)mode);
    G.Viewport(vp[0], vp[1], vp[2], vp[3]);
  }
  G.Color4f(color[0], color[1], color[2], color[3]);
  G.ColorMask(mask[0], mask[1], mask[2], mask[3]);
  G.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, env);
  G.BindTexture(GL_TEXTURE_2D, (GLuint)bound);
  if (!tex_was)
    G.Disable(GL_TEXTURE_2D);
  if (blend) {
    if (G.BlendFuncSeparateOES)
      G.BlendFuncSeparateOES((GLenum)srgb, (GLenum)drgb, (GLenum)sa, (GLenum)da);
    else
      G.BlendFunc((GLenum)src, (GLenum)dst);
  }
  (blend_was ? G.Enable : G.Disable)(GL_BLEND);
  for (unsigned i = 0; i < NOFF; i++)
    if (off_was[i])
      G.Enable(k_off[i]);
  if (G.ClientActiveTexture)
    G.ClientActiveTexture((GLenum)client);
  if (G.ActiveTexture) {
    for (int i = 1; i < units; i++)
      if (unit_tex[i]) {
        G.ActiveTexture(GL_TEXTURE0 + i);
        G.Enable(GL_TEXTURE_2D);
      }
    G.ActiveTexture((GLenum)active);
  }
  while (G.GetError() != GL_NO_ERROR) /* a state the driver lacks: not the engine's */
    ;
  return err;
}

void dcr_blit_read(int x, int y, unsigned char rgba[4]) {
  int vw, vh;
  dcr_window_size(&vw, &vh);
  memset(rgba, 0, 4);
  G.ReadPixels(x, vh - 1 - y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

void dcr_blit_log_state(const char *who) {
  if (g_state <= 0)
    return;
  static const struct {
    GLenum cap;
    const char *name;
  } caps[] = {{GL_CULL_FACE, "cull"},       {GL_BLEND, "blend"},       {GL_ALPHA_TEST, "alpha"},
              {GL_DEPTH_TEST, "depth"},     {GL_SCISSOR_TEST, "scissor"}, {GL_STENCIL_TEST, "stencil"},
              {GL_FOG, "fog"},              {GL_LIGHTING, "lighting"},  {GL_COLOR_LOGIC_OP, "logicop"},
              {GL_TEXTURE_2D, "texture0"},  {GL_CLIP_PLANE0, "clip0"},  {GL_VERTEX_ARRAY, "va"},
              {GL_TEXTURE_COORD_ARRAY, "ta"}, {GL_COLOR_ARRAY, "ca"}};
  char on[160] = "";
  for (unsigned i = 0; i < sizeof caps / sizeof caps[0]; i++)
    if (G.IsEnabled(caps[i].cap)) {
      strcat(on, " ");
      strcat(on, caps[i].name);
    }
  GLint front = 0, cull = 0, vp[4] = {0}, env = 0, fbo = 0, units = 1, active = 0, t1 = 0;
  GLboolean mask[4] = {0};
  G.GetIntegerv(GL_FRONT_FACE, &front);
  G.GetIntegerv(GL_CULL_FACE_MODE, &cull);
  G.GetIntegerv(GL_VIEWPORT, vp);
  G.GetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &env);
  G.GetIntegerv(GL_FRAMEBUFFER_BINDING_OES, &fbo);
  G.GetBooleanv(GL_COLOR_WRITEMASK, mask);
  if (G.ActiveTexture) {
    G.GetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
    G.GetIntegerv(GL_ACTIVE_TEXTURE, &active);
    if (units > 1) {
      G.ActiveTexture(GL_TEXTURE1);
      t1 = G.IsEnabled(GL_TEXTURE_2D);
      G.ActiveTexture((GLenum)active);
    }
  }
  while (G.GetError() != GL_NO_ERROR)
    ;
  debugPrintf("[%s] the game's GL state: on:%s; front face 0x%x, cull 0x%x, viewport %d,%d %dx%d, "
              "texenv 0x%x, framebuffer %d, colour mask %d%d%d%d, %d texture units (unit 1 "
              "texturing %d), active 0x%x\n",
              who, on[0] ? on : " -", (unsigned)front, (unsigned)cull, vp[0], vp[1], vp[2], vp[3],
              (unsigned)env, fbo, mask[0], mask[1], mask[2], mask[3], units, t1, (unsigned)active);
}

/* As dcr_blit, blended, its alpha times `alpha` (0..1): a picture fading. */
GLenum dcr_blit_alpha(GLuint tex, int x, int y, int w, int h, float alpha) {
  if (alpha <= 0.0f)
    return GL_NO_ERROR;
  g_alpha = alpha > 1.0f ? 1.0f : alpha;
  GLenum err = dcr_blit(tex, x, y, w, h, 1, 0);
  g_alpha = 1.0f;
  return err;
}

/* Triangles (nverts/3 of them; window pixels from the top left, and texture
 * coordinates, 2 floats a vertex) of tex, blended, its colour times rgba. */
GLenum dcr_blit_mesh(GLuint tex, const GLfloat *pos, const GLfloat *uv, int nverts, const GLfloat rgba[4]) {
  if (nverts < 3 || rgba[3] <= 0.0f)
    return GL_NO_ERROR;
  g_mesh.pos = pos, g_mesh.uv = uv, g_mesh.n = nverts;
  memcpy(g_mesh.rgba, rgba, sizeof g_mesh.rgba);
  GLenum err = dcr_blit(tex, 0, 0, 1, 1, 1, 0);
  g_mesh.n = 0;
  return err;
}

/* The same, replacing what is there (no blending): a picture of its own,
 * whatever its alpha (another engine's frame). */
GLenum dcr_blit_mesh_opaque(GLuint tex, const GLfloat *pos, const GLfloat *uv, int nverts) {
  if (nverts < 3)
    return GL_NO_ERROR;
  g_mesh.pos = pos, g_mesh.uv = uv, g_mesh.n = nverts;
  g_mesh.rgba[0] = g_mesh.rgba[1] = g_mesh.rgba[2] = g_mesh.rgba[3] = 1.0f;
  GLenum err = dcr_blit(tex, 0, 0, 1, 1, 0, 0);
  g_mesh.n = 0;
  return err;
}

/* The same, added to the picture (light): a glow. */
GLenum dcr_blit_mesh_add(GLuint tex, const GLfloat *pos, const GLfloat *uv, int nverts, const GLfloat rgba[4]) {
  g_mesh.additive = 1;
  GLenum err = dcr_blit_mesh(tex, pos, uv, nverts, rgba);
  g_mesh.additive = 0;
  return err;
}

#endif /* RT_GL_BLIT */
