#!/bin/sh
# Build labyrinth2_nx.nro (the launcher) in devkitPro's 64-bit toolchain
# container. Build the wrapper first (../build.sh): the NRO carries
# ../labyrinth2_nx.nsp and ../labyrinth2_nx.build.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -f "$HERE/../labyrinth2_nx.nsp" ] && [ -f "$HERE/../labyrinth2_nx.build" ] || { echo "build the wrapper first (../build.sh)"; exit 1; }
# The iPad game's files the port uses, into the romfs as ipad.ipa
# (../tools/make_ipad_assets.py; the game copies them out on its first
# start): from L2_IPA=<Labyrinth 2 HD .ipa>, else the first .ipa kept in
# ../SD_CARD/switch/labyrinth2_nx/ or beside the project. None: the NRO
# carries no iPad files (a player's own .ipa in the game folder still works).
IPA="${L2_IPA:-}"
if [ -z "$IPA" ]; then
  for f in "$HERE"/../SD_CARD/switch/labyrinth2_nx/*.ipa "$HERE"/../../*.ipa; do
    [ -f "$f" ] && { IPA="$f"; break; }
  done
fi
mkdir -p "$HERE/romfs"
if [ -n "$IPA" ]; then
  python3 "$HERE/../tools/make_ipad_assets.py" "$IPA" "$HERE/romfs/ipad.ipa.new"
  if cmp -s "$HERE/romfs/ipad.ipa.new" "$HERE/romfs/ipad.ipa"; then
    rm -f "$HERE/romfs/ipad.ipa.new"
  else
    mv "$HERE/romfs/ipad.ipa.new" "$HERE/romfs/ipad.ipa"
    rm -f "$HERE/labyrinth2_nx.nro" # repacked with it
  fi
elif [ -f "$HERE/romfs/ipad.ipa" ]; then
  rm -f "$HERE/romfs/ipad.ipa" "$HERE/labyrinth2_nx.nro"
  echo "no Labyrinth 2 HD .ipa (set L2_IPA): the NRO carries no iPad files"
fi
exec docker run --rm --platform linux/amd64 \
  -v "$HERE/..:/work" -w /work/launcher devkitpro/devkita64:latest \
  bash -lc "make -j\$(nproc) $*"
