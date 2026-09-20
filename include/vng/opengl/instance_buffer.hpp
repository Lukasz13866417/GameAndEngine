#pragma once
#include <vng/gfx/record.hpp>
#include <vng/opengl/buffer.hpp>
#include <vng/opengl/device.hpp>
#include <algorithm>
#include <limits>
#include <optional>
#include <utility>

namespace vng::opengl {
// Renderer-owned per-instance records, independent of the blueprint geometry.
// Capacity grows geometrically; updating an empty batch issues no GL upload.
template<gfx::RecordType Record>
class InstanceBuffer {
public:
    using record_type = Record;
    InstanceBuffer() = default;
    InstanceBuffer(InstanceBuffer&& other) noexcept
        : buffer_(std::move(other.buffer_)), size_(std::exchange(other.size_,0)) {}
    InstanceBuffer& operator=(InstanceBuffer&& other) noexcept {
        if(this!=&other) {buffer_=std::move(other.buffer_);size_=std::exchange(other.size_,0);}return *this;
    }
    InstanceBuffer(const InstanceBuffer&) = delete;
    InstanceBuffer& operator=(const InstanceBuffer&) = delete;

    [[nodiscard]] std::expected<void,Diagnostic> update(const Device& device, std::span<const Record> records) {
        if(buffer_ && buffer_->native_handle() && !buffer_->belongs_to(device))
            return std::unexpected(Diagnostic{.code=ErrorCode::incompatible_device,
                .message="InstanceBuffer update requires its creating device"});
        constexpr auto maximum=std::min<std::size_t>(std::numeric_limits<i32>::max(),
            std::numeric_limits<std::ptrdiff_t>::max()/Record::stride);
        if(records.size()>maximum)
            return std::unexpected(Diagnostic{.code=ErrorCode::invalid_argument,.message="Instance count exceeds backend limits"});
        if(records.empty()) {size_=0;return {};}
        if(!buffer_ || buffer_->size()<records.size_bytes()) {
            const auto previous=buffer_?buffer_->size()/Record::stride:0;
            const auto capacity=std::max(records.size(),std::min(maximum,std::max<std::size_t>(16,previous*2)));
            auto next=Buffer::create(device,{.size=capacity*Record::stride,.storage=BufferStorage::dynamic});
            if(!next)return std::unexpected(next.error());
            if(auto written=next->write(0,std::as_bytes(records));!written)return written;
            buffer_=std::move(*next);
        } else if(auto written=buffer_->write(0,std::as_bytes(records));!written)return written;
        size_=static_cast<u32>(records.size());return {};
    }
    [[nodiscard]] u32 size() const noexcept {return size_;}
    [[nodiscard]] const Buffer* storage() const noexcept {return buffer_?&*buffer_:nullptr;}
private:
    std::optional<Buffer> buffer_;
    u32 size_{};
};

template<gfx::RecordType Record>
[[nodiscard]] std::expected<InstanceBuffer<Record>,Diagnostic> make_backend_instance_buffer(
    const Device& device,std::span<const Record> records) {
    InstanceBuffer<Record> buffer;
    if(auto r=buffer.update(device,records);!r)return std::unexpected(r.error());
    return buffer;
}
} // namespace vng::opengl
