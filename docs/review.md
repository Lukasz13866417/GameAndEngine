# Reviewing scenes

`vng_review` plays saved scenes, alone or side by side, on one shared clock.
While you watch, you can:

- click anything you see to pin a note to it;
- write down what you think of each candidate;
- pick a favourite in a comparison, or give a single scene a verdict.

Everything is kept in a small `.vreview` text file next to the scenes.
Agents read that file and answer the notes in it, and the open app shows
their answers as they arrive.

```sh
cmake --build build --target vng_review
./build/vng_review cage.vreview
```

## Making a review

```sh
./build/vng_review --new cage.vreview --title "Cage replacement" --range 30 47 \
    --candidate "Relay dish" design_A/scene.vscene --about "A 6 km dish on a boom" \
    --candidate "Propellant depot" design_B/scene.vscene \
    --candidate "Mass-driver catcher" design_C/scene.vscene
```

- **Candidates.** Each `--candidate LABEL SCENE` adds one. One candidate makes
  a single review; two to four make a comparison. They get the ids A, B, C and
  D, in order.
- **Descriptions.** `--about` describes the candidate before it; it is shown
  beside the views.
- **Clock range.** `--range FROM TO` limits the clock to the part worth
  judging. Without it, the clock covers the longest scene.
- **Lining up takes.** `--offset S` shifts one candidate's clock: at review
  time *t* it shows its scene at *t + S*. Use it for takes whose action starts
  at different moments.
- **Files only.** `--no-open` writes the file without opening it. `--replace`
  overwrites an existing file.

A review file can also be written by hand; see [the format](#the-vreview-format).

## In the app

**The views.** Each candidate plays through its own scene cameras, as the
demos do. Every view keeps a 16:10 frame, and the grid picks whichever
arrangement makes the views largest: side by side, stacked, or 2×2. A
coloured line under each header ties the candidate to its colour on the time
strip.

**Pinning a note.** A click on a view does four things:

- pauses playback;
- selects that candidate;
- pins a note to whatever is under the pointer: the scene object, and the
  exact point where the click met it (or the background, if nothing was
  there);
- puts the cursor in the note field.

Type the note. A note left empty disappears when you select something else.

**Markers.** Every note shows as a numbered circle within a second of its
time, and the selected note always shows. The circle sits on the point you
clicked, so it follows that spot as the shot moves. Colours:

- amber: open;
- green: resolved;
- a white ring: the selected note.

Click a marker to select its note instead of pinning a new one.

**The time bar** has:

- Play/Pause;
- speed, from 1x down to 1/4x;
- Loop;
- a scrubber;
- a strip with every note as a tick in its candidate's colour.

**The sidebar** shows, from top to bottom:

- the selected candidate's description;
- *Your thoughts on this one*;
- *Pick this one* in a comparison, or the verdict in a single review;
- *Notes*: every note as a row in time order, with its number, candidate,
  time, whether it was resolved or replied to, and its first line;
- *Overall*: your summary.

**The notes list.** Click a row to open its note, and click it again to
close it. Opening a note goes to its time and candidate, and opens it right
under its row:

- where it is pinned (an object's name, or the background);
- its text, ready to edit;
- the author's reply, if there is one;
- *Go to*, *Resolve* (or *Reopen*) and *Delete*.

A note opened from its marker in a view scrolls into sight. The list grows
with its notes up to about a dozen rows, then scrolls.

**Keys.** These work when the cursor is not in a text field:

| Key | Action |
| --- | --- |
| Space | Play or pause |
| Left / Right | Step 1/30 s |
| Home | Back to the start |
| Escape | Deselect the note |

**Saving.** The file saves 0.7 s after your last edit, and again when you
quit. The status line at the bottom of the sidebar says when it last saved.

## Working with an agent

The file is how you and the agent talk:

1. The agent writes the review: its candidates and what each one is.
2. You open it, watch, and leave notes and thoughts.
3. The agent reads the notes, changes the scenes, and answers each note in its
   `reply`. It may set the note's `status` to `"resolved"`.

You can leave the app open through all of this. It checks the file and the
scenes twice a second:

- **Edits to the file are merged in, not overwritten.** Your edits and the
  agent's are merged field by field. Where you both changed the same field,
  yours wins. Replies appear without a restart, and your next save keeps them.
  A note deleted on one side stays deleted, unless the other side changed it
  in the meantime.
- **New candidates, offsets or a new range reopen the review.** The app keeps
  the time, the playback state, the selected candidate and note, and every
  view whose scene is unchanged.
- **A rewritten scene file reloads just that view.** Regenerate a variant and
  it updates in place.
- **A file that stops reading as a review makes the app wait.** This happens,
  for example, when the app catches a slow writer halfway through. If you quit
  before the file reads again, the app writes its copy and keeps the
  unreadable one beside it as `NAME.vreview.unreadable`. If that backup already
  exists or cannot be written, neither file is replaced, the app reports the
  save failure, and exits with a nonzero status. Save failures are never reported
  as successful scripted runs.
- **A scene that does not load shows why in its view.** The other candidates
  still open.

For agents: read the notes in time order, and use `object_name` and `point`
to find exactly what was clicked. Answer in `reply` rather than editing the
reviewer's `text`. When a new take is ready, rewrite the scene file in place,
or add it as another candidate. Run `--screenshot` to check that the review
renders before you hand it over.

## The .vreview format

A `.vreview` file uses the engine's [structured document](documents.md) text
format, with the header `vreview 1.0`:

```text
vreview 1.0
review = 1;
title = "Cage replacement: three designs";
created = "2026-09-28T18:27:30Z";
range = [30, 47];
summary = "C, but calm the lights.";
pick = "C";
verdict = "";
candidates = [
    {
        id = "C";
        label = "Mass-driver catcher";
        scene = "../design_C.out/scene.vscene";
        offset = 0;
        about = "A plated funnel facing the Moon, a warm bay and cargo pod racks.";
        thoughts = "Best silhouette.\nThe rim lights are too busy.";
    },
];
notes = [
    {
        id = 1;
        candidate = "C";
        time = 40.125;
        object = 1002;
        object_name = "GATEWAY / Arabian orbital yard";
        point = [0.0062, 874.34, -1094.66];
        view = [0.372, 0.72];
        text = "Too bright here";
        created = "2026-09-28T18:40:00Z";
        status = "open";
        reply = "";
    },
];
```

**The review**

| Field | Meaning |
| --- | --- |
| `review` | Format version: `1`. |
| `title` | One line, 1 to 256 bytes. The only required field besides `review` and `candidates`. |
| `created` | When the review was made, in UTC (ISO 8601). |
| `range` | `[from, to]` on the review clock. If omitted, the clock covers the longest scene. |
| `summary` | The reviewer's overall thoughts. |
| `pick` | The id of the chosen candidate, or `""`. |
| `verdict` | `""`, `"approved"` or `"needs work"`. The app offers it for single reviews. |
| `candidates` | 1 to 4 candidates. |
| `notes` | Up to 4,096 notes. |

**A candidate**

| Field | Meaning |
| --- | --- |
| `id` | Unique, 1 to 16 bytes, no spaces. The app uses `A` to `D`. |
| `label` | One line: what the candidate is. |
| `scene` | A `.vscene` file, relative to the review file or absolute. |
| `offset` | Seconds added to the review clock for this scene. Default 0. |
| `about` | The author's description. |
| `thoughts` | The reviewer's thoughts on it. |

**A note**

| Field | Meaning |
| --- | --- |
| `id` | Unique, above 0. |
| `candidate` | The id of the candidate it is pinned to. |
| `time` | When it was pinned, on the review clock. |
| `object` | The scene instance clicked, or `0` for the background. |
| `object_name` | That instance's name, as the scene gives it. |
| `point` | Where the click met the object, in scene units. Absent for the background. |
| `view` | Where the click was in the view: `[x, y]` from 0 to 1, from the top left. |
| `text` | The reviewer's note. |
| `created` | When it was pinned, in UTC. |
| `status` | `"open"` or `"resolved"`. |
| `reply` | The author's answer. |

**Rules**

- Text fields hold up to 16 KiB each. They may contain new lines and tabs,
  written as `\n` and `\t` inside the quotes; quotes and backslashes are
  escaped as `\"` and `\\`.
- Titles, labels and object names are single lines.
- Numbers must be finite.

A file that breaks these rules does not open, and the error names the line.
The app never writes such a file. It saves by writing a temporary file beside
the review and renaming it over the old one, so a crash leaves either the old
file or the new one. The file keeps its permissions, and a symbolic link to it
stays a link.

## Scripted runs

These options serve tests and agents:

| Option | Effect |
| --- | --- |
| `--at S` | Start the clock at `S`. |
| `--play` | Start playing once every view shows its first frame. |
| `--select N` | Open note `N` at its time and candidate, as a click on its row would. |
| `--note ID X Y TEXT` | Pin a note on candidate `ID` at the view point `X,Y` (0 to 1, from the top left), as a click there would. |
| `--screenshot NEW.png` | Wait until every view shows the start time and any `--note` is pinned, then save a picture of the whole window and exit. The window stays hidden. |
| `--hidden` | Keep the window off screen. |
| `--frames N` | Quit after `N` frames and print the frame rate and how many frames each view showed. |

With `--hidden` or `--screenshot`, exit code 77 means no OpenGL window could
be made, so a test harness can skip the run. For example:

```sh
./build/vng_review cage.vreview --at 40 --note C 0.37 0.72 "Too bright" --screenshot /tmp/cage.png
```

## Code

- `examples/review/review_file.{hpp,cpp}`, the `vng_review_file` library,
  holds the format and needs no OpenGL. It reads, writes, validates and merges
  reviews, and stamps files to notice changes.
- `examples/review/candidate_view.{hpp,cpp}` runs one candidate's scene. It
  renders offscreen and reads the frames back. Clicks and markers are measured
  against the frame that is on screen: its time, size and camera.
- `examples/review.cpp` is the app: layout, notes, file sync, and reopening
  the review.
- `editor_example::pick` in `examples/editor/selection.hpp` is the editor's
  picking, extended with the point where the view ray meets the object.
- Tests:
  - `vng_review_tests` covers the format, the merge and picking.
  - `vng_review_smoke_test` opens a review in a hidden window and pins a
    scripted note. It is skipped where no OpenGL window can be made.

## Limits

- **Candidates.** At most 4 at once.
- **What a click names.** A click names a whole scene instance, such as the
  whole gateway. The note's `point` says which part of it.
- **Background markers.** A background note has no point, so its marker stays
  where it was clicked on screen rather than following the scene.
- **Loading.** Views render as fast as the scenes allow. Each shows the
  newest finished frame, so a heavy scene can trail the clock by a frame
  while playing. While a changed scene loads, the window pauses; heavy scenes
  take a few seconds.
- **Merging.** The merge works field by field, not line by line: if you and an
  agent edit the same note text at once, yours is kept whole.
- **What it does not do.** No sound, no video export, and nothing is sent
  anywhere: the file is the whole review.
