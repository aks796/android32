/* gl_blit.h -- a picture drawn over the game's frame before it is presented
 * (the intro video, the pointer), the game's GLES 1 state kept. MIT. */
#ifndef DCR_GL_BLIT_H
#define DCR_GL_BLIT_H
#include <GLES/gl.h>

/* The GL calls it needs, looked up once: 1 ready, -1 missing (logged). */
int dcr_blit_setup(const char *who);

/* tex (a GL_TEXTURE_2D of the current context) over the window rectangle x, y,
 * w, h (pixels, from the top left), its first row at the top; blend: by its
 * alpha, else replacing. method 0: a textured quad (glDrawArrays), the way
 * the engine draws; 1: glDrawTexiOES (the texture's crop rectangle must be set:
 * dcr_blit_crop). Returns the GL error the draw left, 0 if none. */
GLenum dcr_blit(GLuint tex, int x, int y, int w, int h, int blend, int method);
void dcr_blit_crop(GLuint tex, int tex_w, int tex_h);
/* The same quad, blended, its alpha times `alpha` (0..1). */
GLenum dcr_blit_alpha(GLuint tex, int x, int y, int w, int h, float alpha);
/* Triangles of tex (nverts/3; 2 floats a vertex: window pixels from the top
 * left, texture coordinates), blended, the colour times rgba. */
GLenum dcr_blit_mesh(GLuint tex, const GLfloat *pos, const GLfloat *uv, int nverts, const GLfloat rgba[4]);
/* The same, replacing what is drawn (no blending, the colour as it is). */
GLenum dcr_blit_mesh_opaque(GLuint tex, const GLfloat *pos, const GLfloat *uv, int nverts);
/* The same, added to what is drawn (GL_SRC_ALPHA, GL_ONE): light, a glow. */
GLenum dcr_blit_mesh_add(GLuint tex, const GLfloat *pos, const GLfloat *uv, int nverts, const GLfloat rgba[4]);

/* The engine's GL state that could keep a picture off the screen, one line
 * in the log. */
void dcr_blit_log_state(const char *who);

/* The window's pixel at x, y (from the top left) as drawn so far. */
void dcr_blit_read(int x, int y, unsigned char rgba[4]);

#endif
