#include <vng/render/renderer.hpp>

struct Draw {};
// The neutral contract must never silently imply a portable backend.
class MissingBackend : public vng::render::Renderer<Draw> {};
