# Window systems and graphics contexts

Window-system selection and graphics-backend selection are separate concerns.
The engine keeps three layers so neither backend leaks into the other:

1. `window::WindowDesc` describes only a native window: size, title, visibility,
   and resize behavior.
2. `opengl::ContextDesc` describes the requested OpenGL context: version,
   debugging, forward compatibility, default-surface samples and sRGB
   capability, and swap interval.
3. `glfw_opengl` is the small integration layer that translates those two
   descriptions into GLFW calls and supplies an OpenGL context-access token.

`vng_window_glfw` itself does not include or link OpenGL. Likewise,
`vng_opengl` does not include or link GLFW. Applications that select this pair
link the distinct `vng::glfw_opengl` integration target.

## Application-facing shape

Backend selection appears once, at creation:

```cpp
#include <vng/window/glfw_opengl.hpp>

auto created = vng::glfw_opengl::create_window(
    vng::window::WindowDesc{
        .width = 1280,
        .height = 720,
        .title = "Game",
    },
    vng::opengl::ContextDesc{
        .debug = true,
        .samples = 4,
        .default_framebuffer_encoding =
            vng::render::ColorEncoding::srgb,
        .swap_interval = 1,
    });

if (!created) {
    // created.error() is a window::Diagnostic
}
auto window = std::move(*created);

auto context = window.make_current();
auto device = vng::opengl::Device::create(*context);
```

After creation, ordinary window operations do not repeat a GLFW class name:

```cpp
while (!window.should_close()) {
    window.poll_events();

    // render...

    if (auto presented = window.present(); !presented) {
        // handle presentation failure
    }
}
```

GLFW has one process-wide event queue, so one `poll_events()` call per frame is
enough even when the application owns multiple GLFW windows.

`present()` intentionally avoids the GLFW-specific term “swap buffers.” It is
the single presentation boundary and can later move onto an acquired `Frame`
without changing `window::WindowDesc` or the platform-window interface.

`ContextDesc::default_framebuffer_encoding` is a creation request and defaults
to `ColorEncoding::srgb`. The GLFW integration translates it to
`GLFW_SRGB_CAPABLE`. After OpenGL procedures are loaded, `Device` queries the
actual default color attachment with
`GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING`; it does not trust the creation
hint. The integration also puts the requested encoding in
`CurrentContextAccess::required_default_framebuffer_encoding`, so
`Device::create` fails with `unsupported_feature` when the native window did
not provision what was requested. A successful default integration therefore
cannot produce a device whose default `FrameDesc` is incompatible.

Physical attachment encoding and framebuffer-sRGB conversion support are
reported separately:

```cpp
const auto capabilities = device.default_framebuffer_capabilities();
if (capabilities.color_encoding == vng::render::ColorEncoding::srgb
    && capabilities.srgb_conversion_supported) {
    // The default FrameDesc is supported by this presentation surface.
}
```

`begin_frame` requires `FrameDesc::color_encoding` to match that physical
encoding exactly. Disabling `GL_FRAMEBUFFER_SRGB` does not make an sRGB
attachment a linear target, and enabling it does not make a linear attachment
sRGB. An unavailable/attachment-less default framebuffer is reported with an
empty `color_encoding` and cannot begin a default-target frame.

Non-GLFW providers do not have to duplicate the query. A provider that promises
a creation result can set the optional
`required_default_framebuffer_encoding`; one that makes no promise leaves it
empty. In both cases `Device` examines whichever default framebuffer is
current. Surfaceless providers therefore remain usable for explicit off-screen
targets even though their default-target capability is unknown.

The runnable examples explicitly request a linear default framebuffer and pair
it with linear pipeline/frame descriptions. This keeps smoke runs useful on
headless/Xvfb configurations that accept an sRGB hint but only offer a linear
visual. Production code can omit those three explicit choices to use the
modern, strict sRGB defaults; on a system that cannot provide them,
`Device::create` reports the mismatch immediately.

## Non-OpenGL GLFW windows

`window::GlfwWindow::create(WindowDesc)` creates a native window with
`GLFW_NO_API`. This is the appropriate lower layer for another explicit API
integration; it has no context, `make_current`, or presentation operations.

## Lifetime and threading

GLFW windows are thread-affine. Create, process events, and destroy them on the
thread that initialized GLFW's event queue. A moved-from window is invalid:
`valid()` is false, `should_close()` is true, and its event methods do not pump
the process-wide queue.

Destroying a still-live GLFW window on a different thread terminates the
process. GLFW cannot legally destroy it there and a destructor cannot report an
error; terminating exposes the programming error instead of silently leaking a
native window and context. Move ownership back to the owning thread before the
last owner is destroyed.

The integration `Window` owns the native OpenGL context, so it must outlive all
`Device` facades and resources made from them. Destroy OpenGL resources while
that window's context is current; otherwise the backend records a lifecycle
diagnostic and deliberately avoids an unsafe driver call.
