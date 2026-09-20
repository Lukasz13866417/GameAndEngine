#pragma once
#include "../editor/runtime.hpp"
#include <span>

namespace example::fleet {
struct EvidenceSubject { vng::u32 id; const char* filename; };
[[nodiscard]] vng::resources::Result<void> inspect(
    vng::opengl::Device&, editor_example::Runtime&, editor_example::State&,
    const vng::gfx::Camera&, vng::Extent2D, vng::f32 time, const std::filesystem::path&,
    std::span<const EvidenceSubject>);
} // namespace example::fleet
