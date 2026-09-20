#pragma once

#include <cstddef>
#include <span>
#include <vng/gfx/record.hpp>

namespace vng::gfx {

// Ownership is selected by the device; CPU data is consumed only for upload.
struct MakeBuffer {
    template<class Device>
    auto operator()(Device& device, std::span<const std::byte> bytes) const
        -> decltype(make_backend_buffer(device, bytes))
    { return make_backend_buffer(device, bytes); }
};
inline constexpr MakeBuffer make_buffer{};

// Explicitly upload a typed per-instance stream. The context chooses its
// backend realization; no VAO, locations or erased stream descriptors leak out.
struct MakeInstanceBuffer {
    template<class Device, RecordType Record>
    auto operator()(Device& device, std::span<const Record> records) const
        -> decltype(make_backend_instance_buffer(device,records))
    { return make_backend_instance_buffer(device,records); }
};
inline constexpr MakeInstanceBuffer make_instance_buffer{};

} // namespace vng::gfx
