#pragma once

#include <vng/opengl/backend.hpp>
#include <vng/render/renderer.hpp>

namespace vng::opengl {

// Explicit OpenGL specialization of the neutral ticket/backend contract.
// This alias adds no wrapper, forwarding layer, or runtime representation.
template<class Ticket>
using Renderer = render::Renderer<Ticket, Backend>;

} // namespace vng::opengl
