#!/usr/bin/env bash
# Records src/rt64_shaders.txt, the shaders the page builds before the game and a section at a time after, by code
# rather than by playing: N64Recomp-browser's tools/record_shaders.mjs serves the page built in <build dir>, drives it
# in headless Chrome through tools/shader_plan.json, a page load a section: the menus, the attract demo, and each course
# raced with tools/autopilot.js steering. What the list had before that this recording didn't meet is kept, in a last
# section, "earlier". Needs node 22 or later and Chrome, and takes about 20 minutes. Then build the page again, so that
# it prebuilds the new list. The ROM is the US Rev A one the page takes.
#
#   bash tools/record_shaders.sh <build dir> <ROM> [--profile <dir>] [--port <n>] [--only <section>,...]
set -euo pipefail
[ $# -ge 2 ] || { sed -n '9p' "$0"; exit 1; }
cd "$(dirname "$0")/.."
build="$1" rom="$2"
shift 2
before="$(mktemp)"
trap 'rm -f "$before" "$before.log"' EXIT
cp src/rt64_shaders.txt "$before"
log="$before.log"
node lib/N64Recomp-browser/tools/record_shaders.mjs tools/shader_plan.json "$build" "$rom" src/rt64_shaders.txt "$@" > "$log" 2>&1 &
recorder=$!
tail -f "$log" --pid=$recorder &
# The recorder can wait for ever once it has written the list (Chrome closes without answering Browser.close), so it
# is stopped, by its own process ID, a few seconds after it says it has.
while kill -0 $recorder 2> /dev/null; do
    if grep -q 'shaders, [0-9]* in section 0' "$log"; then sleep 15; kill $recorder 2> /dev/null || true; break; fi
    sleep 3
done
wait $recorder 2> /dev/null || true
grep -q 'shaders, [0-9]* in section 0' "$log" || { echo 'the recorder stopped before it wrote the list'; exit 1; }
# Every description of the list before that the new one lacks, in a last section.
grep -E '^[0-9a-f]{8}( [0-9a-f]{8}){4}$' src/rt64_shaders.txt | sort -u > "$before.new"
earlier="$(grep -E '^[0-9a-f]{8}( [0-9a-f]{8}){4}$' "$before" | awk 'NR == FNR { seen[$0] = 1; next } !seen[$0]++' "$before.new" -)"
rm -f "$before.new"
if [ -n "$earlier" ]; then
    printf 'section earlier\n%s\n' "$earlier" >> src/rt64_shaders.txt
    echo "$(wc -l <<< "$earlier") descriptions of the list before kept in section earlier"
fi
