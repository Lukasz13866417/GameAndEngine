# Timelines and scene animation

The timeline is CPU data, independent of the UI, shader DSL, window system, and
graphics backend. It stores typed values addressed by an object ID and a property
name. Sampling returns an override; it does not call an effect or mutate an object.

The editor's scene bridge maps those overrides to its mesh and sun settings.
The same evaluated settings can then be passed to the normal runtime and diagnostic
rendering paths. Keying a value does not rebuild a shader or upload a mesh.

## Neutral API

```cpp
#include <vng/timeline/timeline.hpp>

using namespace vng;
timeline::Timeline animation;
const timeline::Target position{1, "position"};

auto first = animation.set(position,
    {0.0F, Vec3{0, 0, 0}, timeline::Interpolation::hold}, "Position", "Ship");
auto second = animation.set(position,
    {4.0F, Vec3{8, 0, 0}, timeline::Interpolation::linear});
// Check first/second before using the authored animation.

auto value = animation.sample(position, 2.0F); // Vec3{4, 0, 0}
```

`Target::object` is a nonzero application identity, not a pointer or GPU handle.
Property strings form the application's stable animation schema. The core does
not know whether `"position"` addresses a mesh, an effect, or another kind of object.

`Value` supports `bool`, `i32`, `u32`, `f32`, `Vec3`, and `std::string`. Each track
retains one value type. Linear interpolation is available only for `f32` and
`Vec3`; discrete values use `hold`. Invalid keys return `std::expected` diagnostics.

The API also provides:

- `find(target)` and `tracks()` for read-only inspection;
- `set(target, key)` to insert or replace a key at the same exact time;
- `erase(target, time)` to remove a key, removing its track when empty;
- `move(target, from, to)` to move a key without changing its value or incoming mode;
- `replace(tracks)` to validate and atomically replace a complete track collection.

A move onto an existing key fails instead of silently overwriting it. `replace`
rejects duplicate targets and duplicate key times, then sorts tracks by
`(object, property)` and keys by time. Empty labels/layers passed to `set` preserve
existing metadata. Labels and layers are organizational only: hiding a layer in
the editor does not disable animation or change its evaluation order.

## Incoming interpolation

The destination key owns the interpolation from its predecessor. For example:

| Key time | Value | Incoming | Meaning |
| --- | --- | --- | --- |
| 0 | 1 | hold | Start at 1. |
| 2 | 3 | hold | Keep 1 until time 2, then switch to 3. |
| 4 | 5 | linear | Interpolate 3 to 5 between times 2 and 4. |

At time 3 the value is 4. Exact key times always return that key's own value.
Before the first key, `sample` returns `nullopt`, letting the application use its
authored fallback. After the final key, its value is held. Invalid query times
also return `nullopt`.

## Editor scene bridge

The example keeps authored values and animation separate:

```cpp
#include "editor/animation.hpp"

// A checked, transactional key edit; the caller handles undo and revision.
auto keyed = editor_example::key_property(
    state, {1, "position"}, 5.0F, vng::Vec3{3, 1, 0},
    vng::timeline::Interpolation::linear);

const auto evaluated = editor_example::evaluate_scene(state, 2.5F);
// evaluated.model / evaluated.sun are small settings values.
// state.model / state.sun / state.mesh remain unchanged.
```

`animation_properties(state)` returns the keyable targets, labels, layers,
authored fallback values, and editor ranges. Current targets are:

| Object | Properties |
| --- | --- |
| Mesh (`1`) | `position`, `scale`, `brightness`, `visible`, `wireframe` |
| Sun (`2`) | `position`, `radius`, `displacement`, `bloom`, `white_spots`, `visible` |

When the first key is added after time zero, `key_property` also inserts the
authored baseline at zero. Later edits retain that baseline. Adding a key directly
at zero creates only that key. Boolean keys are automatically stored as `hold`.
The bridge preserves existing labels/layers and checks all changes before
committing them. It does not change the revision or create an undo entry itself.

`validate_animation` rejects unknown scene targets, wrong value types, values
outside the editor's property limits, and keys beyond `state.timeline_duration`.
Duration is restricted to `[0.1, 86400]` seconds. Sun displacement is `[0, 1]`,
matching the renderer, rather than permitting values that fail at draw time.

Authored fallback edits are not implicit key edits. If a track overrides a
property at the current time, edit that property's key to change its animation.
Camera tracks, character animation, easing curves, track blending and animation
layers that modify evaluation are not part of this slice.

### Scene keyframes

The editor presents a scene keyframe as one timestamp containing property keys
from any number of objects. This is a view over the existing independent tracks,
not a replacement sampling engine. Old sparse tracks still work: the inspector
shows keyed values and evaluated, unkeyed context together.

`editor/keyframes.hpp` provides checked, transactional helpers:

- `add_keyframe(state, time)` captures all current scene properties; an existing timestamp is unchanged.
- `keyframe_times(state)` lists all timestamps and `keyframe_values(state, time)` returns their typed values, incoming modes and keyed flags.
- `update_keyframe(state, from, to, name, values)` edits/renames/retimes the group atomically.
- `erase_keyframe(state, time)` deletes every property key and name at that timestamp.

Unchecking a property's Key switch removes only that property's key. An entry in
`State::keyframe_names` retains intentionally empty groups. Names are optional,
limited to 256 UTF-8 bytes with no control characters, and unrelated to object
identities. These helpers leave undo entries, revisions and transport to the app.

## Persistence

Save and Save As write the authored settings and the complete timeline into the
same `.vscene` document. The scene's active path is managed separately from its
editable filename field. Regular Save targets that active path; Save As switches
it only after successful publication. Failed writes retain the old target and
authored state.

The timeline section is readable and deterministic:

```text
timeline = {
    duration = 10;
    tracks = [
        { object = 1; property = "position"; label = "Position"; layer = "Mesh"; keys = [
            { time = 0; value = [1.7,0,0]; incoming = "hold"; },
            { time = 5; value = [3,1,0]; incoming = "linear"; },
        ]; },
    ];
    keyframes = [
        { time = 0; name = "Start"; },
        { time = 5; name = "Arrival"; },
    ];
};
```

Values are decoded using the target property's type. Malformed or unknown targets,
out-of-range values, duplicate tracks/keys, and invalid interpolation modes are
rejected rather than ignored. Core storage and document byte/key limits also apply.

Older scenes without this section still load with no animation. Their duration is
`max(10, saved_time)`, keeping an old saved playhead visible on the new ruler.
The optional `keyframes` metadata list supplies names and empty groups. Older
timelines without it derive unnamed groups from their property-key timestamps.
Duplicate metadata timestamps, invalid names and out-of-duration groups are rejected.

## Dependency direction and inspection

```text
vng_core
  └─ vng_timeline (values, tracks, validation, sampling)
      └─ vng_editor_project (scene property bridge and .vscene persistence)
          ├─ editor UI (authoring intents, undo, timeline display)
          └─ worker runtime (evaluate settings → existing rendering)
```

The project library also uses the existing content/editor CPU libraries. Neither
the neutral timeline nor the scene bridge depends on OpenGL or a UI widget.
`Timeline::tracks()`, `animation_properties`, `evaluate_scene`, and the saved
document expose each transition without needing a live graphics context.

CPU tests cover interpolation, discrete switches, deterministic serialization,
legacy documents, range rejection, baseline seeding, immutable authored data,
and animated CPU projection. See [the editor guide](editor.md) for running the
editor and its independent worker window.
