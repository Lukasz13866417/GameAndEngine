#pragma once
#include <vng/input/input.hpp>

namespace editor_example {
// Keep a minimized window's layout stable while another editor window remains
// usable. This is layout metadata only, never a claim that a drawable exists.
struct UiSurfaceSize {
    vng::Vec2 logical{1,1};
    vng::Extent2D pixels{1,1};
    void retain(vng::input::Frame& frame) {
        if (!frame.framebuffer.empty() && frame.logical_size.x>0 && frame.logical_size.y>0) {
            logical=frame.logical_size; pixels=frame.framebuffer;
        } else {
            frame.logical_size=logical; frame.framebuffer=pixels; frame.focused=false;
        }
    }
};
// The editor uses a compact desktop-tool density, independently of the user's
// accessibility scale. 18-unit text becomes ~13 logical pixels and a 36-unit
// control becomes ~26px at 100%. Do not shrink the render target or raster bitmap.
inline constexpr float editor_ui_density = .72F;
// Keep the framebuffer native-sized. Existing DPI-aware UI rendering handles
// glyph rasterization and widget geometry using these scaled logical units.
inline void scale_ui_input(vng::input::Frame& frame, unsigned percent) {
    const auto scale=100.F/(static_cast<float>(percent)*editor_ui_density);
    frame.logical_size={frame.logical_size.x*scale,frame.logical_size.y*scale};
    frame.pointer={frame.pointer.x*scale,frame.pointer.y*scale};
    for(auto& event:frame.events)
        event.position={event.position.x*scale,event.position.y*scale};
}
}
