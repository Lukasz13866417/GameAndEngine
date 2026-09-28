#!/bin/sh
# Makes a one-scene review, pins a note in a hidden window by script, saves a
# screenshot, and checks the note reached the file. Exits 77 (skipped) when
# no OpenGL window can be made here.
review=$1 scene=$2 work=$3
mkdir -p "$work" || exit 1
# Screenshots never overwrite, so clear the previous run's.
rm -f "$work/smoke.png"
"$review" --new "$work/smoke.vreview" --candidate "Editor timeline" "$scene" --replace --no-open || exit 1
"$review" "$work/smoke.vreview" --note A 0.5 0.5 "Smoke note" --screenshot "$work/smoke.png"
status=$?
[ "$status" -eq 0 ] || exit "$status"
grep -q 'text = "Smoke note";' "$work/smoke.vreview" || { echo "The note is not in the file"; exit 1; }
[ -s "$work/smoke.png" ] || { echo "No screenshot"; exit 1; }
echo "Review smoke test passed"
