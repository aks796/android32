#!/bin/sh
# Compile check for the runtime: builds source/*.c and *.S (or the files
# named) against test/port/port_config.h, without linking. Needs Docker, the
# vita2hos toolchain image, libnx32 and mesa32's portlibs32.
#   tools/check.sh                 # every file
#   tools/check.sh so_util.c       # some files
#   GL=0 tools/check.sh gl_null.c  # the null renderer
set -e
HERE="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${DCR_TOOLCHAIN_IMAGE:-ghcr.io/vita2hos/devcontainer/vita2hos:latest}"
LIBNX32=""
for c in "$DCR_LIBNX32" "$HERE/../libnx32/prefix" "$HERE/../../libnx32/prefix"; do
  if [ -n "$c" ] && [ -f "$c/lib/libnx.a" ]; then LIBNX32="$(cd "$c" && pwd)"; break; fi
done
[ -n "$LIBNX32" ] || { echo "check.sh: libnx32's prefix/ not found; set DCR_LIBNX32" >&2; exit 1; }
PL="${PORTLIBS32:-$HERE/test/portlibs32}"
[ -f "$PL/include/EGL/egl.h" ] || { echo "check.sh: mesa32's portlibs32 not found at $PL; set PORTLIBS32" >&2; exit 1; }
PL="$(cd -P "$PL" && pwd)"
NXD=/opt/devkitpro/libnx32
FILES="$*"
exec docker run --rm --platform linux/amd64 \
  -v "$HERE:/work" -v "$PL:/portlibs:ro" \
  -v "$LIBNX32/include/switch:$NXD/include/switch:ro" \
  -v "$LIBNX32/include/switch.h:$NXD/include/switch.h:ro" \
  -w /work "$IMAGE" \
  bash -lc "make -k -j\$(nproc) -f test/check.mk GL=${GL:-1} LIST='$FILES' 2>&1"
