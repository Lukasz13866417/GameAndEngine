#pragma once
#include "character.hpp"

namespace character {
// What a character stands on when shown alone: a checkerboard floor of
// one-metre tiles under a plain sky. The floor fades into the sky colour
// beyond 10 m, so it has no edge, and is bound to a one-bone armature so the
// character's skinned renderer can draw it too.
struct Stage {
    static constexpr vng::f32 tile = 1.F;
    static constexpr vng::Vec4 sky{.33F, .40F, .48F, 1};

    [[nodiscard]] static vng::content::Result<Stage> create();
    // Treadmill: how far the floor has moved back under a clip walking for `seconds`.
    [[nodiscard]] static vng::f32 scroll(vng::f32 speed, vng::f32 seconds);

    vng::rig::Armature armature;
    vng::rig::SkinBinding<Mesh> floor;
};
} // namespace character
