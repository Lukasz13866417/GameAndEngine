#pragma once

namespace vng::opengl {

class Device;
class Frame;

// Identity only, not a runtime device/context or a service registry.
struct Backend final {
    using device_type = Device;
    using frame_type = Frame;
};

} // namespace vng::opengl
