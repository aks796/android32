#!/bin/sh
# export_github.sh -- lay out <port>/github_repo/: what goes on GitHub.
#
#   runtime/tools/export_github.sh          (from anywhere; PORT_DIR names the
#                                            port folder, else it is the folder
#                                            two above this script as called)
#
# What is copied: the paths in the port's github_export.list (one per line,
# relative to the port folder; # comments; shell globs), else README.md,
# NOTES.md, LICENSE, Makefile, build.sh, .gitignore, icon.*, source/, tools/
# and launcher/. Never copied, whatever the list says: build outputs (build*/,
# *.elf, *.nso, *.nsp, *.nro, *.nacp, *.npdm, *.map, *.o, the root's *.build),
# SD_CARD*, the game's files (*.apk, *.obb, *.ipa, *.so, *.dll), debug/ and
# debug logs and dumps, portlibs32's libraries, caches, backups/, github_repo/
# and runtime/; and, when the port folder is a git checkout, whatever its
# .gitignore ignores (files git already tracks stay, as with git ls-files).
#
# EXPORT_OUT names another github_repo folder (it must be named github_repo).
# github_repo/ is emptied first -- all but its .git (the checkout of the GitHub
# repo) and runtime/ (the submodule's folder) -- never with rsync --delete.
# The runtime is not copied: .gitmodules names it, and when github_repo/.git
# exists the runtime's current commit is pinned in its index (a gitlink, as
# `git submodule add` makes). Nothing is committed or pushed, and no git
# configuration is changed: review, commit and push by hand. MIT.
set -eu

if [ -n "${PORT_DIR:-}" ]; then
  HERE="$(cd "$PORT_DIR" && pwd -P)"
else
  # lexically: a symlinked runtime/ still leads back to the port
  HERE="$(cd -L "$(dirname "$0")/../.." && pwd -P)"
fi
OUT="${EXPORT_OUT:-$HERE/github_repo}"

die() { echo "export_github.sh: $*" >&2; exit 1; }

# ------------------------------------------------------------ safety first
[ -n "$HERE" ] && [ "$HERE" != / ] || die "refusing: the port folder is '$HERE'"
[ "$HERE" != "$(cd "${HOME:-/}" && pwd -P)" ] || die "refusing: the port folder is the home folder"
[ -f "$HERE/Makefile" ] && [ -d "$HERE/source" ] ||
  die "refusing: $HERE is not a port folder (no Makefile or source/); set PORT_DIR"
[ -n "$OUT" ] || die "refusing: OUT is empty"
case "$OUT" in /*) ;; *) OUT="$HERE/$OUT" ;; esac
[ "$(basename "$OUT")" = github_repo ] || die "refusing: OUT must be a folder named github_repo ($OUT)"
[ ! -L "$OUT" ] || die "refusing: $OUT is a symlink"
[ ! -e "$OUT" ] || [ -d "$OUT" ] || die "refusing: $OUT is not a folder"
[ -d "$(dirname "$OUT")" ] || die "refusing: $(dirname "$OUT") does not exist"
OUT="$(cd "$(dirname "$OUT")" && pwd -P)/github_repo" # (resolved before anything is made)
[ "$OUT" != "$HERE" ] || die "refusing: OUT is the port folder"
case "$HERE/" in "$OUT"/*) die "refusing: OUT ($OUT) holds the port folder" ;; esac
if [ -e "$HERE/runtime" ]; then
  RT_REAL="$(cd -P "$HERE/runtime" && pwd)"
  case "$OUT/" in "$RT_REAL"/*) die "refusing: OUT ($OUT) is inside the runtime" ;; esac
else
  RT_REAL=""
fi

# ------------------------------------------------------------ the file list
cd "$HERE"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/export_github.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT INT TERM
if [ -f github_export.list ]; then
  sed -e 's/#.*//' -e 's/[[:space:]]*$//' -e '/^$/d' github_export.list > "$TMP/entries"
else
  printf '%s\n' README.md NOTES.md LICENSE Makefile build.sh .gitignore 'icon.*' \
    source tools launcher > "$TMP/entries"
fi
: > "$TMP/files"
while IFS= read -r pat; do
  case "$pat" in /* | *..*) echo "export_github.sh: skipping '$pat' (outside the port folder)" >&2; continue ;; esac
  # shellcheck disable=SC2086 # the entry's glob is expanded on purpose
  for p in $pat; do
    if [ -d "$p" ] && [ ! -L "$p" ]; then
      find "$p" \( -name .git -o -name build -o -name 'build-*' -o -name __pycache__ \) -prune \
        -o \( -type f -o -type l \) -print >> "$TMP/files"
    elif [ -f "$p" ] || [ -L "$p" ]; then
      printf '%s\n' "$p" >> "$TMP/files"
    fi
  done
done < "$TMP/entries"

# never, whatever the list says
sed 's|^\./||' "$TMP/files" | grep -v -E \
  -e '(^|/)(build|build-[^/]*|__pycache__)/' -e '(^|/)\.git(/|$)' \
  -e '^(debug|SD_CARD|backups|github_repo|runtime|launcher/romfs|portlibs32/(lib|include|share|bin))(/|$)' \
  -e '\.(elf|nso|nsp|nro|nacp|npdm|map|o|d|a|pyc|apk|apks|xapk|obb|ipa|so|dll|orig|rej|bak|swp)$' \
  -e '(^|/)SD_CARD[^/]*\.zip$' -e '^[^/]*\.build$' -e '(^|/)(stack_[^/]*|jit_arena)\.bin$' \
  -e '(^|/)debug[^/]*\.log$' -e '(^|/)\.DS_Store$' -e '~$' \
  | sort -u > "$TMP/keep" || true

# and nothing the port's own .gitignore keeps out of git
if git -C "$HERE" rev-parse --show-toplevel >/dev/null 2>&1 &&
   [ "$(git -C "$HERE" rev-parse --show-toplevel)" = "$HERE" ]; then
  # git refuses a path behind a symlink (a linked portlibs32/, say) and stops
  # there, leaving the rest unchecked: those paths are named in the list on
  # purpose and are kept; the others are asked about.
  : > "$TMP/linked"
  : > "$TMP/plain"
  while IFS= read -r p; do
    d="$(dirname "$p")"
    linked=0
    while [ "$d" != "." ] && [ "$d" != "/" ]; do
      [ -L "$HERE/$d" ] && linked=1
      d="$(dirname "$d")"
    done
    if [ "$linked" = 1 ]; then echo "$p" >> "$TMP/linked"; else echo "$p" >> "$TMP/plain"; fi
  done < "$TMP/keep"
  git -C "$HERE" check-ignore --stdin < "$TMP/plain" > "$TMP/ignored" || true
  if [ -s "$TMP/ignored" ]; then
    grep -v -x -F -f "$TMP/ignored" "$TMP/plain" > "$TMP/keep2" || true
    mv "$TMP/keep2" "$TMP/plain"
  fi
  cat "$TMP/plain" "$TMP/linked" | sort -u > "$TMP/keep"
fi
[ -s "$TMP/keep" ] || die "nothing to copy (the list: $(tr '\n' ' ' < "$TMP/entries"))"

# ------------------------------------------------------------ empty it, copy
# (keeps github_repo/.git -- the published repository -- and runtime/)
mkdir -p "$OUT"
find "$OUT" -mindepth 1 -maxdepth 1 ! -name .git ! -name runtime -exec rm -rf {} +
tar -cf - -T "$TMP/keep" | tar -xf - -C "$OUT"
if [ -f "$HERE/github_export.gitignore" ]; then
  cp "$HERE/github_export.gitignore" "$OUT/.gitignore"
elif [ ! -f "$OUT/.gitignore" ]; then
  cat > "$OUT/.gitignore" <<'EOF'
build/
launcher/build/
launcher/romfs/
*.nsp
*.nro
*.nacp
*.elf
*.build
SD_CARD/
SD_CARD.zip
portlibs32/include/
portlibs32/lib/
debug/
*.apk
.DS_Store
EOF
fi

# ------------------------------------------------------------ the runtime
URL="${RT_GITHUB_URL:-}"
if [ -z "$URL" ] && [ -n "$RT_REAL" ]; then
  URL="$(git -C "$RT_REAL" remote get-url origin 2>/dev/null || true)"
fi
[ -n "$URL" ] || URL=https://github.com/aks796/android32.git
cat > "$OUT/.gitmodules" <<EOF
[submodule "runtime"]
	path = runtime
	url = $URL
EOF
mkdir -p "$OUT/runtime" # (without it, git add -A would drop the submodule)
PIN=""
if [ -n "$RT_REAL" ] && git -C "$RT_REAL" rev-parse --verify -q HEAD >/dev/null 2>&1; then
  PIN="$(git -C "$RT_REAL" rev-parse HEAD)"
  if [ -n "$(git -C "$RT_REAL" status --porcelain --untracked-files=no 2>/dev/null)" ]; then
    echo "export_github.sh: WARNING: the runtime has uncommitted changes; the pin is its last commit" >&2
  fi
fi
if [ -e "$OUT/.git" ]; then
  if [ -n "$PIN" ]; then
    # a runtime once copied in as plain files leaves the index first (the files stay)
    if [ -n "$(git -C "$OUT" ls-files -- runtime/ 2>/dev/null)" ]; then
      git -C "$OUT" rm -r -q --cached --ignore-unmatch -- runtime
    fi
    git -C "$OUT" update-index --add --cacheinfo "160000,$PIN,runtime"
    echo "runtime pinned at $PIN (github_repo's index; .gitmodules: $URL)"
  else
    echo "export_github.sh: WARNING: the runtime is not a git checkout: nothing pinned" >&2
  fi
else
  echo "github_repo/ has no .git: .gitmodules written, the runtime's commit${PIN:+ ($PIN)} not pinned"
fi

find "$OUT" -name .DS_Store -delete 2>/dev/null || true
echo "github_repo/: $(find "$OUT" -path "$OUT/.git" -prune -o -path "$OUT/runtime" -prune -o -type f -print | wc -l | tr -d ' ') files"
echo "nothing was committed or pushed: review it, then commit and push by hand"
