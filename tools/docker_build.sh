#!/bin/sh
# docker_build.sh -- build a port inside the AArch32 Switch toolchain container
# (devkitARM, switch-tools and miniz from the vita2hos image). A port's
# ./build.sh runs this; its arguments are passed to make, e.g.
#   ./build.sh                  # <TARGET>.nsp and <TARGET>.build
#   ./build.sh clean
#   ./build.sh DCR_GL_MESA=0    # the null renderer
#   ./build.sh rt-files         # what is built from where (runtime.mk)
#
# The port folder is PORT_DIR, else the folder two above this script as it was
# called (<port>/runtime/tools/docker_build.sh -> <port>). It is mounted at
# /work. When <port>/runtime is a symlink (the runtime checked out elsewhere,
# while developing), its real folder is mounted read-only at /work/runtime;
# a git submodule is just part of /work. The same for a symlinked portlibs32.
#
# libnx itself comes from libnx32 (github.com/aks796/libnx32: the vita2hos
# AArch32 libnx with the 32-bit ports' fixes: IPC data that depended on the
# size of an enum, among it the supported-controller list that kept wireless
# controllers out). Its headers and archives are mounted over the image's; the
# image's other libraries in the same folder (miniz...) stay. Clone it next to
# the port folder and run its ./build.sh: its prefix/ is looked for at
# ../libnx32/prefix, ../../libnx32/prefix, ../../thirtytwo/libnx32/prefix
# (from the port), then next to the runtime. DCR_LIBNX32 names another
# install. MIT.
set -eu
IMAGE="${DCR_TOOLCHAIN_IMAGE:-ghcr.io/vita2hos/devcontainer/vita2hos:latest}"

if [ -n "${PORT_DIR:-}" ]; then
  HERE="$(cd "$PORT_DIR" && pwd)"
else
  # lexically: cd -L resolves ".." against the path as called, so a symlinked
  # runtime/ still leads back to the port, not to the runtime's parent
  HERE="$(cd -L "$(dirname "$0")/../.." && pwd -L)"
fi
if [ ! -f "$HERE/Makefile" ] || [ ! -e "$HERE/runtime/runtime.mk" ]; then
  echo "docker_build.sh: $HERE is not a port folder (no Makefile, or no runtime/runtime.mk)" >&2
  echo "  run the port's ./build.sh, or set PORT_DIR to the port folder" >&2
  exit 1
fi
RT_REAL="$(cd -P "$HERE/runtime" && pwd)"

# DCR_LIBNX32, when set, is the only candidate: never a different libnx than
# the one asked for. (The candidates go in front of make's arguments in "$@",
# and are shifted off again.)
if [ -n "${DCR_LIBNX32:-}" ]; then
  set -- "$DCR_LIBNX32" "$@"; NCAND=1
else
  set -- "$HERE/../libnx32/prefix" "$HERE/../../libnx32/prefix" \
         "$HERE/../../thirtytwo/libnx32/prefix" "$RT_REAL/../libnx32/prefix" \
         "$RT_REAL/../../libnx32/prefix" "$@"; NCAND=5
fi
LIBNX32=""
while [ "$NCAND" -gt 0 ]; do
  if [ -z "$LIBNX32" ] && [ -f "$1/lib/libnx.a" ] && [ -f "$1/include/switch.h" ]; then
    LIBNX32="$(cd "$1" && pwd)"
  fi
  shift; NCAND=$((NCAND - 1))
done
if [ -z "$LIBNX32" ]; then
  echo "docker_build.sh: the patched libnx32 was not found${DCR_LIBNX32:+ at $DCR_LIBNX32}" >&2
  echo "  clone github.com/aks796/libnx32 next to the port folder and run its ./build.sh," >&2
  echo "  or set DCR_LIBNX32 to an installed libnx32 (its prefix/ folder)" >&2
  exit 1
fi
if [ ! -f "$HERE/portlibs32/lib/libEGL.a" ]; then
  echo "docker_build.sh: no portlibs32/ (mesa32's lib/ and include/): building with the null renderer" >&2
fi

NXD=/opt/devkitpro/libnx32
# The docker command is built in "$@", in front of make's own arguments (so a
# path with spaces stays one argument): the container's command first.
set -- "$IMAGE" bash -lc 'exec make -j"$(nproc)" "$@"' make "$@"
set -- -w /work \
  -v "$LIBNX32/include/switch:$NXD/include/switch:ro" \
  -v "$LIBNX32/include/switch.h:$NXD/include/switch.h:ro" \
  -v "$LIBNX32/lib/libnx.a:$NXD/lib/libnx.a:ro" \
  -v "$LIBNX32/lib/libnxd.a:$NXD/lib/libnxd.a:ro" "$@"
# A symlinked folder inside /work would point at a host path the container
# does not have: its real folder is mounted where the link is.
if [ -L "$HERE/portlibs32" ] && [ -d "$HERE/portlibs32/" ]; then
  set -- -v "$(cd -P "$HERE/portlibs32" && pwd):/work/portlibs32:ro" "$@"
fi
if [ -L "$HERE/runtime" ]; then
  set -- -v "$RT_REAL:/work/runtime:ro" "$@"
fi
exec docker run --rm --platform linux/amd64 -v "$HERE:/work" "$@"
