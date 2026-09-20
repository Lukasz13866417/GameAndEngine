#pragma once

#include <concepts>
#include <memory>
#include <span>
#include <type_traits>

#include <vng/render/view.hpp>
#include <vng/render/frame.hpp>

namespace vng::render {

// A renderer owns a policy for drawing tickets; this base only gives that
// policy a common, compile-time identity. It intentionally contains no state,
// virtual functions, backend handle, or forwarding implementation.
//
// Concrete renderers derive publicly and provide their own render() function:
//
//   template<Backend B>
//   class ModelRenderer : public Renderer<ModelDraw, B> {
//   public:
//       Result render(typename B::frame_type&, const RenderView&, span<const ModelDraw>);
//   };
//
// A concrete implementation may instead use its backend's short alias, e.g.
// opengl::Renderer<ModelDraw>. There is no default "portable" backend: a
// backend-neutral algorithm is a template, its realized resources are not.
// Keeping the constructor and destructor protected prevents Renderer<Ticket, B>
// itself from becoming the accidental "plain renderer" abstraction.
template<class Ticket, Backend B>
class Renderer {
public:
    using ticket_type = Ticket;
    using backend_type = B;

    Renderer(const Renderer&) noexcept = default;
    Renderer& operator=(const Renderer&) noexcept = default;
    Renderer(Renderer&&) noexcept = default;
    Renderer& operator=(Renderer&&) noexcept = default;

protected:
    constexpr Renderer() noexcept = default;
    ~Renderer() = default;
};

namespace detail {

template<class Candidate>
using renderer_type_t = std::remove_cvref_t<Candidate>;

template<class Candidate>
using renderer_ticket_t = typename renderer_type_t<Candidate>::ticket_type;

} // namespace detail

// Identifies an actual renderer policy, rather than anything that happens to
// have a similarly named function. Public inheritance is intentional: it
// makes the ticket/backend contract inspectable by generic render orchestration while
// adding no runtime representation.
template<class Candidate>
concept RendererType = BackendBound<Candidate> && requires {
    typename detail::renderer_type_t<Candidate>::ticket_type;
} && std::derived_from<
    detail::renderer_type_t<Candidate>,
    Renderer<detail::renderer_ticket_t<Candidate>, backend_t<Candidate>>
> && (!std::same_as<
    detail::renderer_type_t<Candidate>,
    Renderer<detail::renderer_ticket_t<Candidate>, backend_t<Candidate>>
>);

template<RendererType Candidate>
using renderer_ticket_t = detail::renderer_ticket_t<Candidate>;

// Both backend identity and the callable batch entry point must agree. In
// particular, an unconstrained render() template cannot silently opt an
// OpenGL renderer into another backend. Frame variants of the same backend
// are allowed. The result/diagnostic type remains the renderer's choice.
template<class Candidate, class FrameType>
concept RendererFor = RendererType<Candidate> && Frame<FrameType>
    && SameBackend<Candidate, FrameType> && requires(
    Candidate& renderer,
    FrameType& frame,
    const RenderView& view,
    std::span<const detail::renderer_ticket_t<Candidate>> tickets) {
    renderer.render(frame, view, tickets);
};

// Convenience for immediate one-off draws. The renderer itself still has one
// canonical batch API, so it does not need to repeat a single-ticket overload
// or inherit through CRTP merely for span construction.
template<class Candidate, class Frame>
    requires RendererFor<Candidate, Frame>
[[nodiscard]] decltype(auto) render_one(
    Candidate& renderer,
    Frame& frame,
    const RenderView& view,
    const detail::renderer_ticket_t<Candidate>& ticket)
    noexcept(noexcept(renderer.render(
        frame,
        view,
        std::span<const detail::renderer_ticket_t<Candidate>>{
            std::addressof(ticket), 1})))
{
    return renderer.render(
        frame,
        view,
        std::span<const detail::renderer_ticket_t<Candidate>>{
            std::addressof(ticket), 1});
}

} // namespace vng::render
