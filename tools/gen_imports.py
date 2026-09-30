#!/usr/bin/env python3
"""gen_imports.py -- generate source/imports.c, the import table.

Every symbol liblabyrinthii.so imports must map to a host function or object. gl* entry points are
not in the table: the loader resolves them through the GL layer (so_util.c).
The rules, in order:

  1. a shim named b_<symbol> defined in source/*.c|*.S      (bionic ABI shims)
  2. DATA: the object shims below (bionic data symbols)
  3. PASSTHROUGH: newlib/libm functions whose bionic and newlib ABIs agree
  4. weak imports with no implementation are left out on purpose: the loader
     binds them to NULL, which is what the modules test for (__data_start for
     the Boehm GC's data-segment probe, the EHABI __cxa_* hooks)

Anything left over is an error: the table must be complete.

The needed set comes from the game's own libraries when they are available
(--libs DIR), and is cached in tools/imports_needed.txt so the build does not
depend on game files. Symbol NAMES are an interface, not game content.

  python3 tools/gen_imports.py [--libs path/to/lib/armeabi]
"""
import argparse, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "source")
CACHE = os.path.join(HERE, "imports_needed.txt")
MODULES = ["liblabyrinthii.so"]

# Functions whose bionic (armeabi-v7a) and newlib (devkitARM) signatures, types
# and semantics agree: no struct layout, no errno > 34, no fd, no time_t/off_t,
# no FILE, no mbstate_t crosses them. Anything NOT like that has a b_ shim.
PASSTHROUGH = """
acos acosf asin asinf atan atan2 atan2f atanf cos cosf cosh sin sinf sinh tan tanf tanh
exp exp2 expf log log10 log10f logf pow powf fmod fmodf frexp ldexp remainder
atoi atol strtod strtol strtoul strtoll strtoull
bsearch qsort div lrand48 srand48
malloc calloc realloc free memalign
memchr memcmp memmem
strcasecmp strncasecmp strcat strncat strchr strrchr strcmp strncmp strcoll strcpy strncpy
strdup strlcpy strlen strpbrk strsep strstr strtok strtok_r strxfrm
isalnum isalpha isspace isxdigit tolower
towlower towupper wctype iswctype wcscoll wcslen wcsxfrm
wmemchr wmemcmp wmemcpy wmemmove wmemset
snprintf sprintf vsnprintf vsprintf vasprintf sscanf vsscanf swprintf
strcspn strspn strnlen strtof strtold lrintf
wcstod wcstof wcstol wcstold wcstoll wcstoul wcstoull
__errno
__aeabi_d2lz __aeabi_d2ulz __aeabi_dcmpgt __aeabi_dcmplt __aeabi_f2d __aeabi_f2lz
__aeabi_f2ulz __aeabi_fcmpgt __aeabi_fcmplt __aeabi_idiv __aeabi_idivmod __aeabi_l2d
__aeabi_l2f __aeabi_ul2d
""".split()

# bionic data symbols -> the host object that plays them
DATA = {
    "__sF": "b___sF",
    "__stack_chk_guard": "b___stack_chk_guard",
    "__page_size": "b___page_size",
    "_ctype_": "b__ctype_",
    "_tolower_tab_": "b__tolower_tab_",
    "_toupper_tab_": "b__toupper_tab_",
    "environ": "b_environ",
}

# Weak imports deliberately left unbound (NULL).
WEAK_NULL = {
    "__data_start": "Boehm GC: NULL makes it use the module's own data start",
    "data_start": "Boehm GC (as above)",
    "__cxa_begin_cleanup": "EHABI hook; weak, tested for NULL by the unwinder",
    "__cxa_type_match": "EHABI hook; weak, tested for NULL by the unwinder",
    "__cxa_call_unexpected": "EHABI hook; weak",
}


def needed_from_libs(libdir):
    from elftools.elf.elffile import ELFFile
    und, exp = {}, set()
    for m in MODULES:
        with open(os.path.join(libdir, m), "rb") as f:
            e = ELFFile(f)
            for s in e.get_section_by_name(".dynsym").iter_symbols():
                if not s.name:
                    continue
                if s["st_shndx"] == "SHN_UNDEF":
                    weak = s["st_info"]["bind"] == "STB_WEAK"
                    und[s.name] = und.get(s.name, True) and weak
                elif s["st_info"]["bind"] != "STB_LOCAL":
                    exp.add(s.name)
    return {n: w for n, w in und.items() if n not in exp}


def shim_symbols():
    defined = set()
    fn_re = re.compile(r"^[A-Za-z_][\w \*\(\),]*?\b(b_\w+)\s*\(", re.M)
    var_re = re.compile(r"^[A-Za-z_][\w \*]*?\b(b_\w+)\s*(?:\[[^\]]*\])?\s*(?:=|;)", re.M)
    macro_re = re.compile(r"^\s*(?:VF64_1|VF32_1)\((\w+)", re.M)
    for fn in sorted(os.listdir(SRC)):
        p = os.path.join(SRC, fn)
        text = open(p, encoding="utf-8", errors="replace").read()
        if fn.endswith(".c"):
            text_nc = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
            for m in fn_re.finditer(text_nc):
                line = text_nc[m.start():text_nc.find("\n", m.start())]
                if line.lstrip().startswith("static") or line.rstrip().endswith(";") and "(" in line and "{" not in line and "alias" not in line:
                    # a prototype, not a definition (aliases count as definitions)
                    continue
                defined.add(m.group(1))
            for m in var_re.finditer(text_nc):
                if not text_nc[m.start():m.end()].lstrip().startswith(("static", "extern", "return")):
                    defined.add(m.group(1))
            for m in macro_re.finditer(text_nc):
                defined.add("b_" + m.group(1))
            for m in re.finditer(r"^PATH_OP\((\w+)", text_nc, re.M):
                defined.add("b_" + m.group(1))
        elif fn.endswith(".S"):
            for m in re.finditer(r"^FUNC\s+(b_\w+)", text, re.M):
                defined.add(m.group(1))
    return defined


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--libs", help="directory holding the game's armeabi libraries")
    ap.add_argument("--check", action="store_true", help="verify only; do not write")
    a = ap.parse_args()

    if a.libs:
        need = needed_from_libs(a.libs)
        with open(CACHE, "w") as f:
            f.write("# symbol weak(0/1) -- regenerate with gen_imports.py --libs\n")
            for n in sorted(need):
                f.write(f"{n} {int(need[n])}\n")
    else:
        need = {}
        for line in open(CACHE):
            if line.startswith("#") or not line.strip():
                continue
            n, w = line.split()
            need[n] = w == "1"

    shims = shim_symbols()
    rows, missing, unbound = [], [], []
    gl = sorted(n for n in need if re.match(r"gl[A-Z]", n))
    for n in gl:
        del need[n]
    for n in sorted(need):
        if "b_" + n in shims:
            rows.append((n, "b_" + n))
        elif n in DATA:
            rows.append((n, DATA[n]))
        elif n in PASSTHROUGH:
            rows.append((n, n))
        elif n in WEAK_NULL and need[n]:
            unbound.append(n)
        elif need[n]:
            unbound.append(n)
            print(f"note: weak import {n} left NULL (no implementation)", file=sys.stderr)
        else:
            missing.append(n)

    stale = sorted(p for p in PASSTHROUGH if p not in need)
    if stale:
        print("note: passthrough entries not imported by this build:", " ".join(stale), file=sys.stderr)
    print(f"{len(gl)} gl* imports left to the GL layer", file=sys.stderr)
    print(f"{len(need)} imports: {len(rows)} bound "
          f"({sum(1 for _, t in rows if t.startswith('b_'))} shims, "
          f"{sum(1 for n, t in rows if t == n)} passthrough), {len(unbound)} weak->NULL, "
          f"{len(missing)} MISSING", file=sys.stderr)
    if missing:
        print("MISSING:", " ".join(missing), file=sys.stderr)
        return 1
    if a.check:
        return 0

    out = []
    out.append("/* imports.c -- GENERATED by tools/gen_imports.py; do not edit.\n"
               " *\n"
               " * Every import of liblabyrinthii.so (gl* aside: the GL layer serves\n"
               " * those),\n"
               " * mapped to the host function or object that serves it: b_* shims for the\n"
               " * bionic ABI (bionic_*.c, android_ndk.c, gl_*.c), and newlib directly where\n"
               " * the two ABIs agree. Declared through asm labels so no C prototype is\n"
               " * needed (or can conflict): only the address is taken here.\n"
               " *\n"
               " * Weak imports bound to NULL on purpose: " + ", ".join(unbound) + ".\n"
               " * MIT.\n */\n")
    out.append("#include <string.h>\n\n#include \"imports.h\"\n\n")
    targets = sorted(set(t for _, t in rows))
    for t in targets:
        out.append(f"extern const char sym_{t}[] __asm__(\"{t}\");\n")
    out.append("\nDynLibFunction dcr_imports[] = {\n")
    for n, t in rows:
        out.append(f"    {{\"{n}\", (uintptr_t)sym_{t}}},\n")
    out.append("    {\"\", 0},\n};\n\n")
    out.append("int dcr_imports_count = (int)(sizeof(dcr_imports) / sizeof(dcr_imports[0])) - 1;\n\n")
    out.append("uintptr_t dcr_import_lookup(const char *name) {\n"
               "  for (int i = 0; i < dcr_imports_count; i++)\n"
               "    if (!strcmp(dcr_imports[i].symbol, name))\n"
               "      return dcr_imports[i].func;\n"
               "  return 0;\n}\n")
    with open(os.path.join(SRC, "imports.c"), "w") as f:
        f.write("".join(out))
    print("wrote source/imports.c", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
