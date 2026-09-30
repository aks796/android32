#!/usr/bin/env python3
"""gen_imports.py -- generate a port's source/imports.c, the import table.

Every symbol the game's modules import (and no other loaded module exports)
must map to a host function or object. gl* entry points are not in the table:
the loader resolves them through the GL layer (so_util.c). The rules, in order:

  1. a shim named b_<symbol> defined in the runtime's or the port's
     source/*.c|*.S                                  (bionic ABI shims)
  2. DATA: the object shims below (bionic data symbols)
  3. PASSTHROUGH: newlib/libm functions whose bionic and newlib ABIs agree
  4. weak imports with no implementation are left out on purpose: the loader
     binds them to NULL, which is what the modules test for (__data_start for
     the Boehm GC's data-segment probe, the EHABI __cxa_* hooks)

Anything left over is an error: the table must be complete.

Sources scanned for shims: the files runtime.mk builds -- the port's source/,
plus runtime/source minus the files the port replaces (same name, .c or .S)
or excludes (EXCLUDE below, the Makefile's RT_EXCLUDE).

The per-port inputs, both in the port's tools/:
  imports.cfg         MODULES (the libraries to scan), and the port's extra
                      PASSTHROUGH, WEAK_NULL and DATA entries (format below)
  imports_needed.txt  the needed set, cached from the game's own libraries
                      (--libs DIR), so the build does not depend on game
                      files. Symbol NAMES are an interface, not game content.

imports.cfg, one directive per line, repeated as needed; # starts a comment:
  MODULES      libfoo.so libbar.so
  PASSTHROUGH  fmax strlcat            (extra ABI-compatible newlib functions)
  WEAK_NULL    some_weak_hook  why it is left NULL
  DATA         bionic_object b_host_object
  EXCLUDE      opensles.c              (as the Makefile's RT_EXCLUDE)

The output declares dcr_imports[] and dcr_imports_count only; the search over
it (dcr_import_lookup, with the port's port_imports[] overlay in front) is the
runtime's source/imports_lookup.c.

Run it from the port's folder (the runtime at runtime/):
  python3 runtime/tools/gen_imports.py --libs <apk>/lib/armeabi-v7a  # refresh the cache
  python3 runtime/tools/gen_imports.py            # regenerate source/imports.c
  python3 runtime/tools/gen_imports.py --check    # verify only
(the libs: unzip -j <the game's APK> 'lib/armeabi-v7a/*' -d /tmp/libs)
"""
import argparse, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.dirname(HERE)
RUNTIME_SRC = os.path.join(RUNTIME, "source")

# Functions whose bionic (armeabi-v7a) and newlib (devkitARM) signatures, types
# and semantics agree: no struct layout, no errno > 34, no fd, no time_t/off_t,
# no FILE, no mbstate_t crosses them. Anything NOT like that has a b_ shim
# (which wins: rule 1 comes first). The union of the seven ports' lists.
PASSTHROUGH = """
acos acosf asin asinf atan atan2 atan2f atanf cos cosf cosh sin sinf sinh tan tanf tanh
exp exp2 exp2f expf log log10 log10f logf pow powf fmod fmodf frexp ldexp remainder fmax
atoi atol strtod strtol strtoul strtoll strtoull
bsearch qsort div lrand48 srand48
malloc calloc realloc free memalign
memchr memcmp memmem
strcasecmp strncasecmp strcasestr strcat strncat strchr strrchr strcmp strncmp strcoll
strcpy strncpy strdup strlcpy strlcat strlen strpbrk strsep strstr strtok strtok_r strxfrm
strptime
isalnum isalpha iscntrl islower ispunct isspace isupper isxdigit tolower toupper
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


def read_cfg(path):
    """imports.cfg -> (modules, passthrough, weak_null, data, exclude)."""
    modules, passthrough, weak_null, data, exclude = [], [], {}, {}, []
    if not os.path.exists(path):
        sys.exit(f"gen_imports: {path} missing (it names the port's MODULES)")
    for no, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        key, rest = (line.split(None, 1) + [""])[:2]
        key, rest = key.upper(), rest.strip()
        if key == "MODULES":
            modules += rest.split()
        elif key == "PASSTHROUGH":
            passthrough += rest.split()
        elif key == "WEAK_NULL":
            name, why = (rest.split(None, 1) + [""])[:2]
            weak_null[name] = why.strip() or "port: left NULL"
        elif key == "DATA":
            parts = rest.split()
            if len(parts) != 2:
                sys.exit(f"{path}:{no}: DATA takes <bionic symbol> <host object>")
            data[parts[0]] = parts[1]
        elif key == "EXCLUDE":
            exclude += rest.split()
        else:
            sys.exit(f"{path}:{no}: unknown directive {key} (MODULES, PASSTHROUGH, WEAK_NULL, DATA, EXCLUDE)")
    if not modules:
        sys.exit(f"{path}: no MODULES")
    return modules, passthrough, weak_null, data, exclude


def needed_from_libs(libdir, modules):
    from elftools.elf.elffile import ELFFile
    und, exp = {}, set()
    for m in modules:
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


def source_files(port_src, out_path, exclude):
    """The files runtime.mk builds: the port's, then the runtime's that the
    port neither replaces (a file of the same base name, .c or .S) nor
    excludes."""
    def listing(d):
        if not os.path.isdir(d):
            return []
        return sorted(fn for fn in os.listdir(d) if fn.endswith((".c", ".S")))
    base = lambda fn: os.path.splitext(fn)[0]
    port_files = listing(port_src)
    skip = {base(fn) for fn in port_files} | {base(fn) for fn in exclude}
    paths = [os.path.join(port_src, fn) for fn in port_files]
    paths += [os.path.join(RUNTIME_SRC, fn) for fn in listing(RUNTIME_SRC) if base(fn) not in skip]
    out = os.path.abspath(out_path)
    return [p for p in paths if os.path.abspath(p) != out]


def shim_symbols(paths):
    defined = set()
    fn_re = re.compile(r"^[A-Za-z_][\w \*\(\),]*?\b(b_\w+)\s*\(", re.M)
    var_re = re.compile(r"^[A-Za-z_][\w \*]*?\b(b_\w+)\s*(?:\[[^\]]*\])?\s*(?:=|;)", re.M)
    macro_re = re.compile(r"^\s*(?:VF64_1|VF32_1)\((\w+)", re.M)
    for p in paths:
        text = open(p, encoding="utf-8", errors="replace").read()
        if p.endswith(".c"):
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
        elif p.endswith(".S"):
            for m in re.finditer(r"^FUNC\s+(b_\w+)", text, re.M):
                defined.add(m.group(1))
    return defined


def main():
    ap = argparse.ArgumentParser(description="generate the port's source/imports.c")
    ap.add_argument("--libs", help="directory holding the game's armeabi-v7a libraries (refreshes the cache)")
    ap.add_argument("--check", action="store_true", help="verify only; do not write")
    ap.add_argument("--port", default=".", help="the port's folder (default: the current one)")
    ap.add_argument("--cfg", help="imports.cfg (default: <port>/tools/imports.cfg)")
    ap.add_argument("--needed", help="the cache (default: <port>/tools/imports_needed.txt)")
    ap.add_argument("--source", help="the port's sources (default: <port>/source)")
    ap.add_argument("--out", help="the output (default: <port>/source/imports.c)")
    a = ap.parse_args()

    port = os.path.abspath(a.port)
    cfg = a.cfg or os.path.join(port, "tools", "imports.cfg")
    cache = a.needed or os.path.join(port, "tools", "imports_needed.txt")
    port_src = a.source or os.path.join(port, "source")
    out_path = a.out or os.path.join(port_src, "imports.c")
    if os.path.abspath(port) == RUNTIME:
        sys.exit("gen_imports: run it from the port's folder (or pass --port)")

    modules, more_pass, more_weak, more_data, exclude = read_cfg(cfg)
    passthrough = set(PASSTHROUGH) | set(more_pass)
    weak_null = dict(WEAK_NULL, **more_weak)
    data = dict(DATA, **more_data)

    if a.libs:
        need = needed_from_libs(a.libs, modules)
        with open(cache, "w") as f:
            f.write("# symbol weak(0/1) -- regenerate with gen_imports.py --libs\n")
            for n in sorted(need):
                f.write(f"{n} {int(need[n])}\n")
    else:
        need = {}
        for line in open(cache):
            if line.startswith("#") or not line.strip():
                continue
            n, w = line.split()
            need[n] = w == "1"

    shims = shim_symbols(source_files(port_src, out_path, exclude))
    rows, missing, unbound = [], [], []
    gl = sorted(n for n in need if re.match(r"gl[A-Z]", n))
    for n in gl:
        del need[n]
    for n in sorted(need):
        if "b_" + n in shims:
            rows.append((n, "b_" + n))
        elif n in data:
            rows.append((n, data[n]))
        elif n in passthrough:
            rows.append((n, n))
        elif n in weak_null and need[n]:
            unbound.append(n)
        elif need[n]:
            unbound.append(n)
            print(f"note: weak import {n} left NULL (no implementation)", file=sys.stderr)
        else:
            missing.append(n)

    stale = sorted(p for p in more_pass if p not in need)
    if stale:
        print("note: imports.cfg PASSTHROUGH entries not imported by this build:", " ".join(stale), file=sys.stderr)
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

    mods = "/".join(m[:-3] if m.endswith(".so") else m for m in modules)
    out = []
    out.append("/* imports.c -- GENERATED by runtime/tools/gen_imports.py; do not edit.\n"
               " *\n"
               f" * Every import of {mods} that no loaded module\n"
               " * exports (gl* aside: the GL layer serves those), mapped to the host\n"
               " * function or object that serves it: b_* shims for the bionic ABI\n"
               " * (bionic_*.c, android_ndk.c, gl_*.c, the port's files), and newlib\n"
               " * directly where the two ABIs agree. Declared through asm labels so no\n"
               " * C prototype is needed (or can conflict): only the address is taken\n"
               " * here. The search over the table is the runtime's imports_lookup.c.\n"
               " *\n"
               " * Weak imports bound to NULL on purpose: " + (", ".join(unbound) or "none") + ".\n"
               " * MIT.\n */\n")
    out.append("#include \"imports.h\"\n\n")
    targets = sorted(set(t for _, t in rows))
    for t in targets:
        out.append(f"extern const char sym_{t}[] __asm__(\"{t}\");\n")
    out.append("\nDynLibFunction dcr_imports[] = {\n")
    for n, t in rows:
        out.append(f"    {{\"{n}\", (uintptr_t)sym_{t}}},\n")
    out.append("    {\"\", 0},\n};\n\n")
    out.append("int dcr_imports_count = (int)(sizeof(dcr_imports) / sizeof(dcr_imports[0])) - 1;\n")
    with open(out_path, "w") as f:
        f.write("".join(out))
    print(f"wrote {os.path.relpath(out_path)}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
