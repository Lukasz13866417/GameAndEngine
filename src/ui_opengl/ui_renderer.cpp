#include <vng/ui_opengl/ui_renderer.hpp>
#include <vng/text_opengl/text_renderer.hpp>
#include <vng/opengl/gfx_vertex_input.hpp>
#include <vng/opengl/image.hpp>
#include "../opengl/gl_error.hpp"
#include <vng/render/program.hpp>
#include <vng/shader/shader.hpp>
#include <algorithm>
#include <cmath>
#include <map>

namespace vng::opengl {
namespace {
struct Position : gfx::Semantic<Vec2> {};
struct Local : gfx::Semantic<Vec2> {};
struct HalfSize : gfx::Semantic<Vec2> {};
struct Radius : gfx::Semantic<f32> {};
struct Stroke : gfx::Semantic<f32> {};
struct Tint : gfx::Semantic<Vec4> {};
struct Border : gfx::Semantic<Vec4> {};
using Vertex = gfx::Record<Position, Local, HalfSize, Radius, Stroke, Tint, Border>;
using Inputs = shader::VertexInputs<Position, Local, HalfSize, Radius, Stroke, Tint, Border>;
using Varyings =
    shader::VertexOutputs<shader::ClipPosition, shader::smooth<Local>, shader::smooth<HalfSize>,
                          shader::smooth<Radius>, shader::smooth<Stroke>, shader::smooth<Tint>,
                          shader::smooth<Border>>;
using Fragments =
    shader::FragmentInputs<shader::smooth<Local>, shader::smooth<HalfSize>, shader::smooth<Radius>,
                           shader::smooth<Stroke>, shader::smooth<Tint>, shader::smooth<Border>>;
Diagnostic invalid(std::string text) {
    return {.code = ErrorCode::invalid_argument, .message = std::move(text)};
}
std::expected<Program, Diagnostic> program(Device& device) {
    auto vertex = shader::vertex<Inputs, Varyings>("ui_boxes", [](auto& s) {
        return s.output(
            dsl::field<shader::ClipPosition>(dsl::vec4(s.input(Position{}), 0.0F, 1.0F)),
            dsl::field<Local>(s.input(Local{})), dsl::field<HalfSize>(s.input(HalfSize{})),
            dsl::field<Radius>(s.input(Radius{})), dsl::field<Stroke>(s.input(Stroke{})),
            dsl::field<Tint>(s.input(Tint{})), dsl::field<Border>(s.input(Border{})));
    });
    if (!vertex)
        return std::unexpected(invalid(vertex.error().message));
    auto fragment = shader::fragment<Fragments, shader::FragmentOutputs<shader::Color<0>>>(
        "ui_round_rect", [](auto& s) {
            auto radius = s.input(Radius{});
            auto q = dsl::abs(s.input(Local{})) - s.input(HalfSize{}) + dsl::vec2(radius, radius);
            auto outside = dsl::max(q, Vec2{0, 0});
            auto distance = dsl::sqrt(dsl::dot(outside, outside)) +
                            dsl::min(dsl::max(q.x(), q.y()), 0.0F) - radius;
            auto coverage = dsl::clamp(0.5F - distance, 0.0F, 1.0F);
            auto interior = dsl::clamp(0.5F - distance - s.input(Stroke{}), 0.0F, 1.0F);
            auto color = dsl::mix(s.input(Border{}), s.input(Tint{}), interior);
            auto alpha = color.w() * coverage;
            return s.output(dsl::field<shader::Color<0>>(dsl::vec4(color.xyz() * alpha, alpha)));
        });
    if (!fragment)
        return std::unexpected(invalid(fragment.error().message));
    auto linked = shader::link(std::move(*vertex), std::move(*fragment));
    if (!linked)
        return std::unexpected(invalid(linked.error().message));
    return render::compile_program(device, *linked);
}
std::expected<Program, Diagnostic> image_program(Device& device) {
    using Out = shader::VertexOutputs<shader::ClipPosition, shader::smooth<Local>>;
    using In = shader::FragmentInputs<shader::smooth<Local>>;
    auto vertex = shader::vertex<Inputs, Out>("ui_image_vertex", [](auto& s) {
        return s.output(
            dsl::field<shader::ClipPosition>(dsl::vec4(s.input(Position{}), 0.0F, 1.0F)),
            dsl::field<Local>(s.input(Local{})));
    });
    if (!vertex)
        return std::unexpected(invalid(vertex.error().message));
    auto fragment = shader::fragment<In, shader::FragmentOutputs<shader::Color<0>>>(
        "ui_image_fragment", [](auto& s) {
            // The sRGB texture storage decodes RGB only; alpha stays linear.
            auto color = s.template sample_2d<0>(s.input(Local{}));
            return s.output(
                dsl::field<shader::Color<0>>(dsl::vec4(color.xyz() * color.w(), color.w())));
        });
    if (!fragment)
        return std::unexpected(invalid(fragment.error().message));
    auto linked = shader::link(std::move(*vertex), std::move(*fragment));
    if (!linked)
        return std::unexpected(invalid(linked.error().message));
    return render::compile_program(device, *linked);
}
bool rect_ok(ui::Rect r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.width) &&
           std::isfinite(r.height) && std::abs(r.x) <= 1e7F && std::abs(r.y) <= 1e7F &&
           r.width >= 0 && r.height >= 0 && r.width <= 1e7F && r.height <= 1e7F;
}
bool color_ok(Vec4 c) {
    for (u32 i = 0; i < 4; ++i)
        if (!std::isfinite(c[i]) || c[i] < 0 || c[i] > 1)
            return false;
    return true;
}
} // namespace
struct UiRenderer::Impl {
    Program boxes;
    Program images;
    VertexArray vao;
    TextRenderer text;
    std::optional<Buffer> buffer;
    std::size_t capacity{};
    gfx::VertexStream<Vertex> vertices;
    std::vector<render::TextDraw> labels;
    struct Run {
        enum class Kind { boxes, text, image } kind{};
        u32 first{}, count{};
        const ui::ImageDraw* image{};
    };
    std::vector<Run> runs;
    render::UiRenderStats stats{};
    struct CachedImage {
        Image2D image;
        u64 revision{};
        std::weak_ptr<const gfx::ImageData> source;
        bool used{};
    };
    std::map<std::weak_ptr<const void>, CachedImage, std::owner_less<>> image_cache;
    void run(bool is_text, u32 first, u32 count) {
        if (!count)
            return;
        const auto kind = is_text ? Run::Kind::text : Run::Kind::boxes;
        if (!runs.empty() && runs.back().kind == kind)
            runs.back().count += count;
        else
            runs.push_back({kind, first, count, nullptr});
    }
    void append(const ui::BoxDraw& b, Vec2 scale, Extent2D extent) {
        const auto r = b.rect;
        const auto c = b.clip;
        const f32 x0 = std::max({0.0F, r.x, c.x}) * scale.x,
                  y0 = std::max({0.0F, r.y, c.y}) * scale.y;
        const f32 x1 =
            std::min({static_cast<f32>(extent.width) / scale.x, r.x + r.width, c.x + c.width}) *
            scale.x;
        const f32 y1 =
            std::min({static_cast<f32>(extent.height) / scale.y, r.y + r.height, c.y + c.height}) *
            scale.y;
        if (x1 <= x0 || y1 <= y0)
            return;
        const auto first = static_cast<u32>(vertices.size());
        const Vec2 half{r.width * scale.x * .5F, r.height * scale.y * .5F};
        const Vec2 center{r.x * scale.x + half.x, r.y * scale.y + half.y};
        const auto radius = std::min({b.radius * std::min(scale.x, scale.y), half.x, half.y});
        for (auto p :
             {Vec2{x0, y0}, Vec2{x0, y1}, Vec2{x1, y1}, Vec2{x0, y0}, Vec2{x1, y1}, Vec2{x1, y0}}) {
            Vertex v;
            v.set(Position{}, {p.x * 2 / static_cast<f32>(extent.width) - 1,
                               1 - p.y * 2 / static_cast<f32>(extent.height)});
            v.set(Local{}, {p.x - center.x, p.y - center.y});
            v.set(HalfSize{}, half);
            v.set(Radius{}, radius);
            v.set(Stroke{}, b.border_width * std::min(scale.x, scale.y));
            v.set(Tint{}, b.color);
            v.set(Border{}, b.border_width == 0 ? b.color : b.border);
            vertices.push_back(v);
        }
        run(false, first, 6);
        ++stats.boxes;
    }
    void append(const ui::TriangleDraw& b, Vec2 scale, Extent2D extent) {
        std::array<Vec2, 12> polygon{};
        std::copy(b.points.begin(),b.points.end(),polygon.begin());
        std::size_t count=3;
        const std::array limits{std::max(0.F,b.clip.x), std::min(static_cast<f32>(extent.width)/scale.x,b.clip.x+b.clip.width),
            std::max(0.F,b.clip.y), std::min(static_cast<f32>(extent.height)/scale.y,b.clip.y+b.clip.height)};
        for(unsigned plane=0;plane<4;++plane) {
            std::array<Vec2,12> clipped{};
            std::size_t next{};
            const auto distance=[&](Vec2 p) { const auto v=plane<2?p.x:p.y; return plane%2?limits[plane]-v:v-limits[plane]; };
            for(std::size_t i=0;i<count;++i) {
                const auto a=polygon[i], c=polygon[(i+1)%count];
                const auto da=distance(a), dc=distance(c);
                if(da>=0) clipped[next++]=a;
                if((da>=0)!=(dc>=0)) { const auto t=da/(da-dc); clipped[next++]={a.x+(c.x-a.x)*t,a.y+(c.y-a.y)*t}; }
            }
            polygon=clipped; count=next;
        }
        if(count<3) return;
        const auto first=static_cast<u32>(vertices.size());
        for(std::size_t i=1;i+1<count;++i) for(auto point:{polygon[0],polygon[i],polygon[i+1]}) {
            Vertex v;
            v.set(Position{}, {point.x*scale.x*2/static_cast<f32>(extent.width)-1,1-point.y*scale.y*2/static_cast<f32>(extent.height)});
            v.set(Local{}, {0,0}); v.set(HalfSize{}, {1,1}); v.set(Radius{},0); v.set(Stroke{},0);
            v.set(Tint{},b.color); v.set(Border{},b.color); vertices.push_back(v);
        }
        run(false,first,static_cast<u32>(vertices.size())-first);
    }
    void append(const ui::ImageDraw& b, Vec2 scale, Extent2D extent) {
        const auto r = b.rect, c = b.clip;
        const auto x0 = std::max({0.0F, r.x, c.x});
        const auto y0 = std::max({0.0F, r.y, c.y});
        const auto x1 =
            std::min({static_cast<f32>(extent.width) / scale.x, r.x + r.width, c.x + c.width});
        const auto y1 =
            std::min({static_cast<f32>(extent.height) / scale.y, r.y + r.height, c.y + c.height});
        if (x1 <= x0 || y1 <= y0)
            return;
        const auto first = static_cast<u32>(vertices.size());
        for (auto p :
             {Vec2{x0, y0}, Vec2{x0, y1}, Vec2{x1, y1}, Vec2{x0, y0}, Vec2{x1, y1}, Vec2{x1, y0}}) {
            Vertex v;
            v.set(Position{}, {p.x * scale.x * 2 / static_cast<f32>(extent.width) - 1,
                               1 - p.y * scale.y * 2 / static_cast<f32>(extent.height)});
            v.set(Local{}, {(p.x - r.x) / r.width, (p.y - r.y) / r.height});
            vertices.push_back(v);
        }
        runs.push_back({Run::Kind::image, first, 6, &b});
        ++stats.images;
    }
    std::expected<Image2D*, Diagnostic> image_for(const Device& device, const ui::ImageDraw& b) {
        const std::shared_ptr<const void> identity = b.identity ? b.identity : b.pixels;
        const std::weak_ptr<const void> key{identity};
        auto found = image_cache.find(key);
        if (found == image_cache.end() || found->second.image.extent() != b.pixels->extent) {
            auto image =
                upload_backend_image(device, *b.pixels,
                                     {.format = ImageFormat::srgb8_alpha8,
                                      .generate_mipmaps = false,
                                      .sampler = {.min_filter = gfx::ImageFilter::linear,
                                                  .mag_filter = gfx::ImageFilter::linear}});
            if (!image)
                return std::unexpected(std::move(image.error()));
            if (found != image_cache.end())
                image_cache.erase(found);
            found =
                image_cache.emplace(key, CachedImage{std::move(*image), b.revision, b.pixels, true})
                    .first;
            ++stats.image_uploads;
        } else {
            auto& entry = found->second;
            const std::weak_ptr<const gfx::ImageData> source{b.pixels};
            if (entry.revision != b.revision || entry.source.owner_before(source) ||
                source.owner_before(entry.source)) {
                if (auto updated = entry.image.write_rgba8(
                        0, 0, b.pixels->extent.width, b.pixels->extent.height, b.pixels->pixels);
                    !updated)
                    return std::unexpected(std::move(updated.error()));
                entry.revision = b.revision;
                entry.source = source;
                ++stats.image_uploads;
            }
            entry.used = true;
        }
        return &found->second.image;
    }
};
UiRenderer::UiRenderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}
UiRenderer::UiRenderer(UiRenderer&&) noexcept = default;
UiRenderer& UiRenderer::operator=(UiRenderer&&) noexcept = default;
UiRenderer::~UiRenderer() = default;
std::expected<UiRenderer, Diagnostic> UiRenderer::create(Device& device) {
    if (auto valid = device.require_resource_update("UiRenderer::create"); !valid)
        return std::unexpected(std::move(valid.error()));
    auto boxes = program(device);
    if (!boxes)
        return std::unexpected(std::move(boxes.error()));
    auto images = image_program(device);
    if (!images)
        return std::unexpected(std::move(images.error()));
    auto vao = VertexArray::create(device);
    if (!vao)
        return std::unexpected(std::move(vao.error()));
    auto text = TextRenderer::create(device);
    if (!text)
        return std::unexpected(std::move(text.error()));
    return UiRenderer{std::make_unique<Impl>(std::move(*boxes), std::move(*images), std::move(*vao),
                                             std::move(*text))};
}
std::expected<void, Diagnostic> UiRenderer::render(Frame& frame, const ui::Screen& screen) {
    auto draw = screen.draw_list();
    if (!draw)
        return std::unexpected(invalid(draw.error().message));
    return render(frame, *draw);
}
std::expected<void, Diagnostic> UiRenderer::render(Frame& frame, const ui::DrawList& list) {
    return render(frame, render::RenderView::without_camera(frame.extent()), std::span{&list, 1});
}
std::expected<void, Diagnostic> UiRenderer::render(Frame& frame, const render::RenderView& view,
                                                   std::span<const ui::DrawList> lists) {
    if (!impl_ || !frame.active() || frame.extent().empty() || view.extent() != frame.extent())
        return std::unexpected(invalid("UI renderer requires a live matching frame/view"));
    auto& p = *impl_;
    auto& device = frame.device();
    if (!p.boxes.belongs_to(device))
        return std::unexpected(invalid("UI renderer belongs to another device"));
    if (auto valid = device.require_current("UiRenderer::render"); !valid)
        return valid;
    p.stats = {};
    p.vertices.clear();
    p.labels.clear();
    p.runs.clear();
    for (auto& [identity, cached] : p.image_cache)
        cached.used = false;
    std::size_t total{};
    // Validate geometry up front. Text shaping/rasterization can still fail
    // in TextRenderer; submission is not an atomic framebuffer transaction.
    for (const auto& list : lists) {
        if (list.framebuffer != frame.extent() || !std::isfinite(list.logical_size.x) ||
            !std::isfinite(list.logical_size.y) || list.logical_size.x < 1 ||
            list.logical_size.y < 1 || list.logical_size.x > 100000 ||
            list.logical_size.y > 100000 ||
            static_cast<f32>(list.framebuffer.width) / list.logical_size.x > 64 ||
            static_cast<f32>(list.framebuffer.height) / list.logical_size.y > 64)
            return std::unexpected(invalid("UI viewport must match the render target"));
        total += list.commands.size();
        if (total > 100000)
            return std::unexpected(invalid("UI command limit exceeded"));
        for (const auto& command : list.commands) {
            if (const auto* b = std::get_if<ui::BoxDraw>(&command)) {
                if (!rect_ok(b->rect) || !rect_ok(b->clip) || !color_ok(b->color) ||
                    !color_ok(b->border) || !std::isfinite(b->radius) || b->radius < 0 ||
                    !std::isfinite(b->border_width) || b->border_width < 0)
                    return std::unexpected(invalid("Invalid UI rectangle"));
            } else if (const auto* i = std::get_if<ui::ImageDraw>(&command)) {
                if (!rect_ok(i->rect) || !rect_ok(i->clip) || !i->pixels ||
                    i->pixels->extent.empty() || i->pixels->extent.width > 16384 ||
                    i->pixels->extent.height > 16384 ||
                    i->pixels->pixels.size() != static_cast<std::size_t>(i->pixels->extent.width) *
                                                    i->pixels->extent.height * 4)
                    return std::unexpected(invalid("Invalid UI RGBA8 image"));
            } else if (const auto* triangle = std::get_if<ui::TriangleDraw>(&command)) {
                if(!rect_ok(triangle->clip) || !color_ok(triangle->color) || std::ranges::any_of(triangle->points,[](Vec2 p) {
                    return !std::isfinite(p.x)||!std::isfinite(p.y)||std::abs(p.x)>1e7F||std::abs(p.y)>1e7F;
                })) return std::unexpected(invalid("Invalid UI triangle"));
            } else {
                const auto& t = std::get<ui::TextDraw>(command);
                if (!t.font || !rect_ok(t.clip) || !color_ok(t.color) || !std::isfinite(t.size) ||
                    t.size < 1 || t.size > 4096 || !std::isfinite(t.position.x) ||
                    !std::isfinite(t.position.y) || std::abs(t.position.x) > 1e7F ||
                    std::abs(t.position.y) > 1e7F || t.text.size() > 65536)
                    return std::unexpected(invalid("Invalid UI text"));
            }
        }
    }
    for (const auto& list : lists) {
        const Vec2 scale{static_cast<f32>(list.framebuffer.width) / list.logical_size.x,
                         static_cast<f32>(list.framebuffer.height) / list.logical_size.y};
        for (const auto& command : list.commands) {
            if (const auto* b = std::get_if<ui::BoxDraw>(&command))
                p.append(*b, scale, frame.extent());
            else if (const auto* i = std::get_if<ui::ImageDraw>(&command))
                p.append(*i, scale, frame.extent());
            else if (const auto* triangle = std::get_if<ui::TriangleDraw>(&command))
                p.append(*triangle, scale, frame.extent());
            else {
                const auto& t = std::get<ui::TextDraw>(command);
                const auto first = static_cast<u32>(p.labels.size());
                p.labels.push_back(
                    {.text = t.text,
                     .position = {t.position.x * scale.x, t.position.y * scale.y},
                     .size =
                         static_cast<u32>(std::clamp(std::round(t.size * scale.y), 1.0F, 4096.0F)),
                     .color = t.color,
                     .font = t.font,
                     .clip = render::TextClip{t.clip.x * scale.x, t.clip.y * scale.y,
                                              t.clip.width * scale.x, t.clip.height * scale.y}});
                p.run(true, first, 1);
            }
        }
    }
    if (!p.vertices.empty()) {
        if (p.capacity < p.vertices.byte_size()) {
            const auto capacity =
                std::max({std::size_t{4096}, p.capacity * 2, p.vertices.byte_size()});
            auto buffer =
                Buffer::create(device, {.size = capacity, .storage = BufferStorage::dynamic});
            if (!buffer)
                return std::unexpected(std::move(buffer.error()));
            using Layout = gfx::VertexLayout<gfx::Stream<Vertex, gfx::PerVertex>>;
            const auto layout = gfx::resolve_vertex_input<Inputs, Layout>();
            const std::array bindings{ResolvedStreamBuffer{0, &*buffer, 0}};
            if (auto configured = configure_vertex_input(p.vao, layout, bindings); !configured)
                return configured;
            p.buffer.emplace(std::move(*buffer));
            p.capacity = capacity;
        }
        if (auto uploaded = p.buffer->write(0, p.vertices.bytes()); !uploaded)
            return uploaded;
    }
    // Prepare every label together before any draws. Re-uploading the same
    // text buffer once per button forces GPU/CPU synchronization between runs.
    if (auto prepared = p.text.prepare(frame, p.labels); !prepared)
        return prepared;
    // Text and images share one unit/state scope across all painter-order runs.
    struct UnitScope {
        GLint texture{}, sampler{};
        UnitScope() {
            glGetIntegeri_v(GL_TEXTURE_BINDING_2D, 0, &texture);
            glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &sampler);
            glBindSampler(0, 0);
        }
        ~UnitScope() {
            GLint active{};
            glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
            glActiveTexture(static_cast<GLenum>(active));
            glBindSampler(0, static_cast<GLuint>(sampler));
        }
    } unit_scope;
    auto context = frame.render_context();
    auto graphics = context.graphics_state();
    for (const auto& run : p.runs) {
        if (run.kind == Impl::Run::Kind::text) {
            if (auto drawn = p.text.draw_prepared(device, context, run.first, run.count); !drawn)
                return drawn;
        } else {
            const auto image_run = run.kind == Impl::Run::Kind::image;
            if (auto done = context.run(image_run ? p.images : p.boxes); !done)
                return done;
            if (auto done = graphics.set(render::DepthState{false, false}); !done)
                return done;
            if (auto done = graphics.set(render::CullMode::none); !done)
                return done;
            if (auto done = graphics.set(render::BlendMode::premultiplied_alpha); !done)
                return done;
            if (auto done = graphics.set(PolygonMode::fill); !done)
                return done;
            if (auto done = p.vao.bind(); !done)
                return done;
            if (image_run) {
                auto image = p.image_for(device, *run.image);
                if (!image)
                    return std::unexpected(std::move(image.error()));
                if (auto done = (*image)->bind_to_unit(0); !done)
                    return done;
            }
            if (auto done =
                    device.draw_arrays_instanced(Primitive::triangles, run.first, run.count);
                !done)
                return done;
            ++p.stats.draw_calls;
        }
    }
    const auto text_stats = p.text.stats();
    p.stats.glyphs = text_stats.glyphs;
    p.stats.glyph_uploads = text_stats.glyph_uploads;
    p.stats.text_vertex_uploads = text_stats.vertex_uploads;
    p.stats.draw_calls += text_stats.draw_calls;
    std::erase_if(p.image_cache, [](const auto& entry) { return !entry.second.used; });
    p.stats.cached_images = static_cast<u32>(p.image_cache.size());
    return {};
}
render::UiRenderStats UiRenderer::stats() const noexcept {
    return impl_ ? impl_->stats : render::UiRenderStats{};
}
std::expected<void, Diagnostic> UiRenderer::clear_cache(const Device& device) {
    if (!impl_)
        return std::unexpected(invalid("Empty UI renderer"));
    if (auto done = impl_->text.clear_cache(device); !done)
        return done;
    impl_->image_cache.clear();
    impl_->stats = {};
    return {};
}
static_assert(render::RendererFor<UiRenderer, Frame>);
} // namespace vng::opengl
