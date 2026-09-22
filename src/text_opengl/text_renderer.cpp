#include <vng/text_opengl/text_renderer.hpp>

#include <vng/gfx/gfx.hpp>
#include <vng/opengl/frame.hpp>
#include <vng/opengl/gfx_vertex_input.hpp>
#include <vng/opengl/image.hpp>
#include <vng/render/program.hpp>
#include <vng/shader/shader.hpp>

#include "../opengl/gl_error.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vng::opengl {
namespace {

class ProvisionScope final {
public:
    explicit ProvisionScope(bool& providing) : providing_(providing) { providing_ = true; }
    ~ProvisionScope() { providing_ = false; }
    ProvisionScope(const ProvisionScope&) = delete;
    ProvisionScope& operator=(const ProvisionScope&) = delete;

private:
    bool& providing_;
};

struct Position : gfx::Semantic<Vec2> {};
struct UV : gfx::Semantic<Vec2> {};
struct Tint : gfx::Semantic<Vec4> {};
using Vertex = gfx::Record<Position, UV, Tint>;
using Layout = gfx::VertexLayout<gfx::Stream<Vertex, gfx::PerVertex>>;
using Inputs = shader::VertexInputs<Position, UV, Tint>;
using Varyings =
    shader::VertexOutputs<shader::ClipPosition, shader::smooth<UV>, shader::smooth<Tint>>;
using FragmentInputs = shader::FragmentInputs<shader::smooth<UV>, shader::smooth<Tint>>;
using Outputs = shader::FragmentOutputs<shader::Color<0>>;

Diagnostic invalid(std::string message) {
    return {.code = ErrorCode::invalid_argument, .message = std::move(message)};
}

Diagnostic font_error(const text::Diagnostic& error) {
    return invalid("Text renderer: " + error.message);
}

std::expected<Program, Diagnostic> create_program(const Device& device) {
    auto vertex = shader::vertex<Inputs, Varyings>("text_vertex", [](auto& s) {
        return s.output(
            dsl::field<shader::ClipPosition>(dsl::vec4(s.input(Position{}), 0.0F, 1.0F)),
            dsl::field<UV>(s.input(UV{})), dsl::field<Tint>(s.input(Tint{})));
    });
    if (!vertex)
        return std::unexpected(invalid(vertex.error().message));
    auto fragment = shader::fragment<FragmentInputs, Outputs>("text_fragment", [](auto& s) {
        auto tint = s.input(Tint{});
        auto alpha = tint.w() * s.template sample_2d<0>(s.input(UV{})).x();
        return s.output(dsl::field<shader::Color<0>>(dsl::vec4(tint.xyz() * alpha, alpha)));
    });
    if (!fragment)
        return std::unexpected(invalid(fragment.error().message));
    auto linked = shader::link(std::move(*vertex), std::move(*fragment));
    if (!linked)
        return std::unexpected(invalid(linked.error().message));
    return render::compile_program(device, *linked);
}

struct GlyphKey {
    u64 font{};
    u32 size{}, glyph{};
    friend bool operator==(const GlyphKey&, const GlyphKey&) = default;
};
struct GlyphHash {
    std::size_t operator()(const GlyphKey& key) const noexcept {
        return std::hash<u64>{}(key.font) ^ (std::hash<u32>{}(key.glyph) << 1) ^
               (std::hash<u32>{}(key.size) << 17);
    }
};
struct LayoutKey {
    u64 font{};
    u32 size{};
    std::string text;
    friend bool operator==(const LayoutKey&, const LayoutKey&) = default;
};
struct LayoutLookup {
    u64 font{};
    u32 size{};
    std::string_view text;
};
struct LayoutHash {
    using is_transparent = void;
    std::size_t operator()(const LayoutLookup& key) const noexcept {
        return std::hash<std::string_view>{}(key.text) ^ std::hash<u64>{}(key.font) ^
               (std::hash<u32>{}(key.size) << 1);
    }
    std::size_t operator()(const LayoutKey& key) const noexcept {
        return (*this)(LayoutLookup{key.font, key.size, key.text});
    }
};
struct LayoutEqual {
    using is_transparent = void;
    template <class A, class B> bool operator()(const A& a, const B& b) const noexcept {
        return a.font == b.font && a.size == b.size && a.text == b.text;
    }
};

std::expected<void, Diagnostic> validate_ticket(const render::TextDraw& ticket) {
    if (ticket.size == 0 || ticket.size > 4096) {
        return std::unexpected(invalid("Text size must be in [1, 4096] framebuffer pixels"));
    }
    if (!std::isfinite(ticket.position.x) || !std::isfinite(ticket.position.y)) {
        return std::unexpected(invalid("Text position must be finite"));
    }
    for (u32 i = 0; i < 4; ++i) {
        if (!std::isfinite(ticket.color[i]) || ticket.color[i] < 0 || ticket.color[i] > 1) {
            return std::unexpected(invalid("Text color components must be finite and in [0, 1]"));
        }
    }
    if (ticket.clip) {
        const auto& clip = *ticket.clip;
        if (!std::isfinite(clip.x) || !std::isfinite(clip.y) || !std::isfinite(clip.width) ||
            !std::isfinite(clip.height) || clip.width < 0 || clip.height < 0) {
            return std::unexpected(
                invalid("Text clip rectangle must be finite with nonnegative dimensions"));
        }
    }
    return {};
}

// A sampler object overrides texture filtering. Borrow unit 0 once for the
// whole submission; UI owns the equivalent scope when drawing prepared text.
class UnitScope final {
public:
    UnitScope() {
        glGetIntegeri_v(GL_TEXTURE_BINDING_2D, 0, &texture_);
        glGetIntegeri_v(GL_SAMPLER_BINDING, 0, &sampler_);
        glBindSampler(0, 0);
    }
    ~UnitScope() {
        GLint active{};
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_));
        glActiveTexture(static_cast<GLenum>(active));
        glBindSampler(0, static_cast<GLuint>(sampler_));
    }

private:
    GLint texture_{}, sampler_{};
};

} // namespace

struct TextRenderer::Impl {
    struct Glyph {
        u32 page{}, x{}, y{}, width{}, height{};
        i32 left{}, top{};
    };
    struct Page {
        Image2D image;
        u32 x{1}, y{1}, row_height{};
    };
    struct CachedLayout {
        text::TextLayout layout;
        u64 use{};
    };
    struct Run {
        u32 page{}, first{}, count{};
    };

    text::Font default_font;
    FontProvider font_provider;
    render::TextRendererOptions options;
    Program program;
    std::optional<Buffer> vertices;
    VertexArray vao;
    std::size_t capacity{};
    std::vector<Page> pages;
    std::unordered_map<GlyphKey, Glyph, GlyphHash> glyphs;
    std::unordered_map<LayoutKey, CachedLayout, LayoutHash, LayoutEqual> layouts;
    text::TextLayout uncached_layout;
    u64 clock{};
    gfx::VertexStream<Vertex> staging;
    std::vector<Run> runs;
    std::vector<u32> ticket_offsets;
    render::TextRenderStats stats;

    Impl(text::Font font, render::TextRendererOptions settings, Program shader, VertexArray array)
        : default_font(std::move(font)), options(settings), program(std::move(shader)),
          vao(std::move(array)) {}

    const text::Font& font_for(const render::TextDraw& ticket) const {
        return ticket.font ? ticket.font : default_font;
    }

    std::expected<const text::TextLayout*, Diagnostic> layout_for(const render::TextDraw& ticket) {
        const auto& font = font_for(ticket);
        const LayoutLookup key{font.id(), ticket.size, ticket.text};
        if (auto found = layouts.find(key); found != layouts.end()) {
            ++stats.layout_hits;
            found->second.use = ++clock;
            return &found->second.layout;
        }
        auto shaped = font.layout(ticket.text, ticket.size);
        if (!shaped)
            return std::unexpected(font_error(shaped.error()));
        if (options.max_cached_layouts == 0) {
            uncached_layout = std::move(*shaped);
            return &uncached_layout;
        }
        if (layouts.size() >= options.max_cached_layouts) {
            auto oldest = std::min_element(layouts.begin(), layouts.end(), [](auto& a, auto& b) {
                return a.second.use < b.second.use;
            });
            layouts.erase(oldest);
        }
        auto [entry, inserted] =
            layouts.emplace(LayoutKey{key.font, key.size, std::string{key.text}},
                            CachedLayout{std::move(*shaped), ++clock});
        (void)inserted;
        return &entry->second.layout;
    }

    std::expected<Glyph, Diagnostic> glyph_for(const Device& device, const text::Font& font,
                                               u32 size, u32 index) {
        const GlyphKey key{font.id(), size, index};
        if (auto found = glyphs.find(key); found != glyphs.end())
            return found->second;
        auto bitmap = font.rasterize(index, size);
        if (!bitmap)
            return std::unexpected(font_error(bitmap.error()));
        Glyph glyph{.width = bitmap->width,
                    .height = bitmap->height,
                    .left = bitmap->left,
                    .top = bitmap->top};
        if (glyph.width == 0 || glyph.height == 0) {
            glyphs.emplace(key, glyph);
            return glyph;
        }
        if (glyph.width > options.atlas_size - 2 || glyph.height > options.atlas_size - 2) {
            return std::unexpected(
                invalid("Glyph exceeds atlas page size; increase TextRendererOptions::atlas_size"));
        }
        bool packed = false;
        for (u32 i = 0; i <= pages.size(); ++i) {
            if (i == pages.size()) {
                if (pages.size() >= options.max_atlas_pages) {
                    return std::unexpected(
                        invalid("Text atlas is full; clear_cache or increase max_atlas_pages"));
                }
                auto image = Image2D::create(device, options.atlas_size, options.atlas_size,
                                             ImageFormat::r8);
                if (!image)
                    return std::unexpected(std::move(image.error()));
                if (auto cleared = image->clear_r8(); !cleared)
                    return std::unexpected(std::move(cleared.error()));
                if (auto filtered = image->use_linear_filtering(); !filtered)
                    return std::unexpected(std::move(filtered.error()));
                pages.push_back(Page{std::move(*image)});
            }
            auto& page = pages[i];
            auto x = page.x, y = page.y, row = page.row_height;
            if (x + glyph.width + 1 > options.atlas_size) {
                x = 1;
                y += row + 1;
                row = 0;
            }
            if (y + glyph.height + 1 > options.atlas_size)
                continue;
            glyph.page = i;
            glyph.x = x;
            glyph.y = y;
            auto uploaded = page.image.write_r8(x, y, glyph.width, glyph.height,
                                                std::as_bytes(std::span{bitmap->pixels}));
            if (!uploaded)
                return std::unexpected(std::move(uploaded.error()));
            page.x = x + glyph.width + 1;
            page.y = y;
            page.row_height = std::max(row, glyph.height);
            packed = true;
            break;
        }
        if (!packed)
            return std::unexpected(invalid("Unable to pack text glyph"));
        ++stats.glyph_uploads;
        glyphs.emplace(key, glyph);
        return glyph;
    }

    void append(const Glyph& glyph, const text::PositionedGlyph& positioned,
                const render::TextDraw& ticket, Extent2D extent) {
        if (!glyph.width || !glyph.height || ticket.color.w == 0)
            return;
        const double x =
            static_cast<double>(ticket.position.x) + positioned.position.x + glyph.left;
        const double y = static_cast<double>(ticket.position.y) + positioned.position.y - glyph.top;
        double x0 = std::max(0.0, x), y0 = std::max(0.0, y);
        double x1 = std::min<double>(extent.width, x + glyph.width);
        double y1 = std::min<double>(extent.height, y + glyph.height);
        if (ticket.clip) {
            const auto& clip = *ticket.clip;
            x0 = std::max<double>(x0, clip.x);
            y0 = std::max<double>(y0, clip.y);
            x1 = std::min(x1, static_cast<double>(clip.x) + clip.width);
            y1 = std::min(y1, static_cast<double>(clip.y) + clip.height);
        }
        if (x1 <= x0 || y1 <= y0)
            return;
        const auto side = static_cast<double>(options.atlas_size);
        const f32 u0 = static_cast<f32>((glyph.x + x0 - x) / side);
        const f32 v0 = static_cast<f32>((glyph.y + y0 - y) / side);
        const f32 u1 = static_cast<f32>((glyph.x + x1 - x) / side);
        const f32 v1 = static_cast<f32>((glyph.y + y1 - y) / side);
        const f32 left = static_cast<f32>(x0 / extent.width * 2 - 1);
        const f32 right = static_cast<f32>(x1 / extent.width * 2 - 1);
        const f32 top = static_cast<f32>(1 - y0 / extent.height * 2);
        const f32 bottom = static_cast<f32>(1 - y1 / extent.height * 2);
        if (runs.empty() || runs.back().page != glyph.page) {
            runs.push_back({glyph.page, static_cast<u32>(staging.size()), 0});
        }
        const auto add = [&](Vec2 position, Vec2 uv) {
            auto& vertex = staging.emplace_back();
            vertex.set(Position{}, position);
            vertex.set(UV{}, uv);
            vertex.set(Tint{}, ticket.color);
        };
        add({left, top}, {u0, v0});
        add({left, bottom}, {u0, v1});
        add({right, bottom}, {u1, v1});
        add({left, top}, {u0, v0});
        add({right, bottom}, {u1, v1});
        add({right, top}, {u1, v0});
        runs.back().count += 6;
        ++stats.glyphs;
    }

    std::expected<void, Diagnostic> upload_vertices(const Device& device) {
        if (capacity < staging.byte_size()) {
            const auto new_capacity =
                std::max(staging.byte_size(), std::max<std::size_t>(4096, capacity * 2));
            auto buffer = Buffer::create(
                device,
                {.size = new_capacity, .initial_data = {}, .storage = BufferStorage::dynamic});
            if (!buffer)
                return std::unexpected(std::move(buffer.error()));
            const auto layout = gfx::resolve_vertex_input<Inputs, Layout>();
            const std::array binding{ResolvedStreamBuffer{0, &*buffer, 0}};
            if (auto configured = configure_vertex_input(vao, layout, binding); !configured)
                return configured;
            vertices.emplace(std::move(*buffer));
            capacity = new_capacity;
        }
        auto uploaded = vertices->write(0, staging.bytes());
        if (uploaded)
            ++stats.vertex_uploads;
        return uploaded;
    }
};

TextRenderer::TextRenderer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {
}
TextRenderer::TextRenderer(TextRenderer&&) noexcept = default;
TextRenderer& TextRenderer::operator=(TextRenderer&&) noexcept = default;
TextRenderer::~TextRenderer() = default;

std::expected<TextRenderer, Diagnostic> TextRenderer::create(Device& device, text::Font font,
                                                             render::TextRendererOptions options) {
    if (options.atlas_size < 16 || options.atlas_size > 16384 || options.max_atlas_pages == 0 ||
        options.max_atlas_pages > 256 || options.max_glyphs_per_render == 0 ||
        options.max_glyphs_per_render > 1000000 || options.max_cached_layouts > 65536) {
        return std::unexpected(invalid("Invalid text renderer cache limits"));
    }
    if (auto current = device.require_current("TextRenderer::create"); !current)
        return std::unexpected(std::move(current.error()));
    auto program = create_program(device);
    if (!program)
        return std::unexpected(std::move(program.error()));
    auto vao = VertexArray::create(device);
    if (!vao)
        return std::unexpected(std::move(vao.error()));
    return TextRenderer{
        std::make_unique<Impl>(std::move(font), options, std::move(*program), std::move(*vao))};
}

std::expected<void, Diagnostic> TextRenderer::render(Frame& frame, const render::RenderView& view,
                                                     std::span<const render::TextDraw> tickets) {
    if (view.extent() != frame.extent()) {
        return std::unexpected(invalid("Text render view must match the frame extent"));
    }
    return render(frame, tickets);
}

std::expected<void, Diagnostic> TextRenderer::render(Frame& frame, const render::TextDraw& ticket) {
    return render(frame, std::span{&ticket, 1});
}

std::expected<void, Diagnostic> TextRenderer::render(Frame& frame,
                                                     std::span<const render::TextDraw> tickets) {
    if (auto prepared = prepare(frame, tickets); !prepared)
        return prepared;
    if (impl_->staging.empty())
        return {};
    const UnitScope unit_scope;
    auto commands = frame.render_context();
    return draw_prepared(frame.device(), commands, 0, static_cast<u32>(tickets.size()));
}

std::expected<void, Diagnostic> TextRenderer::prepare(Frame& frame,
                                                      std::span<const render::TextDraw> tickets) {
    if (!impl_ || !frame.active() || frame.extent().empty()) {
        return std::unexpected(invalid("TextRenderer::render requires a live renderer and frame"));
    }
    const auto& device = frame.device();
    if (!impl_->program.belongs_to(device)) {
        return std::unexpected(
            Diagnostic{.code = ErrorCode::incompatible_device,
                       .message = "Text renderer belongs to a different device/context"});
    }
    if (auto current = device.require_current("TextRenderer::render"); !current)
        return current;
    if (tickets.size() > std::numeric_limits<u32>::max())
        return std::unexpected(invalid("Text submission has too many tickets"));
    impl_->stats = {};
    impl_->stats.atlas_pages = static_cast<u32>(impl_->pages.size());
    impl_->staging.clear();
    impl_->runs.clear();
    impl_->ticket_offsets.clear();
    u64 glyph_count = 0;
    for (const auto& ticket : tickets) {
        impl_->ticket_offsets.push_back(static_cast<u32>(impl_->staging.size()));
        if (auto valid = validate_ticket(ticket); !valid)
            return valid;
        if (!impl_->font_for(ticket))
            return std::unexpected(
                invalid("Text ticket has no font and renderer has no default font"));
        if (ticket.text.empty() || ticket.color.w == 0 ||
            (ticket.clip && (ticket.clip->width == 0 || ticket.clip->height == 0)))
            continue;
        auto layout = impl_->layout_for(ticket);
        if (!layout)
            return std::unexpected(std::move(layout.error()));
        glyph_count += (*layout)->glyphs.size();
        if (glyph_count > impl_->options.max_glyphs_per_render) {
            return std::unexpected(invalid("Text submission exceeds max_glyphs_per_render"));
        }
        for (const auto& positioned : (*layout)->glyphs) {
            auto glyph = impl_->glyph_for(device, impl_->font_for(ticket), ticket.size,
                                          positioned.glyph_index);
            if (!glyph)
                return std::unexpected(std::move(glyph.error()));
            impl_->append(*glyph, positioned, ticket, frame.extent());
        }
    }
    impl_->ticket_offsets.push_back(static_cast<u32>(impl_->staging.size()));
    impl_->stats.atlas_pages = static_cast<u32>(impl_->pages.size());
    if (impl_->staging.empty())
        return {};
    return impl_->upload_vertices(device);
}

std::expected<void, Diagnostic> TextRenderer::draw_prepared(const Device& device, Commands& context,
                                                            u32 first_ticket, u32 ticket_count) {
    if (!impl_ || first_ticket >= impl_->ticket_offsets.size() ||
        ticket_count >= impl_->ticket_offsets.size() - first_ticket)
        return std::unexpected(invalid("Text draw range exceeds the prepared submission"));
    const auto first = impl_->ticket_offsets[first_ticket];
    const auto end = impl_->ticket_offsets[first_ticket + ticket_count];
    if (first == end)
        return {};
    auto graphics = context.graphics_state();
    if (auto selected = context.run(impl_->program); !selected)
        return selected;
    if (auto changed = graphics.set(render::DepthTest{false}); !changed)
        return changed;
    if (auto changed = graphics.set(render::DepthWrite{false}); !changed)
        return changed;
    if (auto changed = graphics.set(render::CullMode::none); !changed)
        return changed;
    if (auto changed = graphics.set(render::BlendMode::premultiplied_alpha); !changed)
        return changed;
    if (auto changed = graphics.set(PolygonMode::fill); !changed)
        return changed;
    if (auto bound = impl_->vao.bind(); !bound)
        return bound;

    auto run = std::lower_bound(
        impl_->runs.begin(), impl_->runs.end(), first,
        [](const Impl::Run& value, u32 start) { return value.first + value.count <= start; });
    for (; run != impl_->runs.end() && run->first < end; ++run) {
        const auto begin = std::max(run->first, first);
        const auto count = std::min(run->first + run->count, end) - begin;
        if (auto bound = impl_->pages[run->page].image.bind_to_unit(0); !bound)
            return bound;
        if (auto drawn = device.draw_arrays_instanced(Primitive::triangles, begin, count); !drawn)
            return drawn;
        ++impl_->stats.draw_calls;
    }
    return {};
}

std::expected<text::TextMetrics, Diagnostic>
TextRenderer::measure(const render::TextDraw& ticket) const {
    if (!impl_)
        return std::unexpected(invalid("TextRenderer::measure used an empty renderer"));
    if (auto valid = validate_ticket(ticket); !valid)
        return std::unexpected(std::move(valid.error()));
    auto metrics = impl_->font_for(ticket).measure(ticket.text, ticket.size);
    if (!metrics)
        return std::unexpected(font_error(metrics.error()));
    return *metrics;
}

render::TextRenderStats TextRenderer::stats() const noexcept {
    return impl_ ? impl_->stats : render::TextRenderStats{};
}

std::expected<void, Diagnostic> TextRenderer::clear_cache(const Device& device) {
    if (providing_) {
        return std::unexpected(
            invalid("A font provider cannot clear its owner's caches reentrantly"));
    }
    if (!impl_ || !impl_->program.belongs_to(device)) {
        return std::unexpected(invalid("TextRenderer::clear_cache requires its owning device"));
    }
    if (auto current = device.require_resource_update("TextRenderer::clear_cache"); !current)
        return current;
    impl_->glyphs.clear();
    impl_->layouts.clear();
    impl_->pages.clear();
    impl_->stats = {};
    impl_->uncached_layout = {};
    impl_->staging.clear();
    impl_->runs.clear();
    return {};
}

resources::Result<void> TextRenderer::validate_update(const Device& device) const {
    if (providing_) {
        return std::unexpected(resources::Diagnostic{
            .code = resources::ErrorCode::operation_failed,
            .message = "A font provider cannot update its owner reentrantly"});
    }
    if (!impl_ || !impl_->program.belongs_to(device)) {
        return std::unexpected(resources::to_diagnostic(
            invalid("Text resource update requires a live renderer and its owning device")));
    }
    return resources::into_result(device.require_resource_update("TextRenderer resource update"));
}

resources::Result<void> TextRenderer::install_font(Device& device, text::Font font,
                                                   FontProvider provider) {
    if (auto valid = validate_update(device); !valid)
        return valid;
    if (!font) {
        return std::unexpected(resources::to_diagnostic(
            text::Diagnostic{.message = "A replacement default font must be valid"}));
    }
    // There is no fallible work after this point. All font-dependent caches
    // expire together; fonts carried by individual tickets remain their owners'.
    impl_->default_font = std::move(font);
    impl_->font_provider = std::move(provider);
    impl_->glyphs.clear();
    impl_->layouts.clear();
    impl_->pages.clear();
    impl_->uncached_layout = {};
    impl_->staging.clear();
    impl_->runs.clear();
    impl_->stats = {};
    return {};
}

resources::Result<void> TextRenderer::reload_font(Device& device) {
    if (auto valid = validate_update(device); !valid)
        return valid;
    return reload_font(device, impl_->font_provider);
}

resources::Result<void> TextRenderer::reload_font(Device& device, FontProvider provider) {
    if (auto valid = validate_update(device); !valid)
        return valid;
    auto font = [&] {
        const ProvisionScope scope{providing_};
        return provider.provide(device);
    }();
    if (!font)
        return std::unexpected(std::move(font.error()));
    return install_font(device, std::move(*font), std::move(provider));
}

resources::Result<void> TextRenderer::replace_font(Device& device, text::Font font) {
    return install_font(device, std::move(font), {});
}

resources::Result<resources::ReloadReport> TextRenderer::reload(Device& device) {
    if (auto valid = validate_update(device); !valid)
        return std::unexpected(std::move(valid.error()));
    auto font = impl_->default_font;
    const auto provider = impl_->font_provider;
    resources::ReloadReport report;
    if (provider) {
        auto supplied = [&] {
            const ProvisionScope scope{providing_};
            return provider.provide(device);
        }();
        if (!supplied)
            return std::unexpected(std::move(supplied.error()));
        if (!*supplied) {
            return std::unexpected(resources::to_diagnostic(
                text::Diagnostic{.message = "The font provider returned an empty font"}));
        }
        font = std::move(*supplied);
        report.refreshed.push_back("font");
    } else {
        report.retained.push_back("font");
    }
    if (auto valid = validate_update(device); !valid)
        return std::unexpected(std::move(valid.error()));
    auto candidate = create(device, std::move(font), impl_->options);
    if (!candidate)
        return std::unexpected(resources::to_diagnostic(std::move(candidate.error())));
    candidate->impl_->font_provider = provider;
    report.refreshed.push_back("program");
    report.refreshed.push_back("font caches");
    if (auto valid = validate_update(device); !valid)
        return std::unexpected(std::move(valid.error()));
    impl_.swap(candidate->impl_);
    return report;
}

resources::Result<TextRenderer> TextRendererBuilder::build() const {
    auto& device = *device_;
    const auto provider = provider_;
    const auto options = options_;
    if (auto ready = device.require_resource_update("TextRendererBuilder::build"); !ready)
        return std::unexpected(resources::to_diagnostic(std::move(ready.error())));
    auto font = font_;
    if (provider) {
        auto supplied = provider.provide(device);
        if (!supplied)
            return std::unexpected(std::move(supplied.error()));
        if (!*supplied) {
            return std::unexpected(resources::to_diagnostic(
                text::Diagnostic{.message = "The font provider returned an empty font"}));
        }
        font = std::move(*supplied);
    }
    if (auto ready = device.require_resource_update("TextRendererBuilder::build commit"); !ready)
        return std::unexpected(resources::to_diagnostic(std::move(ready.error())));
    auto renderer = TextRenderer::create(device, std::move(font), options);
    if (!renderer)
        return std::unexpected(resources::to_diagnostic(std::move(renderer.error())));
    renderer->impl_->font_provider = provider;
    return std::move(*renderer);
}

std::expected<TextRenderer, Diagnostic>
make_backend_text_renderer(Device& device, text::Font default_font,
                           render::TextRendererOptions options) {
    return TextRenderer::create(device, std::move(default_font), options);
}

static_assert(render::RendererFor<TextRenderer, Frame>);

} // namespace vng::opengl
