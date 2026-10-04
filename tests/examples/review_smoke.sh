#!/bin/sh
# Makes a one-scene review, pins a note in a hidden window by script, saves a
# screenshot, and checks the note reached the file; then the same for a
# skinned character, whose notes also name the bone, the clip and the camera. Exits 77 (skipped) when
# no OpenGL window can be made here.
review=$1 scene=$2 work=$3 character=$4
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

# --select opens a note in the list; a note the review lacks is refused.
rm -f "$work/select.png"
"$review" "$work/smoke.vreview" --select 1 --screenshot "$work/select.png" || exit 1
[ -s "$work/select.png" ] || { echo "No screenshot with a note open"; exit 1; }
if "$review" "$work/smoke.vreview" --select 99 --hidden --frames 1 2>/dev/null; then
    echo "--select of a missing note did not fail"
    exit 1
fi

# Comparison pins must name the requested view, not the first view whose
# uninitialized bounds happen to contain the point.
rm -f "$work/comparison.png"
"$review" --new "$work/comparison.vreview" --candidate "A" "$scene" \
    --candidate "B" "$scene" --candidate "C" "$scene" --candidate "D" "$scene" --replace --no-open || exit 1
"$review" "$work/comparison.vreview" --note D 0.5 0.5 "Fourth view" --screenshot "$work/comparison.png" || exit 1
grep -q 'candidate = "D";' "$work/comparison.vreview" || { echo "Wrong pinned candidate"; exit 1; }

# A load error is useful onscreen, but isn't a successfully pinned scene.
"$review" --new "$work/missing.vreview" --candidate "Missing" "$work/no-such-scene.vscene" --replace --no-open || exit 1
"$review" "$work/missing.vreview" --note A 0.5 0.5 "Must not save" --hidden --frames 1
[ "$?" -eq 1 ] || { echo "Pinning an unloaded scene did not fail"; exit 1; }
if grep -q 'text = "Must not save";' "$work/missing.vreview"; then
    echo "A failed scene received a note"
    exit 1
fi
echo "Comparison and failed-load pin tests passed"

# A character review: a click names the bone under it, and keeps the clip and camera.
rm -f "$work/character.png" "$work/character_select.png"
"$review" --new "$work/character.vreview" --candidate "Soldier" "$character" --replace --no-open || exit 1
"$review" "$work/character.vreview" --note A 0.5 0.42 "Chest" --screenshot "$work/character.png" || exit 1
grep -q 'object_name = "DEF-' "$work/character.vreview" || { echo "The character note names no bone"; exit 1; }
grep -q 'clip = "walk";' "$work/character.vreview" || { echo "The character note has no clip"; exit 1; }
grep -q 'camera = \[' "$work/character.vreview" || { echo "The character note has no camera"; exit 1; }
"$review" "$work/character.vreview" --select 1 --screenshot "$work/character_select.png" || exit 1
echo "Character review tests passed"

# A click that replaces an empty note takes its number: no gaps from unwritten notes.
"$review" --new "$work/numbering.vreview" --candidate "Editor timeline" "$scene" --replace --no-open || exit 1
"$review" "$work/numbering.vreview" --note A 0.4 0.5 "" --note A 0.6 0.5 "Second click" --hidden --frames 30 || exit 1
grep -q 'id = 1;' "$work/numbering.vreview" || { echo "The note after an empty one is not number 1"; exit 1; }
if grep -q 'id = 2;' "$work/numbering.vreview"; then
    echo "An empty note used up a number"
    exit 1
fi
echo "Note numbering test passed"
