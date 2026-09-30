#!/bin/sh
# build.sh -- builds a port's launcher NRO in devkitPro's 64-bit toolchain
# container (devkitpro/devkita64). A port's launcher/build.sh is:
#
#   #!/bin/sh
#   HERE="$(cd "$(dirname "$0")" && pwd)"
#   LAUNCHER_DIR="$HERE" PAYLOAD=labyrinth2_nx exec "$HERE/../runtime/launcher/build.sh" "$@"
#
# PAYLOAD is the 32-bit program's name (the wrapper's TARGET, the launcher
# Makefile's PORT_PAYLOAD). Build the wrapper first (../build.sh): the NRO
# carries ../$PAYLOAD.nsp and ../$PAYLOAD.build.
#
# Extra romfs files: if the port has launcher/romfs_extras.sh, it is sourced
# here before make, with HERE (the port's launcher folder), PORT (the port's
# folder) and ROMFS (HERE/romfs) set. It puts its files into $ROMFS and
# removes $HERE/<NRO> when they change, so that the NRO is repacked (dcr:
# the DuckTales files from a world-wide APK; lab2: the iPad files; pvz: the
# English pack). An `exit 1` there stops the build.
#
# The runtime is mounted at /runtime (the port's runtime/ may be a symlink
# that the container cannot follow) and passed to make as A32. Arguments go
# to make ("clean", ...).
set -e
: "${LAUNCHER_DIR:?set by the port launcher/build.sh}"
: "${PAYLOAD:?set by the port launcher/build.sh: the 32-bit program name}"
HERE="$(cd "$LAUNCHER_DIR" && pwd)"
PORT="$(cd "$HERE/.." && pwd)"
A32="$(cd "$(dirname "$0")/.." && pwd -P)"
IMAGE="${DCR_LAUNCHER_IMAGE:-devkitpro/devkita64:latest}"
case " $* " in
*" clean "*) ;;
*)
  [ -f "$PORT/$PAYLOAD.nsp" ] && [ -f "$PORT/$PAYLOAD.build" ] || { echo "build the wrapper first (../build.sh)"; exit 1; }
  ROMFS="$HERE/romfs"
  mkdir -p "$ROMFS"
  if [ -f "$HERE/romfs_extras.sh" ]; then
    export HERE PORT ROMFS
    . "$HERE/romfs_extras.sh"
  fi
  ;;
esac
exec docker run --rm --platform linux/amd64 \
  -v "$PORT:/work" -v "$A32:/runtime:ro" -w /work/launcher "$IMAGE" \
  bash -lc "make -j\$(nproc) A32=/runtime $*"
