#pragma once

namespace vng::render {

// Encoding contract for a color render target. Backends may realize this as
// fixed-function conversion (OpenGL), an attachment/view format
// (Vulkan/D3D), or a validation requirement.
enum class ColorEncoding {
    linear,
    srgb,
};

} // namespace vng::render
