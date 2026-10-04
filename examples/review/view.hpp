#pragma once
#include <vng/gfx/image.hpp>
#include <vng/opengl/device.hpp>
#include <vng/resources/diagnostic.hpp>
#include <memory>
#include <optional>
#include <string>

namespace review {
// What a click met in a view: the object (0 for the background), its name,
// and the point where the click met it, in the view's own coordinates (scene
// units for a scene; the bind pose for a character, so it moves with him).
struct Hit {
    vng::u32 object{};
    std::string name;
    vng::Vec3 point{};
};
// One candidate's view: renders its subject at a time into an image, and says
// what lies under a point of the image it last showed.
class View {
public:
    virtual ~View() = default;
    [[nodiscard]] virtual vng::f32 duration() const = 0;
    // Takes a finished frame if there is one, then renders `time` at `extent`
    // unless that image is already shown or a frame is still in flight.
    virtual vng::resources::Result<void> update(vng::opengl::Device&, vng::f32 time, vng::Extent2D) = 0;
    [[nodiscard]] virtual const std::shared_ptr<const vng::gfx::ImageData>& image() const = 0;
    [[nodiscard]] virtual vng::u64 revision() const = 0;
    [[nodiscard]] virtual std::optional<vng::f32> shown_time() const = 0;
    // What lies under a normalized top-left point of the shown image.
    [[nodiscard]] virtual std::optional<Hit> pick(vng::Vec2 normalized) = 0;
    // Where a point (as a Hit gives it) appears in the shown image, normalized,
    // when it is in front of the camera.
    [[nodiscard]] virtual std::optional<vng::Vec2> project(vng::Vec3) const = 0;
};
} // namespace review
