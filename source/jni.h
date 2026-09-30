/* jni.h -- the port's Java VM: object model, and the API class handlers use.
 *
 * There is no Java here. The game talks to "Java" through a JNIEnv, and every
 * answer it gets comes from a C handler registered for (class, method). The
 * machinery (jni_core.c) is generic: it marshals arguments by signature for
 * all three call forms (varargs, va_list, jvalue[]), keeps a class hierarchy
 * so a method declared on android/content/Context is found when called on the
 * activity, reference-counts objects, and records what the engine asked for
 * that nothing answers (logged once per method -- that list is the to-do list).
 *
 * The class handlers (jni_android.c) are plain tables of JMethodDef/JFieldDef.
 * MIT.
 */
#ifndef DCR_JNI_H
#define DCR_JNI_H

#include <stdarg.h>
#include <stdint.h>

typedef uint8_t jboolean;
typedef int8_t jbyte;
typedef uint16_t jchar;
typedef int16_t jshort;
typedef int32_t jint;
typedef int64_t jlong;
typedef float jfloat;
typedef double jdouble;
typedef jint jsize;

typedef union jvalue {
  jboolean z;
  jbyte b;
  jchar c;
  jshort s;
  jint i;
  jlong j;
  jfloat f;
  jdouble d;
  void *l;
} jvalue;

#define JNI_OK 0
#define JNI_ERR (-1)
#define JNI_EDETACHED (-2)
#define JNI_VERSION_1_6 0x00010006

typedef struct JClass JClass;
typedef struct JObj JObj;
typedef struct JMethod JMethod;
typedef struct JField JField;

enum { JK_OBJECT, JK_STRING, JK_ARRAY, JK_CLASS };

struct JObj {
  uint32_t magic;
  uint8_t kind;
  uint8_t immortal;           /* singletons and class objects: never freed */
  uint16_t _pad;
  volatile int32_t refs;
  JClass *cls;
  union {
    struct { char *utf; jchar *u16; jsize u16len; } s;        /* JK_STRING */
    struct { char elem; jsize len; void *data; } a;           /* JK_ARRAY  */
    struct { JClass *of; } c;                                 /* JK_CLASS  */
  };
  /* payload owned by the class's handlers */
  void *p;
  intptr_t v[4];
  void (*finalize)(JObj *self);
  struct JFieldVal *fields; /* instance fields set from C or by SetXxxField */
};

typedef struct JFieldVal {
  struct JFieldVal *next;
  char name[32];
  uint8_t is_obj;             /* v.l is a JObj reference held by the field */
  jvalue v;
} JFieldVal;

struct JClass {
  char name[112];             /* "android/content/Context" */
  JClass *super;
  JObj *obj;                  /* its java.lang.Class instance */
};

#define JMETH_MAGIC 0x4a4d4944u  /* 'JMID': JMethod.magic */

typedef jvalue (*JMethodFn)(JObj *self, const jvalue *args, const JMethod *m);
typedef jvalue (*JFieldFn)(JObj *self, const JField *f);

typedef struct { const char *cls, *name, *sig; JMethodFn fn; } JMethodDef; /* sig NULL: any */
/* get NULL + ival/sval: a platform constant (static final), see jni_android.c */
typedef struct { const char *cls, *name; JFieldFn get; jint ival; const char *sval; } JFieldDef;

struct JMethod {
  uint32_t magic;
  JClass *cls;                /* class it was looked up on */
  char name[64];
  char sig[224];
  uint8_t is_static;
  char ret;                   /* V Z B C S I J F D L [ */
  uint8_t warned;
  const JMethodDef *def;      /* NULL: unhandled, default return */
};

struct JField {
  uint32_t magic;
  JClass *cls;
  char name[64];
  char sig[128];
  uint8_t is_static;
  uint8_t warned;
  const JFieldDef *def;
};

/* ---------------------------------------------------------------- the VM */
extern void *g_jni_env;       /* JNIEnv*  (a pointer to the function table pointer) */
extern void *g_jni_vm;        /* JavaVM*  */

void jni_init(void);

/* Handler tables and the class hierarchy (jni_android.c). */
extern const JMethodDef jni_method_defs[];
extern const JFieldDef jni_field_defs[];
extern const char *const jni_class_supers[][2];   /* {sub, super}, NULL-terminated */
extern const char *const jni_missing_classes[];   /* fallback when classes.txt is absent */

/* Does a phone running this APK have the class (a/b/C$D)? The APK's own classes
 * (<root>/classes.txt, from tools/stage_sd.py) plus the Android framework. */
int jni_class_exists(const char *name);

/* ------------------------------------------------------------ objects */
JClass *jni_class(const char *name);              /* interned; never NULL */
JObj *jni_new(const char *cls);                   /* refs = 1 */
JObj *jni_singleton(const char *cls);             /* immortal, one per class */
JObj *jni_str(const char *utf);                   /* NULL in, NULL out */
JObj *jni_str_fmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
JObj *jni_array(char elem, jsize len);            /* zeroed; elem 'L' or ZBCSIJFD */
const char *jni_utf(const void *jstring);         /* "" for NULL / non-strings */
int jni_is(const void *obj, const char *cls);     /* instance of (walks supers) */
JObj *jni_retain(JObj *o);
void jni_release(JObj *o);
void jni_throw(const char *cls, const char *msg); /* pending exception */

/* Method IDs and calls from C (the same objects GetMethodID / Call*Method use). */
JMethod *jni_method(JClass *c, const char *name, const char *sig, int is_static);
/* As jni_method, but when no handler has this exact signature, one with the
 * same name and argument count (C# queries by its own argument types). */
JMethod *jni_method_best(JClass *c, const char *name, const char *sig, int is_static);
JField *jni_field(JClass *c, const char *name, const char *sig, int is_static);
JClass *jni_class_of(const void *class_obj);      /* a java.lang.Class -> JClass, or NULL */
jvalue jni_call(JObj *self, JMethod *m, const jvalue *args);
void jni_exception_report(const char *where);    /* log + clear a pending one */

/* Instance fields of objects the port builds (event objects, MotionRange...).
 * GetXxxField on a field with no JFieldDef reads these; SetXxxField writes them.
 * is_object: the value is a JObj the field keeps a reference to. */
void jni_set_field(JObj *o, const char *name, jvalue val, int is_object);
jvalue jni_get_field(JObj *o, const char *name);
extern int g_jni_log; /* log every call (config.ini [debug] log_java_calls) */
int jni_live_objects(void); /* Java objects alive now (a leak shows as growth) */

/* --------------------------------------------------- natives (engine side) */
/* RegisterNatives capture: the function the engine registered, or NULL. */
void *jni_native(const char *cls, const char *name);

/* jvalue helpers for handlers */
static inline jvalue jv_i(jint i) { jvalue v; v.j = 0; v.i = i; return v; }
static inline jvalue jv_z(int z) { jvalue v; v.j = 0; v.z = z ? 1 : 0; return v; }
static inline jvalue jv_j(jlong j) { jvalue v; v.j = j; return v; }
static inline jvalue jv_f(jfloat f) { jvalue v; v.j = 0; v.f = f; return v; }
static inline jvalue jv_d(jdouble d) { jvalue v; v.d = d; return v; }
static inline jvalue jv_l(void *l) { jvalue v; v.j = 0; v.l = l; return v; }
static inline jvalue jv_none(void) { jvalue v; v.j = 0; return v; }


#endif /* DCR_JNI_H */
