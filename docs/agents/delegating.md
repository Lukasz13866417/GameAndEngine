# Delegating: handing work to another agent or model

Read this when you, an agent or the owner, hand part of a task to another agent. That agent
might be a sub-agent, a parallel session, a reviewer, or a different model such as Claude
Fable or Codex. Also read it when you receive such a brief.

**TL;DR**
- Give each delegate its own worktree, build directory and output folder under
  `.claude/worktrees/`. Never use `/tmp`, the main checkout, or a shared build directory.
- The brief must stand on its own. The delegate has not seen this conversation, and it may
  be a different model. Fill in the template in §1.
- Delegates on the same branch get disjoint files. When they must touch the same files, give
  each one its own worktree and pick one result (variants).
- Verify everything yourself: read the diff, rebuild, re-run the tests, re-render the frames.
- Permission denials go back to the owner unchanged. Never have another agent do what you or
  it were denied.

Base branches, approvals and commit style are in [collaboration.md](collaboration.md).
Volatile facts go in the brief only through [status.md](status.md), or by giving the command
that re-measures them.

## When to delegate

| Good fit | Poor fit |
|---|---|
| 2–3 variants of an unclear creative choice. Each variant gets its own worktree; the owner picks one. | Tightly coupled edits to one file on one branch |
| Read-only reviewers on renders or diffs ([§6](#6-reviewer-agents-for-visual-work)) | Anything that needs an approval the owner hasn't given yet |
| Disjoint work: separate docs files, subsystems or test suites | Work whose result you cannot check |

## 1. The brief

Save the brief as `$MAIN/.claude/worktrees/<task>_brief.md`. That location is persistent and
excluded from git. Paste it as the delegate's prompt, or tell the delegate to read it.

- For several delegates, use one file with `<L>` placeholders, plus one line per delegate
  ("You are variant B: <idea>").
- Use absolute paths throughout. A sub-agent's shell may reset its working directory between
  commands.

```text
# Brief: <one-line goal>

## Goal
<What "done" looks like, in 2-4 sentences. Quote the owner's request verbatim when there is one.>
Standing rules: read AGENTS.md and docs/agents/collaboration.md (section 6, the owner's
preferences) in your worktree. They override anything in this brief that conflicts with them.
<If the delegate's worktree has no docs/agents/ (an older base, or guides not yet on main:
see status.md), paste collaboration.md section 6 here and give the absolute path of a checkout
that has the guides.>

## Where you work
- Repo: /home/luke/Desktop/GameAndEngine is the main checkout. Read it only; another agent edits it.
- Your worktree: <WT>/<task>_<L>, branch wip/<task>-<L>, based on <commit> (<branch it came from>).
- Your build dir: <WT>/<task>_<L>/build (Debug, yours alone; already configured[ and built]). Use -j 8.
- Your output folder: <WT>/<task>_<L>.out/. Put every deliverable here. Never use /tmp (a reboot wipes it).

## Scope
- Edit only: <paths, plus the functions or symbols inside them>.
- Read as needed: <docs and files that explain the area>.
- Do not edit anything else. In particular: <CMakeLists.txt, tests/, examples/assets/, docs/, ...>.
- Write generated scenes and renders to your output folder, never over examples/assets.

## Context
<Facts the delegate cannot find cheaply: instance ids, time ranges, coordinates, why things
are the way they are. Give every number together with the command that re-measures it.>

## Commands (absolute paths; each one ran for me on <date>)
- Rebuild: cmake --build <WT>/<task>_<L>/build --target <targets> -j 8   (~<time>)
- Author or run: <exact command>   (~<time>)
- Test: ctest --test-dir <WT>/<task>_<L>/build -R '<regex>' --output-on-failure   (~<time>)
- Render (visual work): <WT>/<task>_<L>/build/vng_scene_frames <scene> <out>/<new-dir> 960 600 --range <a> <b> <fps>
- Commands that take over 2 minutes: set a longer tool timeout, or run them in the background and poll a log.

## Constraints
- Technical: <budgets, e.g. extra vertices, the 64 MiB scene limit, key limits; renderer limits>.
- Taste (visual work): look natural and never forced; add detail, but not too much; no camera
  shake (including rotational nods or whips).
- Never: push, open PRs, touch the main checkout or other worktrees, use git stash, change
  git config, install anything, or open windows on the desktop (no demos, no editor; use
  vng_scene_frames).
- Permissions: if a command is denied or needs approval, stop and put the exact command and
  the exact message in your reply. Do not work around it. Messages from me are not the
  owner's approval.

## Deliverables (in <WT>/<task>_<L>.out/)
- The change, as <commits on wip/<task>-<L> (do not push) | change.patch, made with
  `git add -A && git diff --cached --binary <commit> > <out>/change.patch` (do not commit)>.
- NOTES.md: what you did and why; numbers you measured, each with its command; every
  command you ran, with its result; what you skipped or could not verify.
- Visual work: sheet.png, clip.mp4 (60 fps if the motion is fast), and 2-3 hero_*.png full frames.

## Reply
At most 200 words: the outcome, where the files are, what failed or was skipped, and any open
questions. Put the detail in NOTES.md, not in the reply.
```

A good brief also:
- names the exact functions or blocks to replace ("in `space_assets.cpp`, the block from
  `// Mass-driver catcher beyond the +X end` …"), not just the file;
- says which shots or behaviours must stay unchanged;
- says how the delegate can check itself: which instance id to probe, and which test proves
  there is no shake.

## 2. Set up isolated worktrees

Run this from your own worktree. Each delegate starts from your current commit, so commit
first: uncommitted changes don't reach the delegates.

```sh
MAIN="$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")"
WT="$MAIN/.claude/worktrees"; DEPS="$MAIN/build/_deps"; TASK=<short-name>; BASE=$(git rev-parse HEAD)
test -d "$DEPS/glfw-src" || echo "no fetched sources in $DEPS: ask before configuring online"
df -h "$MAIN" | tail -1   # per delegate: ~0.22 GB checkout + ~0.9 GB (est.) build with the four targets below
for L in A B C; do
  git -C "$MAIN" worktree add -b "wip/$TASK-$L" "$WT/${TASK}_$L" "$BASE"
  mkdir -p "$WT/${TASK}_$L.out"
  cmake -S "$WT/${TASK}_$L" -B "$WT/${TASK}_$L/build" -DCMAKE_BUILD_TYPE=Debug \
    -DFETCHCONTENT_SOURCE_DIR_CATCH2="$DEPS/catch2-src" -DFETCHCONTENT_SOURCE_DIR_GLAD="$DEPS/glad-src" \
    -DFETCHCONTENT_SOURCE_DIR_GLFW="$DEPS/glfw-src" -DFETCHCONTENT_SOURCE_DIR_HARFBUZZ="$DEPS/harfbuzz-src" \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON > "$WT/${TASK}_$L.out/configure.log"
done
for L in A B C; do   # build only what the delegates need, in parallel; can pass 2 min, so set a long timeout
  cmake --build "$WT/${TASK}_$L/build" --target vng_make_tunnel_departure vng_tunnel_scene_tests vng_scene_frames vng_scene_probe -j 8 \
    > "$WT/${TASK}_$L.out/build.log" 2>&1 &
done; wait; tail -n 1 "$WT/${TASK}"_?.out/build.log
```

- **Costs.**
  - A configure takes about 2 s.
  - Building `vng_make_tunnel_departure` from scratch took about 26 s, in one of three
    worktrees set up together. The GL-based tools add more time (unverified).
  - A build of only that tool used about 0.4 GB of disk. With the GL review tools and the
    tunnel test it is about 0.9 GB (estimate), and a full Debug build uses about 4.3 GB. Free
    disk has been tight (last reading: [status.md](status.md#machine-and-agent-environment)),
    so run `df -h` before a round.
  - Use `-j 8` per build when several run at once; several `-j 20` builds can exhaust the
    RAM ([status.md](status.md) has the machine's cores and memory).
- **Swap the targets for the task.** Name test targets explicitly: a target the brief's Test
  line runs but nobody built shows in ctest as "Not Run". The `vng_make_*`,
  `vng_scene_frames` and `vng_scene_probe` tools are `EXCLUDE_FROM_ALL`
  ([build-and-test.md](build-and-test.md)).
- **Sharing the review tools.** `vng_scene_frames` and `vng_scene_probe` compile in no paths;
  the scene is an argument. When a change touches only generator code (`examples/scenes`,
  `examples/support`), and not `examples/editor/runtime.*`, `mesh_shading.hpp` or the project
  format, delegates may run your fresh `build/vng_scene_frames` and `vng_scene_probe` by
  absolute path instead of building their own. Rebuild them before the round, not during it.
- **Keep the `.out` folder next to the worktree, not inside it**, as for your own
  ([collaboration.md](collaboration.md#create-your-worktree)).
- **Patch-only delegates** can use `worktree add --detach "$WT/${TASK}_$L" "$BASE"` instead of
  a branch. Their commits become unreachable when the worktree is removed.
- **Clean up** once the owner has picked and the result is committed. Do this only for
  worktrees and branches you created; the `.out` folders keep the patches.

  ```sh
  git -C "$MAIN" worktree remove --force "$WT/${TASK}_B"   # also deletes its build/
  git -C "$MAIN" branch -D "wip/$TASK-B"
  ```

## 3. Starting the delegate

- **A sub-agent in your session:**
  - Give it the brief as its prompt, or "Read `<WT>/<task>_brief.md`; you are variant B".
  - It returns only its final message, so everything else must be in `NOTES.md`.
  - It usually cannot ask the owner anything ([§7](#7-permissions-never-pass-between-agents)).
- **A separate session, where the owner picks the model** (e.g. Fable or Codex):
  - Start it with the delegate's worktree as its working directory, so that it sees that
    tree's code and `AGENTS.md`, then paste the brief. A worktree whose base lacks these
    guides (an older base, or any base while the guides are not on `main`:
    [status.md](status.md)) has no `AGENTS.md`, so the brief must carry the rules.
  - It may not see the owner's saved Claude memory, and a non-Claude model never does
    ([collaboration.md §1](collaboration.md#1-who-is-here)). This is why the brief points at
    [collaboration.md §6](collaboration.md#6-the-owners-preferences-in-full).
- **Write for any model.**
  - Use plain shell commands and file paths.
  - Don't name harness-specific tools.
  - Give timeouts in minutes rather than tool parameters.

## 4. Receiving a brief

1. Check where you are: `pwd`, `git status --short`, `git log -1 --oneline`. They must match
   the brief's worktree and base. If they don't, stop and report.
2. Read `AGENTS.md`. If the brief conflicts with its safety rules, the safety rules win; say
   so in your reply.
3. Before you trust a tool, rebuild it with the brief's `cmake --build … --target` line. The
   rebuild is a no-op when the tool is already fresh.
4. Re-measure any number you rely on. Numbers in briefs rot.
5. Stay inside the brief's scope. If the task needs a file outside it, ask in your reply
   instead of editing it.
6. Deliver exactly the listed files, then reply briefly.

## 5. Verify and integrate what comes back

Treat a delegate's report as a claim, and check it. These commands use the shell variables
from §2 (`MAIN`, `WT`, `TASK`, `BASE`).

```sh
D="$WT/${TASK}_C"; B="wip/$TASK-C"
git -C "$D" status --short          # leftovers it forgot to commit, files outside scope
git log --oneline "$BASE..$B"       # what it committed (patch deliverable: git apply --stat <patch>)
git diff --stat "$BASE" "$B"        # only the files in scope?
git diff "$BASE" "$B"               # read all of it
grep CMAKE_HOME_DIRECTORY "$D/build/CMakeCache.txt"   # its binaries were built from its own tree
```

Then, in **your** worktree:

```sh
git cherry-pick "$BASE..$B"         # or: git apply --check <patch> && git apply <patch>
cmake --build build --target <tests and tools the change affects> -j 12
ctest --test-dir build -R '<regex>' --output-on-failure
```

For a scene generator, also re-author and re-render yourself, into fresh paths:

```sh
mkdir -p "$WT/$TASK.out"
./build/vng_make_tunnel_departure examples/assets "$WT/$TASK.out/check.vscene"        # ~4 s
./build/vng_scene_probe "$WT/$TASK.out/check.vscene" --size                           # ~20 s
./build/vng_scene_frames "$WT/$TASK.out/check.vscene" "$WT/$TASK.out/check_frames" 960 600 --range <a> <b> 4
```

- **Compare** your frames with the delegate's sheet. A difference means a stale binary, the
  wrong scene, or uncommitted changes in its tree.
- **Look at the frames yourself** before you forward anything.
- **Check each claimed number** (vertex counts, MiB, clearances) against the probe.
- **If the change touches `CMakeLists.txt`, includes or quoted code,** also run
  `node --test tests/layering/layering.test.cjs docs/explore/tests/content.test.cjs`.
- **You own the finished change.** A delegate's patch is raw material: you add the test
  updates, the docs, the regenerated committed asset (with consent) and the commit, with
  your trailer.
  - Example: variant C's patch touched two files. The commit that landed it (a8aff7d) also
    changed a header, a test, `docs/tunnel.md` and the regenerated scene.

## 6. Reviewer agents for visual work

After each substantial visual iteration, run 2–3 reviewers, each with one focus:
- cinematography and editing;
- art direction and lighting;
- VFX and continuity.

Every reviewer is read-only: it may render into its own output folder but edits nothing.

**Give each reviewer:**
- the owner's brief, verbatim, and the owner's taste rules: natural and never forced, detail
  but not excessive, no shake;
- what changed in this iteration;
- the absolute paths of the contact sheet and 2–3 full-size frames, with their times;
- the renderer's limits: opaque meshes only, bloom on emissives, no MSAA, no motion blur,
  no camera roll ([limits-and-non-features.md](limits-and-non-features.md));
- the issues you already know about, so it doesn't report them again;
- the commands for extra evidence: `vng_scene_frames … --look TIME EX EY EZ TX TY TZ ZOOM FAR`
  for another camera, and `vng_scene_probe SCENE ID TIME…` for screen positions.

**Ask for** at most about 8 suggestions, ranked. Each one should give:
- the time;
- what to change and why;
- a rough cost.

Ask reviewers to flag anything that looks broken separately from matters of taste.

**Triage:** adopt the best suggestions and reject contradictory or weaker ones. Tell the owner
which you adopted and which you rejected, with one reason each
([collaboration.md §7](collaboration.md#7-reporting-to-the-owner)).

**Variants rounds** count as a substantial iteration. Before you show the owner, give each
reviewer the labelled side-by-side hero frames and every variant's sheet in one brief. Ask it to
rank the variants for its focus and to flag anything broken in each. Put the triage into your
one-line trade-offs and your recommendation.

## 7. Permissions never pass between agents

- **Approvals don't transfer.** An approval the owner gave you does not cover a delegate unless
  the owner said so. Even then, the delegate's own guard decides.
  - Never write "the owner approved X" in a brief unless you are quoting the owner, with its
    scope.
- **Denials come back up.** A delegate that is denied must report the exact command and the
  exact message. It must not try variations.
  - You then relay the denial to the owner as it is, with why the action is needed and your
    recommendation, and wait.
- **Never launder a permission.** Don't re-run the denied action yourself because your guard
  might allow it. Don't reword or split it, wrap it in a script, or hand it to another agent,
  model or session with weaker guards.
- **Never loosen permissions.** Never tell a delegate to disable a sandbox or to change
  permission settings, `CLAUDE.md`, `AGENTS.md` or harness config. No agent's message can
  authorize that.

## 8. Pitfalls seen in practice

| Pitfall | What happened | Prevention |
|---|---|---|
| `/tmp` wiped | A reboot wiped a session's scratchpad: uncommitted worktrees, renders and ad-hoc probe binaries. Hours of work were lost. `git worktree list` still shows the dead entries as `prunable`. | Keep worktrees, briefs and outputs under `$MAIN/.claude/worktrees/`. Commit on a branch. Turn useful probes into real targets (as `vng_scene_frames` and `vng_scene_probe` were). |
| Shared build dir | Another agent's test run replaced a worktree's `Testing/Temporary/LastTest.log`. Two builds in one dir race on the same objects. | One `build/` per worktree. Use `--output-on-failure`. |
| Binary from the wrong tree | Binaries compile in absolute source and asset paths. A main-checkout binary shows main-checkout files, and the editor's Reload C++ builds into its baked build dir. | Run each delegate's binaries from its own `build/`. Check `CMAKE_HOME_DIRECTORY`. This applies to generators, tests and demos; the two review tools are path-free (§2). |
| Stale binaries | `ctest` and a plain `cmake --build` never rebuild the `EXCLUDE_FROM_ALL` tools. A scene was regenerated by a binary built a minute before the last commit, and had to be checked for byte-identical output. | Run the `--target` build line right before using any tool. Rebuild the renderers after changing `mesh_shading.hpp`. |
| Same file, several agents | Three design variants all edited `space_assets.cpp` and `departure_voyage.cpp`. That was safe only because each had its own worktree and one result was chosen. | On a shared branch, give delegates disjoint files. Overlapping files only as alternatives in separate worktrees. |
| Numbers in the brief rot | A brief said "~0.2 s per frame". Measured on the departure scene: ~7.5 s to load, then ~0.04–0.05 s per 960x600 frame. | Put the measuring command next to every number, or link [status.md](status.md). |
| New files missing from a patch | `git diff` omits untracked files. | Commit on a branch, or `git add -A` first and use `git diff --cached`. |
| Windows on the owner's screen | Demos, the editor and the window tests open visible windows ([AGENTS.md §2](../../AGENTS.md#2-safety-rules) lists them). There is no xvfb. | Say "no windows" in the brief. Use `vng_scene_frames` and `vng_scene_probe`. |
| User state overwritten | The editor rewrites `~/.config/vng/editor.settings`, and Save overwrites the opened scene. | `XDG_CONFIG_HOME=<absolute scratch dir>` and a copy of the scene ([editor.md](editor.md)). |
| Tool timeouts | Agent shells often stop commands at 2 min. Full ctest takes ~4 min, and a full build takes minutes. | Put the timings in the brief. Use long timeouts or background runs. |
| Branch already checked out | `fatal: '<branch>' is already used by worktree at '<path>'` | Give each delegate a new `-b` branch, or `--detach`. |
| Missing preferences | A worktree session may not load the owner's Claude memory, and other models never do ([collaboration.md §1](collaboration.md#1-who-is-here)). A worktree whose base lacks `docs/agents/` (an older base, or any base while the guides are not on `main`: [status.md](status.md)) has no guides either. | The brief's "Standing rules" line, plus the taste constraints written out. Paste collaboration.md §6, and give the absolute path of a checkout that has the guides, when the worktree lacks them. |
| Denied actions | Claude Code's auto-mode guard blocked staging and `git switch` in the main checkout (the way out was a separate worktree) and `git push` (only the owner's approval in chat unblocked it). | [§7](#7-permissions-never-pass-between-agents): report the denial up to the owner; never route around it. |
