/* jni_core.c -- JNIEnv / JavaVM for a VM that does not exist (see jni.h).
 *
 * Function table: the 233 JNINativeInterface slots are filled by name through
 * the enum below, which lists them in the order of the JNI specification --
 * one misplaced slot would silently send the engine to the wrong function, so
 * the order is written out once, checked by a static assertion on the count,
 * and every slot is assigned by its own name.
 *
 * Calls: every Call<Type>Method{,V,A} and NewObject{,V,A} funnels into one
 * invoke(), with the arguments first decoded into a jvalue[] by walking the
 * method signature (so float varargs arrive promoted to double, jlong is read
 * 8-byte aligned, as the AAPCS lays them out). A method with no handler
 * returns zero/NULL and is logged once -- that log is the list of what the
 * engine still wants from "Java".
 *
 * References: objects are reference counted. A new object has one reference
 * (the local ref the engine receives); NewGlobalRef adds one; DeleteLocalRef /
 * DeleteGlobalRef drop one. Singletons and class objects are immortal. Objects
 * the port passes INTO engine natives are retained across the call, because
 * JNI lets a native DeleteLocalRef its own arguments.
 *
 * Fields: GetXxxField answers from the port's jni_field_defs[] (a handler or a
 * platform constant), else from the object's own field storage, which
 * SetXxxField and jni_set_field() write (engines read back the fields of the
 * event objects they are handed).
 *
 * Per port: the handler tables (required data, see jni.h); port_jni_invoke(),
 * a weak callback that sees every call first; and the RT_JNI_UNHANDLED_*
 * settings below, for what an unhandled call answers. MIT.
 */
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "jni.h"
#include "rt_settings.h"
#include "util.h"

/* ---------------------------------------------------------------- settings */
/* An unhandled static instance() / getInstance() / sharedInstance() with no
 * arguments answers the class's singleton object instead of null, so calls
 * on it become unhandled no-ops rather than null dereferences (C# plugin
 * wrappers). dcr 1; lab2, abs, pvz, sonic, flappy, a8r 0. */
#ifndef RT_JNI_UNHANDLED_INSTANCE_SINGLETON
#define RT_JNI_UNHANDLED_INSTANCE_SINGLETON 0
#endif
/* An unhandled instance method on a ...Builder, or one declared to return the
 * receiver's own class, answers the receiver (build/create excepted), so a
 * chain of setters survives. dcr 1; the others 0. */
#ifndef RT_JNI_UNHANDLED_BUILDER_RETURNS_SELF
#define RT_JNI_UNHANDLED_BUILDER_RETURNS_SELF 0
#endif

/* from group G (rt_cfg.h): the runtime's view of config.ini */
#if __has_include("rt_cfg.h")
#include "rt_cfg.h"
#else
typedef struct RtConfig {
  int res_w, res_h, boost, gl_selftest, boot_log, log_jni;
} RtConfig;
const RtConfig *rt_config(void);
#endif

/* from group B (bionic_printf.c, bionic.h): vsnprintf printing a NULL %s as
 * "(null)", as bionic does (newlib's would strlen(NULL)) */
int b_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
/* from group F (dcr_path.c, dcr_path.h) */
const char *dcr_game_root(void);


#define JOBJ_MAGIC 0x4a4f424au   /* 'JOBJ' */
#define JFIELD_MAGIC 0x4a464944u /* 'JFID' */

static Mutex g_lock;

/* =============================== classes ================================== */
#define MAX_CLASSES 512
static JClass *g_classes[MAX_CLASSES];
static int g_nclasses;
static JClass *g_class_class, *g_string_class;

static const char *super_of(const char *name) {
  for (int i = 0; jni_class_supers[i][0]; i++)
    if (!strcmp(jni_class_supers[i][0], name))
      return jni_class_supers[i][1];
  return strcmp(name, "java/lang/Object") ? "java/lang/Object" : NULL;
}

static JClass *class_locked(const char *name);

static volatile int32_t g_live_objects;
int jni_live_objects(void) { return g_live_objects; }

static JObj *new_obj_raw(JClass *c, int kind) {
  JObj *o = calloc(1, sizeof *o);
  if (!o)
    return NULL;
  __atomic_add_fetch(&g_live_objects, 1, __ATOMIC_RELAXED);
  o->magic = JOBJ_MAGIC;
  o->kind = (uint8_t)kind;
  o->refs = 1;
  o->cls = c;
  return o;
}

static JClass *class_locked(const char *name) {
  for (int i = 0; i < g_nclasses; i++)
    if (!strcmp(g_classes[i]->name, name))
      return g_classes[i];
  if (g_nclasses >= MAX_CLASSES) {
    debugPrintf("[jni] class table full at %s\n", name);
    return g_classes[0];
  }
  JClass *c = calloc(1, sizeof *c);
  snprintf(c->name, sizeof c->name, "%s", name);
  g_classes[g_nclasses++] = c;
  const char *sup = name[0] == '[' ? "java/lang/Object" : super_of(name);
  c->super = sup ? class_locked(sup) : NULL;
  c->obj = new_obj_raw(g_class_class ? g_class_class : c, JK_CLASS);
  c->obj->immortal = 1;
  c->obj->c.of = c;
  return c;
}

JClass *jni_class(const char *name) {
  char buf[112];
  snprintf(buf, sizeof buf, "%s", name ? name : "java/lang/Object");
  for (char *p = buf; *p; p++)
    if (*p == '.')
      *p = '/';
  mutexLock(&g_lock);
  JClass *c = class_locked(buf);
  mutexUnlock(&g_lock);
  return c;
}

static int class_is(const JClass *c, const char *name) {
  for (; c; c = c->super)
    if (!strcmp(c->name, name))
      return 1;
  return 0;
}

static int is_missing_class(const char *name) {
  for (int i = 0; jni_missing_classes[i]; i++) {
    const char *m = jni_missing_classes[i];
    size_t n = strlen(m);
    if (n && m[n - 1] == '*' ? !strncmp(name, m, n - 1) : !strcmp(name, m))
      return 1;
  }
  return 0;
}

/* ---- which classes exist ----
 * On a phone, FindClass / Class.forName find the app's own classes (its dex
 * files) and the framework's, and nothing else. The answer steers the game:
 * Unity's JNI_OnLoad registers natives on its wrapper classes and calls
 * FatalError if one is missing, and C# probes optional plugin classes with
 * AndroidJavaClass and takes a different path when they are absent. The
 * wrapper runs no Java and the staged APK may carry no dex, so the setup
 * writes the NAMES the APK defines to <root>/classes.txt. Without that file
 * we fall back to "everything but jni_missing_classes". */
static char *g_cls_blob;
static const char **g_cls;
static int g_ncls = -1; /* -1: no list loaded */

static int cmp_cstr(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void load_class_list(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    debugPrintf("[jni] %s missing: guessing which Java classes exist (the setup "
                "writes it from the APK)\n", path);
    return;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  g_cls_blob = sz > 0 ? malloc((size_t)sz + 1) : NULL;
  if (!g_cls_blob || fread(g_cls_blob, 1, (size_t)sz, f) != (size_t)sz) {
    fclose(f);
    free(g_cls_blob);
    g_cls_blob = NULL;
    debugPrintf("[jni] %s unreadable: guessing which Java classes exist\n", path);
    return;
  }
  fclose(f);
  g_cls_blob[sz] = 0;
  int lines = 1;
  for (long i = 0; i < sz; i++)
    lines += g_cls_blob[i] == '\n';
  g_cls = malloc(sizeof *g_cls * (size_t)lines);
  int n = 0;
  for (char *p = g_cls_blob; g_cls && *p;) {
    char *e = strchr(p, '\n');
    if (e)
      *e = 0;
    size_t len = strlen(p);
    if (len && p[len - 1] == '\r')
      p[--len] = 0;
    if (len && p[0] != '#')
      g_cls[n++] = p;
    if (!e)
      break;
    p = e + 1;
  }
  qsort(g_cls, (size_t)n, sizeof *g_cls, cmp_cstr);
  g_ncls = n;
  debugPrintf("[jni] classes.txt: %d classes defined by the APK\n", n);
}

/* The platform's own namespaces (android/support is app code: in the list). */
static int is_framework_class(const char *n) {
  static const char *const pfx[] = {"java/", "javax/", "dalvik/", "android/", "org/json/",
                                    "org/xml/", "org/xmlpull/", "org/w3c/", "org/apache/http/",
                                    "com/android/internal/", "libcore/", "sun/", NULL};
  if (!strncmp(n, "android/support/", 16))
    return 0;
  for (int i = 0; pfx[i]; i++)
    if (!strncmp(n, pfx[i], strlen(pfx[i])))
      return 1;
  return 0;
}

int jni_class_exists(const char *name) {
  if (!name || !*name)
    return 0;
  if (name[0] == '[' || is_framework_class(name))
    return 1;
  if (g_ncls >= 0)
    return bsearch(&name, g_cls, (size_t)g_ncls, sizeof *g_cls, cmp_cstr) != NULL;
  return !is_missing_class(name);
}

/* =============================== objects ================================== */
static inline int is_obj(const void *p) {
  const JObj *o = p;
  return o && ((uintptr_t)o & 3) == 0 && o->magic == JOBJ_MAGIC;
}

JObj *jni_retain(JObj *o) {
  if (is_obj(o) && !o->immortal)
    __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
  return o;
}

static void destroy(JObj *o) {
  if (o->finalize)
    o->finalize(o);
  for (JFieldVal *f = o->fields, *n; f; f = n) {
    n = f->next;
    if (f->is_obj)
      jni_release(f->v.l);
    free(f);
  }
  switch (o->kind) {
  case JK_STRING:
    free(o->s.utf);
    free(o->s.u16);
    break;
  case JK_ARRAY:
    if (o->a.elem == 'L')
      for (jsize i = 0; i < o->a.len; i++)
        jni_release(((JObj **)o->a.data)[i]);
    free(o->a.data);
    break;
  }
  o->magic = 0xdeadbeefu;
  __atomic_sub_fetch(&g_live_objects, 1, __ATOMIC_RELAXED);
  free(o);
}

void jni_release(JObj *o) {
  if (!is_obj(o) || o->immortal)
    return;
  if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) == 0)
    destroy(o);
}

JObj *jni_new(const char *cls) { return new_obj_raw(jni_class(cls), JK_OBJECT); }

JObj *jni_singleton(const char *cls) {
  static struct { JClass *c; JObj *o; } s[128];
  static int n;
  JClass *c = jni_class(cls);
  mutexLock(&g_lock);
  for (int i = 0; i < n; i++)
    if (s[i].c == c) {
      mutexUnlock(&g_lock);
      return s[i].o;
    }
  JObj *o = new_obj_raw(c, JK_OBJECT);
  o->immortal = 1;
  if (n < (int)ARRAY_SIZE(s)) {
    s[n].c = c;
    s[n].o = o;
    n++;
  }
  mutexUnlock(&g_lock);
  return o;
}

int jni_is(const void *obj, const char *cls) {
  const JObj *o = obj;
  return is_obj(o) && class_is(o->cls, cls);
}

/* ----- strings: UTF-8 is the stored form, UTF-16 made on demand ----- */
JObj *jni_str(const char *utf) {
  if (!utf)
    return NULL;
  JObj *o = new_obj_raw(g_string_class, JK_STRING);
  o->s.utf = strdup(utf);
  return o;
}

JObj *jni_str_fmt(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  b_vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return jni_str(buf);
}

const char *jni_utf(const void *s) {
  const JObj *o = s;
  return (is_obj(o) && o->kind == JK_STRING && o->s.utf) ? o->s.utf : "";
}

/* Decode (modified) UTF-8 -> UTF-16. Handles C0 80 for NUL, CESU-8 surrogate
 * halves, and standard 4-byte sequences (as surrogate pairs). */
static jsize ju8_to_u16(const char *s, jchar *out, jsize cap) {
  const uint8_t *p = (const uint8_t *)s;
  jsize n = 0;
  while (*p) {
    uint32_t c;
    if (p[0] < 0x80) { c = p[0]; p += 1; }
    else if ((p[0] & 0xe0) == 0xc0 && p[1]) { c = (p[0] & 0x1fu) << 6 | (p[1] & 0x3fu); p += 2; }
    else if ((p[0] & 0xf0) == 0xe0 && p[1] && p[2]) { c = (p[0] & 0x0fu) << 12 | (p[1] & 0x3fu) << 6 | (p[2] & 0x3fu); p += 3; }
    else if ((p[0] & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) {
      c = (p[0] & 0x07u) << 18 | (p[1] & 0x3fu) << 12 | (p[2] & 0x3fu) << 6 | (p[3] & 0x3fu);
      p += 4;
      if (out && n + 1 < cap) {
        c -= 0x10000;
        out[n] = (jchar)(0xd800 | (c >> 10));
        out[n + 1] = (jchar)(0xdc00 | (c & 0x3ff));
      }
      n += 2;
      continue;
    } else { c = 0xfffd; p += 1; }
    if (out && n < cap)
      out[n] = (jchar)c;
    n++;
  }
  return n;
}

static char *ju16_to_u8(const jchar *u, jsize len) {
  char *out = malloc((size_t)len * 3 + 1), *o = out;
  if (!out)
    return NULL;
  for (jsize i = 0; i < len; i++) {
    uint32_t c = u[i];
    if (c >= 0xd800 && c <= 0xdbff && i + 1 < len && u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff) {
      c = 0x10000 + ((c - 0xd800) << 10) + (u[i + 1] - 0xdc00);
      i++;
      *o++ = (char)(0xf0 | c >> 18);
      *o++ = (char)(0x80 | ((c >> 12) & 0x3f));
      *o++ = (char)(0x80 | ((c >> 6) & 0x3f));
      *o++ = (char)(0x80 | (c & 0x3f));
    } else if (c < 0x80) {
      *o++ = (char)c;
    } else if (c < 0x800) {
      *o++ = (char)(0xc0 | c >> 6);
      *o++ = (char)(0x80 | (c & 0x3f));
    } else {
      *o++ = (char)(0xe0 | c >> 12);
      *o++ = (char)(0x80 | ((c >> 6) & 0x3f));
      *o++ = (char)(0x80 | (c & 0x3f));
    }
  }
  *o = 0;
  return out;
}

static void ensure_u16(JObj *o) {
  if (o->s.u16)
    return;
  jsize n = ju8_to_u16(o->s.utf, NULL, 0);
  jchar *u = malloc(((size_t)n + 1) * sizeof(jchar));
  ju8_to_u16(o->s.utf, u, n);
  u[n] = 0;
  jchar *expected = NULL;
  if (__atomic_compare_exchange_n(&o->s.u16, &expected, u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    o->s.u16len = n;
  else
    free(u); /* another thread made it */
}

/* ----- arrays ----- */
static size_t elem_size(char e) {
  switch (e) {
  case 'Z': case 'B': return 1;
  case 'C': case 'S': return 2;
  case 'J': case 'D': return 8;
  default: return 4; /* I F L */
  }
}

JObj *jni_array(char elem, jsize len) {
  char cname[64];
  if (len < 0)
    len = 0;
  snprintf(cname, sizeof cname, elem == 'L' ? "[Ljava/lang/Object;" : "[%c", elem);
  JObj *o = new_obj_raw(jni_class(cname), JK_ARRAY);
  o->a.elem = elem;
  o->a.len = len;
  o->a.data = calloc((size_t)len + 1, elem_size(elem));
  return o;
}

/* =============================== exceptions =============================== */
static __thread JObj *t_exc;

void jni_throw(const char *cls, const char *msg) {
  JObj *e = jni_new(cls);
  e->p = strdup(msg ? msg : "");
  e->finalize = NULL;
  if (t_exc)
    jni_release(t_exc);
  t_exc = e;
  debugPrintf("[jni] throw %s: %s\n", cls, msg ? msg : "");
}

/* ============================= method / field IDs ========================= */
#define MAX_METHODS 4096
static JMethod *g_methods[MAX_METHODS];
static int g_nmethods;
#define MAX_FIELDS 1024
static JField *g_fields[MAX_FIELDS];
static int g_nfields;

static const JMethodDef *find_method_def(const JClass *c, const char *name, const char *sig) {
  for (; c; c = c->super)
    for (int i = 0; jni_method_defs[i].cls; i++) {
      const JMethodDef *d = &jni_method_defs[i];
      if (!strcmp(d->cls, c->name) && (!d->name || !strcmp(d->name, name)) &&
          (!d->sig || !strcmp(d->sig, sig)))
        return d; /* a NULL name: every method of the class (the last entries for it) */
    }
  return NULL;
}

static const JFieldDef *find_field_def(const JClass *c, const char *name) {
  for (; c; c = c->super)
    for (int i = 0; jni_field_defs[i].cls; i++) {
      const JFieldDef *d = &jni_field_defs[i];
      if (!strcmp(d->cls, c->name) && !strcmp(d->name, name))
        return d;
    }
  return NULL;
}

static char ret_char(const char *sig) {
  const char *p = strchr(sig, ')');
  return p ? p[1] : 'V';
}

static JMethod *method_id(JClass *c, const char *name, const char *sig, int is_static) {
  if (!c || !name || !sig)
    return NULL;
  mutexLock(&g_lock);
  for (int i = 0; i < g_nmethods; i++) {
    JMethod *m = g_methods[i];
    if (m->cls == c && m->is_static == is_static && !strcmp(m->name, name) && !strcmp(m->sig, sig)) {
      mutexUnlock(&g_lock);
      return m;
    }
  }
  JMethod *m = calloc(1, sizeof *m);
  m->magic = JMETH_MAGIC;
  m->cls = c;
  snprintf(m->name, sizeof m->name, "%s", name);
  snprintf(m->sig, sizeof m->sig, "%s", sig);
  m->is_static = (uint8_t)is_static;
  m->ret = ret_char(sig);
  m->def = find_method_def(c, name, sig);
  if (g_nmethods < MAX_METHODS)
    g_methods[g_nmethods++] = m;
  mutexUnlock(&g_lock);
  return m;
}

static JField *field_id(JClass *c, const char *name, const char *sig, int is_static) {
  if (!c || !name)
    return NULL;
  mutexLock(&g_lock);
  for (int i = 0; i < g_nfields; i++) {
    JField *f = g_fields[i];
    if (f->cls == c && f->is_static == is_static && !strcmp(f->name, name)) {
      mutexUnlock(&g_lock);
      return f;
    }
  }
  JField *f = calloc(1, sizeof *f);
  f->magic = JFIELD_MAGIC;
  f->cls = c;
  snprintf(f->name, sizeof f->name, "%s", name);
  snprintf(f->sig, sizeof f->sig, "%s", sig ? sig : "");
  f->is_static = (uint8_t)is_static;
  f->def = find_field_def(c, name);
  if (g_nfields < MAX_FIELDS)
    g_fields[g_nfields++] = f;
  mutexUnlock(&g_lock);
  return f;
}

/* ================================ invoke ================================== */
#define MAX_ARGS 24

/* Advance past one type in a signature; returns the pointer after it. */
static const char *skip_type(const char *p) {
  while (*p == '[')
    p++;
  if (*p == 'L') {
    const char *e = strchr(p, ';');
    return e ? e + 1 : p + strlen(p);
  }
  return *p ? p + 1 : p;
}

static int decode_va(const char *sig, va_list *va, jvalue *out) {
  const char *p = sig && sig[0] == '(' ? sig + 1 : "";
  int n = 0;
  while (*p && *p != ')' && n < MAX_ARGS) {
    out[n].j = 0;
    switch (*p) {
    case 'Z': out[n].z = (jboolean)va_arg(*va, int); break;
    case 'B': out[n].b = (jbyte)va_arg(*va, int); break;
    case 'C': out[n].c = (jchar)va_arg(*va, int); break;
    case 'S': out[n].s = (jshort)va_arg(*va, int); break;
    case 'I': out[n].i = va_arg(*va, jint); break;
    case 'J': out[n].j = va_arg(*va, jlong); break;
    case 'F': out[n].f = (jfloat)va_arg(*va, double); break;
    case 'D': out[n].d = va_arg(*va, double); break;
    default: out[n].l = va_arg(*va, void *); break; /* L... and [... */
    }
    p = skip_type(p);
    n++;
  }
  return n;
}

static int count_args(const char *sig) {
  const char *p = sig && sig[0] == '(' ? sig + 1 : "";
  int n = 0;
  while (*p && *p != ')' && n < MAX_ARGS) {
    p = skip_type(p);
    n++;
  }
  return n;
}

int g_jni_log; /* set by jni_init from rt_config()->log_jni */

__attribute__((weak)) int port_jni_invoke(JObj *self, JMethod *m, const jvalue *args, jvalue *out) {
  return 0;
}

static void warn_unhandled(JMethod *m, const JObj *self) {
  if (m->warned)
    return;
  m->warned = 1;
  debugPrintf("[jni] unhandled %s%s.%s%s (on %s)\n", m->is_static ? "static " : "", m->cls->name,
              m->name, m->sig, is_obj(self) ? self->cls->name : "null");
}

static jvalue invoke(JObj *self, JMethod *m, const jvalue *args) {
  if (!m || m->magic != JMETH_MAGIC) {
    debugPrintf("[jni] call through a bad method ID %p from %p\n", (void *)m, __builtin_return_address(0));
    return jv_none();
  }
  jvalue r_;
  r_.j = 0;
  if (port_jni_invoke(self, m, args, &r_))
    return r_;
  const JMethodDef *d = m->def;
  /* Declared on a superclass or an interface, implemented by the receiver. */
  if (is_obj(self) && self->kind != JK_CLASS && self->cls != m->cls) {
    const JMethodDef *dd = find_method_def(self->cls, m->name, m->sig);
    if (dd)
      d = dd;
  }
  if (g_jni_log)
    debugPrintf("[jni] %s%s.%s%s%s\n", m->is_static ? "static " : "", m->cls->name, m->name, m->sig,
                d && d->fn ? "" : " (unhandled)");
  if (d && d->fn)
    return d->fn(self, args, m);
  warn_unhandled(m, self);
  /* An unhandled method returning a String answers "" rather than null: the
   * framework getters the game calls essentially never return null, and
   * native callers read the result without a check. */
  if (m->ret == 'L') {
    const char *r = strchr(m->sig, ')');
    if (r && !strcmp(r + 1, "Ljava/lang/String;"))
      return jv_l(jni_str(""));
#if RT_JNI_UNHANDLED_INSTANCE_SINGLETON
    /* A plugin's static instance() / getInstance(): its singleton object, so
     * the C# wrapper's calls on it become unhandled no-ops instead of
     * NullReferenceExceptions. */
    if (m->is_static && !strncmp(m->sig, "()", 2) &&
        (!strcmp(m->name, "instance") || !strcmp(m->name, "getInstance") ||
         !strcmp(m->name, "sharedInstance")))
      return jv_l(jni_singleton(m->cls->name));
#endif
#if RT_JNI_UNHANDLED_BUILDER_RETURNS_SELF
    /* A builder's setter returns the builder: C# chains them (an ads or
     * billing plugin's Options$Builder), and a null in the chain is a
     * NullReferenceException that ends the caller. Unity asks with a
     * `Ljava/lang/Object;` return type, so the receiver's class decides: a
     * ...Builder, or a method declared to return the receiver's own class. */
    if (!m->is_static && is_obj(self) && self->kind == JK_OBJECT && r && strcmp(m->name, "build") &&
        strcmp(m->name, "create")) {
      const size_t nl = strlen(self->cls->name);
      const int builder = nl >= 7 && !strcmp(self->cls->name + nl - 7, "Builder");
      const int own = r[1] == 'L' && !strncmp(r + 2, self->cls->name, nl) && r[2 + nl] == ';';
      if (builder || own)
        return jv_l(jni_retain((JObj *)self));
    }
#endif
  }
  return jv_none();
}

/* ----- constant handlers for the port's tables (jni.h) ----- */
JNI_H_DECL(jni_h_void) { return jv_none(); }
JNI_H_DECL(jni_h_false) { return jv_z(0); }
JNI_H_DECL(jni_h_true) { return jv_z(1); }
JNI_H_DECL(jni_h_zero) { return jv_i(0); }
JNI_H_DECL(jni_h_minus1) { return jv_i(-1); }
JNI_H_DECL(jni_h_null) { return jv_l(NULL); }
JNI_H_DECL(jni_h_empty_string) { return jv_l(jni_str("")); }
JNI_H_DECL(jni_h_self) { return jv_l(jni_retain(self)); }

static JClass *cls_of(const void *clsobj);

JMethod *jni_method(JClass *c, const char *name, const char *sig, int is_static) {
  return method_id(c, name, sig, is_static);
}

JMethod *jni_method_best(JClass *c, const char *name, const char *sig, int is_static) {
  JMethod *m = method_id(c, name, sig, is_static);
  if (!m || m->def)
    return m;
  int n = count_args(sig);
  for (const JClass *k = c; k; k = k->super)
    for (int i = 0; jni_method_defs[i].cls; i++) {
      const JMethodDef *d = &jni_method_defs[i];
      if (d->sig && d->name && !strcmp(d->cls, k->name) && !strcmp(d->name, name) &&
          count_args(d->sig) == n)
        return method_id(c, name, d->sig, is_static);
    }
  return m;
}

JField *jni_field(JClass *c, const char *name, const char *sig, int is_static) {
  return field_id(c, name, sig, is_static);
}

JClass *jni_class_of(const void *class_obj) { return cls_of(class_obj); }
jvalue jni_call(JObj *self, JMethod *m, const jvalue *args) { return invoke(self, m, args); }

#define INVOKE_VA(self, m, va_ptr)                           \
  ({                                                         \
    jvalue a_[MAX_ARGS];                                     \
    decode_va((m) ? (m)->sig : "()V", (va_ptr), a_);         \
    invoke((self), (m), a_);                                 \
  })

/* ============================ the JNIEnv table ============================ */
typedef void *jobj_t;

enum {
  J_reserved0, J_reserved1, J_reserved2, J_reserved3,
  J_GetVersion, J_DefineClass, J_FindClass, J_FromReflectedMethod, J_FromReflectedField,
  J_ToReflectedMethod, J_GetSuperclass, J_IsAssignableFrom, J_ToReflectedField, J_Throw,
  J_ThrowNew, J_ExceptionOccurred, J_ExceptionDescribe, J_ExceptionClear, J_FatalError,
  J_PushLocalFrame, J_PopLocalFrame, J_NewGlobalRef, J_DeleteGlobalRef, J_DeleteLocalRef,
  J_IsSameObject, J_NewLocalRef, J_EnsureLocalCapacity, J_AllocObject, J_NewObject,
  J_NewObjectV, J_NewObjectA, J_GetObjectClass, J_IsInstanceOf, J_GetMethodID,
#define CALL3(T) J_Call##T##Method, J_Call##T##MethodV, J_Call##T##MethodA,
  CALL3(Object) CALL3(Boolean) CALL3(Byte) CALL3(Char) CALL3(Short) CALL3(Int) CALL3(Long)
  CALL3(Float) CALL3(Double) CALL3(Void)
#define NVCALL3(T) J_CallNonvirtual##T##Method, J_CallNonvirtual##T##MethodV, J_CallNonvirtual##T##MethodA,
  NVCALL3(Object) NVCALL3(Boolean) NVCALL3(Byte) NVCALL3(Char) NVCALL3(Short) NVCALL3(Int)
  NVCALL3(Long) NVCALL3(Float) NVCALL3(Double) NVCALL3(Void)
  J_GetFieldID,
  J_GetObjectField, J_GetBooleanField, J_GetByteField, J_GetCharField, J_GetShortField,
  J_GetIntField, J_GetLongField, J_GetFloatField, J_GetDoubleField,
  J_SetObjectField, J_SetBooleanField, J_SetByteField, J_SetCharField, J_SetShortField,
  J_SetIntField, J_SetLongField, J_SetFloatField, J_SetDoubleField,
  J_GetStaticMethodID,
#define SCALL3(T) J_CallStatic##T##Method, J_CallStatic##T##MethodV, J_CallStatic##T##MethodA,
  SCALL3(Object) SCALL3(Boolean) SCALL3(Byte) SCALL3(Char) SCALL3(Short) SCALL3(Int)
  SCALL3(Long) SCALL3(Float) SCALL3(Double) SCALL3(Void)
  J_GetStaticFieldID,
  J_GetStaticObjectField, J_GetStaticBooleanField, J_GetStaticByteField, J_GetStaticCharField,
  J_GetStaticShortField, J_GetStaticIntField, J_GetStaticLongField, J_GetStaticFloatField,
  J_GetStaticDoubleField,
  J_SetStaticObjectField, J_SetStaticBooleanField, J_SetStaticByteField, J_SetStaticCharField,
  J_SetStaticShortField, J_SetStaticIntField, J_SetStaticLongField, J_SetStaticFloatField,
  J_SetStaticDoubleField,
  J_NewString, J_GetStringLength, J_GetStringChars, J_ReleaseStringChars, J_NewStringUTF,
  J_GetStringUTFLength, J_GetStringUTFChars, J_ReleaseStringUTFChars, J_GetArrayLength,
  J_NewObjectArray, J_GetObjectArrayElement, J_SetObjectArrayElement,
#define ARR8(P, S) P##Boolean##S, P##Byte##S, P##Char##S, P##Short##S, P##Int##S, P##Long##S, P##Float##S, P##Double##S,
  ARR8(J_New, Array)
  ARR8(J_Get, ArrayElements)
  ARR8(J_Release, ArrayElements)
  ARR8(J_Get, ArrayRegion)
  ARR8(J_Set, ArrayRegion)
  J_RegisterNatives, J_UnregisterNatives, J_MonitorEnter, J_MonitorExit, J_GetJavaVM,
  J_GetStringRegion, J_GetStringUTFRegion, J_GetPrimitiveArrayCritical,
  J_ReleasePrimitiveArrayCritical, J_GetStringCritical, J_ReleaseStringCritical,
  J_NewWeakGlobalRef, J_DeleteWeakGlobalRef, J_ExceptionCheck, J_NewDirectByteBuffer,
  J_GetDirectBufferAddress, J_GetDirectBufferCapacity, J_GetObjectRefType,
  J_COUNT
};
_Static_assert(J_COUNT == 233, "JNINativeInterface has 233 slots");

static void *g_fn[J_COUNT];
static void **g_env_ptr = g_fn;       /* JNIEnv is a pointer to the table */
void *g_jni_env = &g_env_ptr;

/* ----- versions / classes ----- */
static jint j_GetVersion(void *env) { return JNI_VERSION_1_6; }

static jobj_t j_FindClass(void *env, const char *name) {
  if (!name)
    return NULL;
  char buf[112];
  snprintf(buf, sizeof buf, "%s", name);
  for (char *p = buf; *p; p++)
    if (*p == '.')
      *p = '/';
  if (!jni_class_exists(buf)) {
    jni_throw("java/lang/NoClassDefFoundError", buf);
    return NULL;
  }
  return jni_class(buf)->obj;
}

static JClass *cls_of(const void *clsobj) {
  const JObj *o = clsobj;
  return (is_obj(o) && o->kind == JK_CLASS) ? o->c.of : NULL;
}

static jobj_t j_GetSuperclass(void *env, jobj_t c) {
  JClass *k = cls_of(c);
  return (k && k->super) ? k->super->obj : NULL;
}

static jboolean j_IsAssignableFrom(void *env, jobj_t sub, jobj_t sup) {
  JClass *a = cls_of(sub), *b = cls_of(sup);
  return a && b && class_is(a, b->name);
}

static jobj_t j_GetObjectClass(void *env, jobj_t obj) {
  JObj *o = obj;
  if (!is_obj(o))
    return NULL;
  return o->cls->obj;
}

static jboolean j_IsInstanceOf(void *env, jobj_t obj, jobj_t clazz) {
  JClass *k = cls_of(clazz);
  if (!obj)
    return 1; /* null is an instance of every class, per JNI */
  return is_obj(obj) && k && class_is(((JObj *)obj)->cls, k->name);
}

/* ----- reflection: Method/Field objects wrapping IDs ----- */
static jobj_t j_ToReflectedMethod(void *env, jobj_t cls, void *mid, jboolean is_static) {
  JObj *o = jni_new("java/lang/reflect/Method");
  o->p = mid;
  return o;
}
static void *j_FromReflectedMethod(void *env, jobj_t m) { return is_obj(m) ? ((JObj *)m)->p : NULL; }
static jobj_t j_ToReflectedField(void *env, jobj_t cls, void *fid, jboolean is_static) {
  JObj *o = jni_new("java/lang/reflect/Field");
  o->p = fid;
  return o;
}
static void *j_FromReflectedField(void *env, jobj_t f) { return is_obj(f) ? ((JObj *)f)->p : NULL; }

static jobj_t j_DefineClass(void *env, const char *name, jobj_t loader, const void *buf, jsize len) {
  return NULL;
}

/* ----- exceptions ----- */
static jint j_Throw(void *env, jobj_t e) {
  if (t_exc)
    jni_release(t_exc);
  t_exc = jni_retain(e);
  return 0;
}
static jint j_ThrowNew(void *env, jobj_t cls, const char *msg) {
  JClass *k = cls_of(cls);
  jni_throw(k ? k->name : "java/lang/Exception", msg);
  return 0;
}
void jni_exception_report(const char *where) {
  if (!t_exc)
    return;
  debugPrintf("[jni] %s threw %s: %s\n", where, t_exc->cls->name, t_exc->p ? (char *)t_exc->p : "");
  jni_release(t_exc);
  t_exc = NULL;
}

static jobj_t j_ExceptionOccurred(void *env) { return t_exc ? jni_retain(t_exc) : NULL; }
static void j_ExceptionDescribe(void *env) {
  if (t_exc)
    debugPrintf("[jni] pending exception %s: %s\n", t_exc->cls->name, t_exc->p ? (char *)t_exc->p : "");
}
static void j_ExceptionClear(void *env) {
  if (t_exc) {
    jni_release(t_exc);
    t_exc = NULL;
  }
}
static jboolean j_ExceptionCheck(void *env) { return t_exc != NULL; }
static void j_FatalError(void *env, const char *msg) {
  debugPrintf("[jni] FatalError: %s\n", msg ? msg : "");
  fatal_error_jni:
  svcBreak(BreakReason_Panic, 0, 0);
  goto fatal_error_jni;
}

/* ----- references ----- */
static jint j_PushLocalFrame(void *env, jint cap) { return 0; }
static jobj_t j_PopLocalFrame(void *env, jobj_t result) { return result; }
static jobj_t j_NewGlobalRef(void *env, jobj_t o) { return jni_retain(o); }
static void j_DeleteGlobalRef(void *env, jobj_t o) { jni_release(o); }
static void j_DeleteLocalRef(void *env, jobj_t o) { jni_release(o); }
static jboolean j_IsSameObject(void *env, jobj_t a, jobj_t b) { return a == b; }
static jobj_t j_NewLocalRef(void *env, jobj_t o) { return jni_retain(o); }
static jint j_EnsureLocalCapacity(void *env, jint cap) { return 0; }
static jobj_t j_NewWeakGlobalRef(void *env, jobj_t o) { return jni_retain(o); }
static void j_DeleteWeakGlobalRef(void *env, jobj_t o) { jni_release(o); }
static jint j_GetObjectRefType(void *env, jobj_t o) { return is_obj(o) ? 1 : 0; }
static jint j_MonitorEnter(void *env, jobj_t o) { return 0; }
static jint j_MonitorExit(void *env, jobj_t o) { return 0; }

/* ----- objects ----- */
static jobj_t new_object(jobj_t clazz, JMethod *ctor, const jvalue *args) {
  JClass *k = cls_of(clazz);
  if (!k)
    return NULL;
  JObj *o = new_obj_raw(k, JK_OBJECT);
  if (ctor) {
    jvalue r = invoke(o, ctor, args);
    /* A constructor handler may hand back a different object (String, boxed
     * values, File built from another File...); it replaces the blank one. */
    if (r.l && r.l != o) {
      jni_release(o);
      return r.l;
    }
  }
  return o;
}
static jobj_t j_AllocObject(void *env, jobj_t clazz) { return new_object(clazz, NULL, NULL); }
static jobj_t j_NewObject(void *env, jobj_t clazz, JMethod *ctor, ...) {
  jvalue a[MAX_ARGS];
  va_list va;
  va_start(va, ctor);
  decode_va(ctor ? ctor->sig : "()V", &va, a);
  va_end(va);
  return new_object(clazz, ctor, a);
}
static jobj_t j_NewObjectV(void *env, jobj_t clazz, JMethod *ctor, va_list va) {
  jvalue a[MAX_ARGS];
  va_list c;
  va_copy(c, va);
  decode_va(ctor ? ctor->sig : "()V", &c, a);
  va_end(c);
  return new_object(clazz, ctor, a);
}
static jobj_t j_NewObjectA(void *env, jobj_t clazz, JMethod *ctor, const jvalue *args) {
  jvalue a[MAX_ARGS];
  int n = ctor ? count_args(ctor->sig) : 0;
  for (int i = 0; i < n; i++)
    a[i] = args[i];
  return new_object(clazz, ctor, a);
}

static void *j_GetMethodID(void *env, jobj_t clazz, const char *name, const char *sig) {
  return method_id(cls_of(clazz), name, sig, 0);
}
static void *j_GetStaticMethodID(void *env, jobj_t clazz, const char *name, const char *sig) {
  return method_id(cls_of(clazz), name, sig, 1);
}

/* ----- the call families ----- */
#define CALL_FAMILY(T, RT, F)                                                                   \
  static RT j_Call##T##Method(void *env, jobj_t o, JMethod *m, ...) {                           \
    va_list va;                                                                                 \
    va_start(va, m);                                                                            \
    jvalue r = INVOKE_VA(o, m, &va);                                                            \
    va_end(va);                                                                                 \
    return (RT)r.F;                                                                             \
  }                                                                                             \
  static RT j_Call##T##MethodV(void *env, jobj_t o, JMethod *m, va_list va) {                   \
    va_list c;                                                                                  \
    va_copy(c, va);                                                                             \
    jvalue r = INVOKE_VA(o, m, &c);                                                             \
    va_end(c);                                                                                  \
    return (RT)r.F;                                                                             \
  }                                                                                             \
  static RT j_Call##T##MethodA(void *env, jobj_t o, JMethod *m, const jvalue *a) {              \
    return (RT)invoke(o, m, a).F;                                                               \
  }                                                                                             \
  static RT j_CallNonvirtual##T##Method(void *env, jobj_t o, jobj_t k, JMethod *m, ...) {       \
    va_list va;                                                                                 \
    va_start(va, m);                                                                            \
    jvalue r = INVOKE_VA(o, m, &va);                                                            \
    va_end(va);                                                                                 \
    return (RT)r.F;                                                                             \
  }                                                                                             \
  static RT j_CallNonvirtual##T##MethodV(void *env, jobj_t o, jobj_t k, JMethod *m, va_list va) { \
    va_list c;                                                                                  \
    va_copy(c, va);                                                                             \
    jvalue r = INVOKE_VA(o, m, &c);                                                             \
    va_end(c);                                                                                  \
    return (RT)r.F;                                                                             \
  }                                                                                             \
  static RT j_CallNonvirtual##T##MethodA(void *env, jobj_t o, jobj_t k, JMethod *m, const jvalue *a) { \
    return (RT)invoke(o, m, a).F;                                                               \
  }                                                                                             \
  static RT j_CallStatic##T##Method(void *env, jobj_t k, JMethod *m, ...) {                     \
    va_list va;                                                                                 \
    va_start(va, m);                                                                            \
    jvalue r = INVOKE_VA(k, m, &va);                                                            \
    va_end(va);                                                                                 \
    return (RT)r.F;                                                                             \
  }                                                                                             \
  static RT j_CallStatic##T##MethodV(void *env, jobj_t k, JMethod *m, va_list va) {             \
    va_list c;                                                                                  \
    va_copy(c, va);                                                                             \
    jvalue r = INVOKE_VA(k, m, &c);                                                             \
    va_end(c);                                                                                  \
    return (RT)r.F;                                                                             \
  }                                                                                             \
  static RT j_CallStatic##T##MethodA(void *env, jobj_t k, JMethod *m, const jvalue *a) {        \
    return (RT)invoke(k, m, a).F;                                                               \
  }

CALL_FAMILY(Object, jobj_t, l)
CALL_FAMILY(Boolean, jboolean, z)
CALL_FAMILY(Byte, jbyte, b)
CALL_FAMILY(Char, jchar, c)
CALL_FAMILY(Short, jshort, s)
CALL_FAMILY(Int, jint, i)
CALL_FAMILY(Long, jlong, j)
CALL_FAMILY(Float, jfloat, f)
CALL_FAMILY(Double, jdouble, d)

static void j_CallVoidMethod(void *env, jobj_t o, JMethod *m, ...) {
  va_list va;
  va_start(va, m);
  INVOKE_VA(o, m, &va);
  va_end(va);
}
static void j_CallVoidMethodV(void *env, jobj_t o, JMethod *m, va_list va) {
  va_list c;
  va_copy(c, va);
  INVOKE_VA(o, m, &c);
  va_end(c);
}
static void j_CallVoidMethodA(void *env, jobj_t o, JMethod *m, const jvalue *a) { invoke(o, m, a); }
static void j_CallNonvirtualVoidMethod(void *env, jobj_t o, jobj_t k, JMethod *m, ...) {
  va_list va;
  va_start(va, m);
  INVOKE_VA(o, m, &va);
  va_end(va);
}
static void j_CallNonvirtualVoidMethodV(void *env, jobj_t o, jobj_t k, JMethod *m, va_list va) {
  va_list c;
  va_copy(c, va);
  INVOKE_VA(o, m, &c);
  va_end(c);
}
static void j_CallNonvirtualVoidMethodA(void *env, jobj_t o, jobj_t k, JMethod *m, const jvalue *a) {
  invoke(o, m, a);
}
static void j_CallStaticVoidMethod(void *env, jobj_t k, JMethod *m, ...) {
  va_list va;
  va_start(va, m);
  INVOKE_VA(k, m, &va);
  va_end(va);
}
static void j_CallStaticVoidMethodV(void *env, jobj_t k, JMethod *m, va_list va) {
  va_list c;
  va_copy(c, va);
  INVOKE_VA(k, m, &c);
  va_end(c);
}
static void j_CallStaticVoidMethodA(void *env, jobj_t k, JMethod *m, const jvalue *a) { invoke(k, m, a); }

/* ----- fields ----- */
static void *j_GetFieldID(void *env, jobj_t clazz, const char *name, const char *sig) {
  return field_id(cls_of(clazz), name, sig, 0);
}
static void *j_GetStaticFieldID(void *env, jobj_t clazz, const char *name, const char *sig) {
  return field_id(cls_of(clazz), name, sig, 1);
}

/* ---- per-object field storage (Java objects the port builds for the game) ---- */
static Mutex g_field_lock;

static JFieldVal *field_slot(JObj *o, const char *name, int create) {
  mutexLock(&g_field_lock);
  JFieldVal *v = o->fields;
  while (v && strcmp(v->name, name))
    v = v->next;
  if (!v && create && (v = calloc(1, sizeof *v))) {
    snprintf(v->name, sizeof v->name, "%s", name);
    v->next = o->fields;
    o->fields = v;
  }
  mutexUnlock(&g_field_lock);
  return v;
}

void jni_set_field(JObj *o, const char *name, jvalue val, int is_object) {
  if (!is_obj(o))
    return;
  JFieldVal *v = field_slot(o, name, 1);
  if (!v)
    return;
  JObj *old = v->is_obj ? v->v.l : NULL;
  if (is_object)
    jni_retain(val.l);
  v->v = val;
  v->is_obj = (uint8_t)is_object;
  jni_release(old);
}

jvalue jni_get_field(JObj *o, const char *name) {
  JFieldVal *v = is_obj(o) ? field_slot(o, name, 0) : NULL;
  return v ? v->v : jv_none();
}

static jvalue get_field(JObj *self, JField *f) {
  if (!f || f->magic != JFIELD_MAGIC)
    return jv_none();
  const JFieldDef *d = f->def;
  if (is_obj(self) && self->kind != JK_CLASS && self->cls != f->cls) {
    const JFieldDef *dd = find_field_def(self->cls, f->name);
    if (dd)
      d = dd;
  }
  if (d && d->get)
    return d->get(self, f);
  if (d) /* a platform constant */
    return d->sval ? jv_l(jni_str(d->sval)) : jv_i(d->ival);
  if (is_obj(self)) {
    JFieldVal *v = field_slot(self, f->name, 0);
    if (v)
      return v->is_obj ? jv_l(jni_retain(v->v.l)) : v->v;
  }
  if (!f->warned) {
    f->warned = 1;
    debugPrintf("[jni] unhandled field %s%s.%s %s\n", f->is_static ? "static " : "", f->cls->name,
                f->name, f->sig);
  }
  return jv_none();
}

static void set_field(JObj *self, JField *f, jvalue val, int is_object) {
  if (!f || f->magic != JFIELD_MAGIC)
    return;
  if (is_obj(self)) {
    jni_set_field(self, f->name, val, is_object);
    return;
  }
  if (!f->warned) {
    f->warned = 1;
    debugPrintf("[jni] ignored write to field %s.%s\n", f->cls->name, f->name);
  }
}

#define FIELD_FAMILY(T, RT, F)                                                          \
  static RT j_Get##T##Field(void *env, jobj_t o, JField *f) { return (RT)get_field(o, f).F; } \
  static RT j_GetStatic##T##Field(void *env, jobj_t k, JField *f) { return (RT)get_field(k, f).F; } \
  static void j_Set##T##Field(void *env, jobj_t o, JField *f, RT v) {                   \
    jvalue j_; j_.j = 0; j_.F = v; set_field(o, f, j_, #F[0] == 'l');                   \
  }                                                                                      \
  static void j_SetStatic##T##Field(void *env, jobj_t k, JField *f, RT v) {             \
    jvalue j_; j_.j = 0; j_.F = v; set_field(k, f, j_, #F[0] == 'l');                   \
  }

FIELD_FAMILY(Object, jobj_t, l)
FIELD_FAMILY(Boolean, jboolean, z)
FIELD_FAMILY(Byte, jbyte, b)
FIELD_FAMILY(Char, jchar, c)
FIELD_FAMILY(Short, jshort, s)
FIELD_FAMILY(Int, jint, i)
FIELD_FAMILY(Long, jlong, j)
FIELD_FAMILY(Float, jfloat, f)
FIELD_FAMILY(Double, jdouble, d)

/* ----- strings ----- */
static jobj_t j_NewString(void *env, const jchar *u, jsize len) {
  char *utf = ju16_to_u8(u, len);
  JObj *o = new_obj_raw(g_string_class, JK_STRING);
  o->s.utf = utf ? utf : strdup("");
  return o;
}
static jobj_t j_NewStringUTF(void *env, const char *utf) { return jni_str(utf); }
static jsize j_GetStringLength(void *env, jobj_t s) {
  JObj *o = s;
  if (!is_obj(o) || o->kind != JK_STRING)
    return 0;
  ensure_u16(o);
  return o->s.u16len;
}
static const jchar *j_GetStringChars(void *env, jobj_t s, jboolean *is_copy) {
  JObj *o = s;
  if (is_copy)
    *is_copy = 0;
  if (!is_obj(o) || o->kind != JK_STRING)
    return NULL;
  ensure_u16(o);
  return o->s.u16;
}
static void j_ReleaseStringChars(void *env, jobj_t s, const jchar *c) {}
static jsize j_GetStringUTFLength(void *env, jobj_t s) { return (jsize)strlen(jni_utf(s)); }
static const char *j_GetStringUTFChars(void *env, jobj_t s, jboolean *is_copy) {
  if (is_copy)
    *is_copy = 0;
  JObj *o = s;
  return (is_obj(o) && o->kind == JK_STRING) ? o->s.utf : NULL;
}
static void j_ReleaseStringUTFChars(void *env, jobj_t s, const char *c) {}
static void j_GetStringRegion(void *env, jobj_t s, jsize start, jsize len, jchar *buf) {
  JObj *o = s;
  if (!is_obj(o) || o->kind != JK_STRING || !buf)
    return;
  ensure_u16(o);
  if (start < 0 || len < 0 || start + len > o->s.u16len) {
    jni_throw("java/lang/StringIndexOutOfBoundsException", "GetStringRegion");
    return;
  }
  memcpy(buf, o->s.u16 + start, (size_t)len * sizeof(jchar));
}
static void j_GetStringUTFRegion(void *env, jobj_t s, jsize start, jsize len, char *buf) {
  JObj *o = s;
  if (!is_obj(o) || o->kind != JK_STRING || !buf)
    return;
  ensure_u16(o);
  if (start < 0 || len < 0 || start + len > o->s.u16len) {
    jni_throw("java/lang/StringIndexOutOfBoundsException", "GetStringUTFRegion");
    return;
  }
  char *u = ju16_to_u8(o->s.u16 + start, len);
  strcpy(buf, u ? u : "");
  free(u);
}
static const jchar *j_GetStringCritical(void *env, jobj_t s, jboolean *is_copy) {
  return j_GetStringChars(env, s, is_copy);
}
static void j_ReleaseStringCritical(void *env, jobj_t s, const jchar *c) {}

/* ----- arrays ----- */
static JObj *as_array(jobj_t a) {
  JObj *o = a;
  return (is_obj(o) && o->kind == JK_ARRAY) ? o : NULL;
}
static jsize j_GetArrayLength(void *env, jobj_t a) {
  JObj *o = as_array(a);
  return o ? o->a.len : 0;
}
static jobj_t j_NewObjectArray(void *env, jsize len, jobj_t clazz, jobj_t init) {
  JObj *o = jni_array('L', len);
  if (cls_of(clazz)) {
    char n[128];
    snprintf(n, sizeof n, "[L%s;", cls_of(clazz)->name);
    o->cls = jni_class(n);
  }
  for (jsize i = 0; init && i < len; i++)
    ((JObj **)o->a.data)[i] = jni_retain(init);
  return o;
}
static jobj_t j_GetObjectArrayElement(void *env, jobj_t a, jsize i) {
  JObj *o = as_array(a);
  if (!o || o->a.elem != 'L' || i < 0 || i >= o->a.len) {
    jni_throw("java/lang/ArrayIndexOutOfBoundsException", "GetObjectArrayElement");
    return NULL;
  }
  return jni_retain(((JObj **)o->a.data)[i]);
}
static void j_SetObjectArrayElement(void *env, jobj_t a, jsize i, jobj_t v) {
  JObj *o = as_array(a);
  if (!o || o->a.elem != 'L' || i < 0 || i >= o->a.len) {
    jni_throw("java/lang/ArrayIndexOutOfBoundsException", "SetObjectArrayElement");
    return;
  }
  JObj **slot = &((JObj **)o->a.data)[i];
  JObj *old = *slot;
  *slot = jni_retain(v);
  jni_release(old);
}

#define ARRAY_FAMILY(T, E, CT)                                                                 \
  static jobj_t j_New##T##Array(void *env, jsize len) { return jni_array(E, len); }            \
  static CT *j_Get##T##ArrayElements(void *env, jobj_t a, jboolean *is_copy) {                 \
    JObj *o = as_array(a);                                                                     \
    if (is_copy)                                                                               \
      *is_copy = 0;                                                                            \
    return o ? (CT *)o->a.data : NULL;                                                         \
  }                                                                                            \
  static void j_Release##T##ArrayElements(void *env, jobj_t a, CT *e, jint mode) {}           \
  static void j_Get##T##ArrayRegion(void *env, jobj_t a, jsize s, jsize n, CT *buf) {          \
    JObj *o = as_array(a);                                                                     \
    if (!o || s < 0 || n < 0 || s + n > o->a.len) {                                            \
      jni_throw("java/lang/ArrayIndexOutOfBoundsException", "Get" #T "ArrayRegion");           \
      return;                                                                                  \
    }                                                                                          \
    memcpy(buf, (CT *)o->a.data + s, (size_t)n * sizeof(CT));                                  \
  }                                                                                            \
  static void j_Set##T##ArrayRegion(void *env, jobj_t a, jsize s, jsize n, const CT *buf) {    \
    JObj *o = as_array(a);                                                                     \
    if (!o || s < 0 || n < 0 || s + n > o->a.len) {                                            \
      jni_throw("java/lang/ArrayIndexOutOfBoundsException", "Set" #T "ArrayRegion");           \
      return;                                                                                  \
    }                                                                                          \
    memcpy((CT *)o->a.data + s, buf, (size_t)n * sizeof(CT));                                  \
  }

ARRAY_FAMILY(Boolean, 'Z', jboolean)
ARRAY_FAMILY(Byte, 'B', jbyte)
ARRAY_FAMILY(Char, 'C', jchar)
ARRAY_FAMILY(Short, 'S', jshort)
ARRAY_FAMILY(Int, 'I', jint)
ARRAY_FAMILY(Long, 'J', jlong)
ARRAY_FAMILY(Float, 'F', jfloat)
ARRAY_FAMILY(Double, 'D', jdouble)

static void *j_GetPrimitiveArrayCritical(void *env, jobj_t a, jboolean *is_copy) {
  JObj *o = as_array(a);
  if (is_copy)
    *is_copy = 0;
  return o ? o->a.data : NULL;
}
static void j_ReleasePrimitiveArrayCritical(void *env, jobj_t a, void *p, jint mode) {}

/* ----- direct buffers ----- */
static jobj_t j_NewDirectByteBuffer(void *env, void *addr, jlong cap) {
  JObj *o = jni_new("java/nio/DirectByteBuffer");
  o->p = addr;
  o->v[0] = (intptr_t)cap;
  return o;
}
static void *j_GetDirectBufferAddress(void *env, jobj_t b) {
  return jni_is(b, "java/nio/ByteBuffer") ? ((JObj *)b)->p : NULL;
}
static jlong j_GetDirectBufferCapacity(void *env, jobj_t b) {
  return jni_is(b, "java/nio/ByteBuffer") ? (jlong)((JObj *)b)->v[0] : -1;
}

/* ----- natives ----- */
typedef struct { const char *name, *sig; void *fn; } JNINativeMethod;
#define MAX_NATIVES 256
static struct { JClass *cls; char name[64]; char sig[224]; void *fn; } g_natives[MAX_NATIVES];
static int g_nnatives;

static jint j_RegisterNatives(void *env, jobj_t clazz, const JNINativeMethod *m, jint n) {
  JClass *k = cls_of(clazz);
  if (!k || !m)
    return JNI_ERR;
  mutexLock(&g_lock);
  for (jint i = 0; i < n; i++) {
    int slot = -1;
    for (int j = 0; j < g_nnatives; j++)
      if (g_natives[j].cls == k && !strcmp(g_natives[j].name, m[i].name))
        slot = j;
    if (slot < 0 && g_nnatives < MAX_NATIVES)
      slot = g_nnatives++;
    if (slot < 0)
      break;
    g_natives[slot].cls = k;
    snprintf(g_natives[slot].name, sizeof g_natives[slot].name, "%s", m[i].name);
    snprintf(g_natives[slot].sig, sizeof g_natives[slot].sig, "%s", m[i].sig);
    g_natives[slot].fn = m[i].fn;
  }
  mutexUnlock(&g_lock);
  debugPrintf("[jni] RegisterNatives %s: %d method(s)\n", k->name, (int)n);
  for (jint i = 0; i < n; i++)
    debugPrintf("[jni]   %s%s -> %p\n", m[i].name, m[i].sig, m[i].fn);
  return JNI_OK;
}
static jint j_UnregisterNatives(void *env, jobj_t clazz) { return JNI_OK; }

void *jni_native(const char *cls, const char *name) {
  for (int i = 0; i < g_nnatives; i++)
    if (!strcmp(g_natives[i].cls->name, cls) && !strcmp(g_natives[i].name, name))
      return g_natives[i].fn;
  return NULL;
}

/* ============================== the JavaVM ================================ */
static jint vm_DestroyJavaVM(void *vm) { return JNI_OK; }
static jint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  if (env)
    *env = g_jni_env;
  return JNI_OK;
}
static jint vm_DetachCurrentThread(void *vm) { return JNI_OK; }
static jint vm_GetEnv(void *vm, void **env, jint version) {
  if (env)
    *env = g_jni_env;
  return JNI_OK;
}
static void *g_vm_fn[8] = {
    NULL, NULL, NULL, (void *)vm_DestroyJavaVM, (void *)vm_AttachCurrentThread,
    (void *)vm_DetachCurrentThread, (void *)vm_GetEnv, (void *)vm_AttachCurrentThread,
};
static void **g_vm_ptr = g_vm_fn;
void *g_jni_vm = &g_vm_ptr;
static jint j_GetJavaVM(void *env, void **vm) {
  if (vm)
    *vm = g_jni_vm;
  return JNI_OK;
}

/* ================================ init ==================================== */
static void unimplemented_slot(void) {
  debugPrintf("[jni] call through an unimplemented JNIEnv slot from %p\n", __builtin_return_address(0));
}

void jni_init(void) {
  const RtConfig *cfg = rt_config();
  g_jni_log = cfg ? cfg->log_jni : 0;
  mutexInit(&g_lock);
  /* java/lang/Class must exist before any class object is made. */
  g_class_class = class_locked("java/lang/Class");
  g_class_class->obj->cls = g_class_class;
  for (int i = 0; i < g_nclasses; i++)
    g_classes[i]->obj->cls = g_class_class;
  g_string_class = jni_class("java/lang/String");

  for (int i = 0; i < J_COUNT; i++)
    g_fn[i] = (void *)unimplemented_slot;
  g_fn[J_reserved0] = g_fn[J_reserved1] = g_fn[J_reserved2] = g_fn[J_reserved3] = NULL;

#define S(n) g_fn[J_##n] = (void *)j_##n
  S(GetVersion); S(DefineClass); S(FindClass); S(FromReflectedMethod); S(FromReflectedField);
  S(ToReflectedMethod); S(GetSuperclass); S(IsAssignableFrom); S(ToReflectedField); S(Throw);
  S(ThrowNew); S(ExceptionOccurred); S(ExceptionDescribe); S(ExceptionClear); S(FatalError);
  S(PushLocalFrame); S(PopLocalFrame); S(NewGlobalRef); S(DeleteGlobalRef); S(DeleteLocalRef);
  S(IsSameObject); S(NewLocalRef); S(EnsureLocalCapacity); S(AllocObject); S(NewObject);
  S(NewObjectV); S(NewObjectA); S(GetObjectClass); S(IsInstanceOf); S(GetMethodID);
#define S3(T) S(Call##T##Method); S(Call##T##MethodV); S(Call##T##MethodA); \
  S(CallNonvirtual##T##Method); S(CallNonvirtual##T##MethodV); S(CallNonvirtual##T##MethodA); \
  S(CallStatic##T##Method); S(CallStatic##T##MethodV); S(CallStatic##T##MethodA)
  S3(Object); S3(Boolean); S3(Byte); S3(Char); S3(Short); S3(Int); S3(Long); S3(Float);
  S3(Double); S3(Void);
  S(GetFieldID); S(GetStaticMethodID); S(GetStaticFieldID);
#define SF(T) S(Get##T##Field); S(Set##T##Field); S(GetStatic##T##Field); S(SetStatic##T##Field)
  SF(Object); SF(Boolean); SF(Byte); SF(Char); SF(Short); SF(Int); SF(Long); SF(Float); SF(Double);
  S(NewString); S(GetStringLength); S(GetStringChars); S(ReleaseStringChars); S(NewStringUTF);
  S(GetStringUTFLength); S(GetStringUTFChars); S(ReleaseStringUTFChars); S(GetArrayLength);
  S(NewObjectArray); S(GetObjectArrayElement); S(SetObjectArrayElement);
#define SA(T) S(New##T##Array); S(Get##T##ArrayElements); S(Release##T##ArrayElements); \
  S(Get##T##ArrayRegion); S(Set##T##ArrayRegion)
  SA(Boolean); SA(Byte); SA(Char); SA(Short); SA(Int); SA(Long); SA(Float); SA(Double);
  S(RegisterNatives); S(UnregisterNatives); S(MonitorEnter); S(MonitorExit); S(GetJavaVM);
  S(GetStringRegion); S(GetStringUTFRegion); S(GetPrimitiveArrayCritical);
  S(ReleasePrimitiveArrayCritical); S(GetStringCritical); S(ReleaseStringCritical);
  S(NewWeakGlobalRef); S(DeleteWeakGlobalRef); S(ExceptionCheck); S(NewDirectByteBuffer);
  S(GetDirectBufferAddress); S(GetDirectBufferCapacity); S(GetObjectRefType);
#undef S

  int missing = 0;
  for (int i = 4; i < J_COUNT; i++)
    missing += g_fn[i] == (void *)unimplemented_slot;
  debugPrintf("[jni] JNIEnv ready: %d slots, %d unimplemented\n", J_COUNT, missing);

  if (g_ncls < 0) {
    char path[512];
    snprintf(path, sizeof path, "%s/classes.txt", dcr_game_root());
    load_class_list(path);
  }
}
