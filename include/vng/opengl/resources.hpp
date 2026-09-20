#pragma once

#include <vng/gfx/buffer.hpp>
#include <vng/opengl/buffer.hpp>
#include <vng/opengl/device.hpp>
#include <vng/resources/resources.hpp>

namespace vng::opengl {
inline auto make_backend_buffer(Device& device, std::span<const std::byte> bytes)
{ return Buffer::from_bytes(device, bytes); }
} // namespace vng::opengl
