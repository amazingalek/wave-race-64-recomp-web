#!/usr/bin/env bash
# Patches the submodules for the web page (docs/BUILDING.md): N64Recomp-browser's
# web support first (its patches/apply.cmake, each project by name, since its
# patches.txt says rt64 and the folder here is RT64), then this port's own
# patch scripts, which find the web support in and pick their versions for it
# (tools/patch_rt64_water.py). Every submodule is reset first, so it can be run
# again at any time. Run from the repository root.
set -euo pipefail
cd "$(dirname "$0")/.."
python="$(command -v python3 || command -v python)"
for sub in lib/RT64 lib/N64ModernRuntime lib/RecompFrontend; do
    git -C "$sub" submodule foreach --recursive -q 'git reset -q --hard && git clean -fdq'
    git -C "$sub" reset -q --hard
    git -C "$sub" clean -fdq
done
# apply.cmake's stamps from an earlier run, which say a three-way merge is in.
find "$(git rev-parse --git-common-dir)/modules" -name 'n64recomp-browser-*' -delete
apply=lib/N64Recomp-browser/patches/apply.cmake
for p in "rt64 lib/RT64" "plume lib/RT64/src/contrib/plume" "hlslpp lib/RT64/src/contrib/hlslpp" \
         "N64ModernRuntime lib/N64ModernRuntime" "N64Recomp lib/N64ModernRuntime/N64Recomp" \
         "RecompFrontend lib/RecompFrontend"; do
    cmake -P "$apply" $p
done
for script in patch_rt64.py patch_n64recomp.py patch_rsprecomp.py patch_librecomp.py patch_recompinput.py \
              patch_runtime_shutdown.py; do
    "$python" "tools/$script"
done
