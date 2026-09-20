#include <vng/opengl/readback.hpp>
#include <vng/opengl/device.hpp>
#include <vng/opengl/framebuffer.hpp>
#include "context_state.hpp"
#include "gl_error.hpp"
#include "readback_state.hpp"
#include <array>
#include <cstring>
#include <utility>

namespace vng::opengl {
namespace {
auto invalid(std::string message) {
    return std::unexpected(Diagnostic{.code=ErrorCode::invalid_argument,.message=std::move(message)});
}
}
struct Rgba8ReadbackQueue::Impl {
    struct Slot {
        GLuint buffer{};
        const std::byte* mapped{};
        std::size_t bytes{};
        GLsync fence{};
        Extent2D extent{};
        u64 id{};
    };
    std::shared_ptr<detail::ContextState> state;
    std::array<Slot, capacity> slots{};
    u64 last_id{};
    ~Impl() {
        if (!state->is_current()) {
            state->record_lifecycle_failure("Rgba8ReadbackQueue destroyed without its current context");
            return;
        }
        for (auto& slot : slots) {
            if (slot.fence) glDeleteSync(slot.fence);
            if (slot.buffer) glDeleteBuffers(1, &slot.buffer);
        }
    }
    std::expected<void, Diagnostic> current() const {
        if (auto r=state->require_current("Rgba8ReadbackQueue");!r) return r;
        if (state->active_frame_generation.load())
            return invalid("End the active frame before transferring preview pixels");
        return {};
    }
};
Rgba8ReadbackQueue::Rgba8ReadbackQueue(std::unique_ptr<Impl> p):impl_(std::move(p)) {}
Rgba8ReadbackQueue::Rgba8ReadbackQueue(Rgba8ReadbackQueue&&) noexcept=default;
Rgba8ReadbackQueue& Rgba8ReadbackQueue::operator=(Rgba8ReadbackQueue&&) noexcept=default;
Rgba8ReadbackQueue::~Rgba8ReadbackQueue()=default;
std::expected<Rgba8ReadbackQueue, Diagnostic> Rgba8ReadbackQueue::create(const Device& device) {
    if (!device.state_) return invalid("Readback queue requires a device");
    if (auto r=device.require_resource_update("Create readback queue");!r) return std::unexpected(r.error());
    auto p=std::make_unique<Impl>();p->state=device.state_;
    return Rgba8ReadbackQueue{std::move(p)};
}
bool Rgba8ReadbackQueue::available() const noexcept {
    if (impl_) for (const auto& slot:impl_->slots) if (!slot.fence) return true;
    return false;
}
bool Rgba8ReadbackQueue::pending() const noexcept {
    if (impl_) for (const auto& slot:impl_->slots) if (slot.fence) return true;
    return false;
}
void Rgba8ReadbackQueue::discard() noexcept {
    if (impl_) for (auto& slot:impl_->slots) slot.id=0;
}
std::expected<bool,Diagnostic> Rgba8ReadbackQueue::try_submit(
    const Framebuffer& source,u32 attachment,Extent2D extent,u64 id) {
    if (!impl_) return invalid("Moved-from readback queue");
    auto& p=*impl_;
    if (auto r=p.current();!r) return std::unexpected(r.error());
    if (p.state!=source.state_) return std::unexpected(detail::incompatible_device("Readback framebuffer"));
    const auto format=source.color_attachment_formats_.find(attachment);
    if (!source.handle_ || format==source.color_attachment_formats_.end() ||
        (format->second!=ImageFormat::rgba8 && format->second!=ImageFormat::srgb8_alpha8) ||
        source.color_attachment_samples_.at(attachment)!=0)
        return invalid("Readback requires a single-sample RGBA8 or sRGB8-alpha8 attachment");
    const auto bounds=source.extent();
    if (extent.empty() || extent.width>bounds.width || extent.height>bounds.height ||
        extent.width>8192 || extent.height>8192 || u64(extent.width)*extent.height>16*1024*1024)
        return invalid("Invalid readback extent");
    if (!id || id<=p.last_id) return invalid("Readback IDs must increase");
    Impl::Slot* free{};
    for(auto& slot:p.slots) if(!slot.fence) {free=&slot;break;}
    if(!free) return false;
    auto& slot=*free;
    const auto bytes=std::size_t(extent.width)*extent.height*4;
    if(slot.bytes<bytes) {
        GLuint buffer{};const void* mapped{};
        constexpr auto flags=GL_MAP_READ_BIT|GL_MAP_PERSISTENT_BIT|GL_MAP_COHERENT_BIT;
        auto allocated=detail::checked_gl_call("Allocate asynchronous readback storage",[&] {
            glCreateBuffers(1,&buffer);
            glNamedBufferStorage(buffer,static_cast<GLsizeiptr>(bytes),nullptr,flags);
            mapped=glMapNamedBufferRange(buffer,0,static_cast<GLsizeiptr>(bytes),flags);
        });
        if(!allocated || !mapped) {
            if(buffer) glDeleteBuffers(1,&buffer);
            if(!allocated) return std::unexpected(allocated.error());
            return invalid("Could not map readback storage");
        }
        if(slot.buffer) glDeleteBuffers(1,&slot.buffer);
        slot.buffer=buffer;slot.bytes=bytes;slot.mapped=static_cast<const std::byte*>(mapped);
    }
    detail::ReadbackState restore(source.handle_);
    auto submitted=detail::checked_gl_call("Queue asynchronous RGBA8 readback",[&] {
        glBindBuffer(GL_PIXEL_PACK_BUFFER,slot.buffer);
        glReadBuffer(GL_COLOR_ATTACHMENT0+attachment);
        glReadPixels(0,0,static_cast<GLsizei>(extent.width),static_cast<GLsizei>(extent.height),
                     GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        slot.fence=glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE,0);
        glFlush(); // Hidden contexts do not swap; ensure the fence can progress.
    });
    if(!submitted || !slot.fence) {
        // Do not reuse storage whose transfer may have been submitted without a fence.
        if(slot.fence) glDeleteSync(slot.fence);
        glDeleteBuffers(1,&slot.buffer);slot={};
        if(!submitted) return std::unexpected(submitted.error());
        return invalid("Could not fence readback transfer");
    }
    slot.extent=extent;slot.id=id;p.last_id=id;
    return true;
}
std::expected<std::optional<Rgba8Readback>,Diagnostic> Rgba8ReadbackQueue::try_take() {
    if(!impl_) return invalid("Moved-from readback queue");
    auto& p=*impl_;
    if(auto r=p.current();!r) return std::unexpected(r.error());
    Impl::Slot* latest{};
    for(auto& slot:p.slots) {
        if(!slot.fence) continue;
        GLenum status{};
        if(auto r=detail::checked_gl_call("Poll readback fence",[&] {
            status=glClientWaitSync(slot.fence,0,0);
        });!r) return std::unexpected(r.error());
        if(status==GL_WAIT_FAILED) return invalid("Readback fence wait failed");
        if(status==GL_TIMEOUT_EXPIRED) continue;
        glDeleteSync(slot.fence);slot.fence=nullptr;
        if(slot.id && (!latest || slot.id>latest->id)) latest=&slot;
    }
    if(!latest) return std::optional<Rgba8Readback>{};
    const auto row=std::size_t(latest->extent.width)*4;
    Rgba8Readback value{latest->id,{latest->extent,std::vector<std::byte>(row*latest->extent.height)}};
    for(u32 y=0;y<latest->extent.height;++y)
        std::memcpy(value.image.pixels.data()+std::size_t(y)*row,
                    latest->mapped+std::size_t(latest->extent.height-1-y)*row,row);
    for(auto& slot:p.slots) if(!slot.fence) slot.id=0;
    return std::optional<Rgba8Readback>{std::move(value)};
}
} // namespace vng::opengl
