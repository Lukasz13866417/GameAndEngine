#include "runtime.hpp"
#include "scene_annotations.hpp"
#include "mesh_programs.hpp"
#include "edits.hpp"
#include "animation.hpp"
#include "preview_values.hpp"
#include "scene_samples.hpp"
#include "mesh_shading.hpp"
#include "blueprint_mesh_renderer.hpp"
#include "starfield.hpp"
#include "../file_mesh_types.hpp"
#include "../support/sun_renderer.hpp"
#include "../support/sun_softening.hpp"
#include "../support/presentation.hpp"
#include <vng/render_opengl/program_runtime.hpp>
#include <vng/bloom_opengl/bloom.hpp>
#include <vng/content/vmesh_schema.hpp>
#include <vng/shader/shader.hpp>
#include <vng/opengl/render_state_scope.hpp>
#include <vng/providers/target.hpp>
#include <glad/gl.h>
#include <chrono>
#include <cstring>
#include <map>
#include <sstream>

namespace editor_example {
using namespace vng;
namespace {
using namespace file_mesh_example;
using namespace mesh_shading;
using Clock = std::chrono::steady_clock;
struct Timing {
    double& destination;
    Clock::time_point start{Clock::now()};
    ~Timing() {
        destination = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
};
auto invalid(std::string message) {
    resources::Diagnostic diagnostic;
    diagnostic.code = resources::ErrorCode::invalid_argument;
    diagnostic.message = std::move(message);
    return std::unexpected(std::move(diagnostic));
}
template <class T, class E> auto result(std::expected<T, E> value) {
    return resources::into_result(std::move(value));
}
struct MeshResource {
    content::vmesh::Document source_document;
    Mesh mesh;
    BlueprintMeshRenderer renderer;
    vng::u64 visibility_revision{};
    bool masked{};
};
resources::Result<MeshResource> realize_mesh(opengl::Device& device,
                                            MeshPrograms& programs,
                                            const content::vmesh::Document& document,
                                            Mesh mesh) {
    auto gpu = result(opengl::upload_mesh(device, mesh, {.dynamic_vertices = true,.dynamic_face_visibility=true}));
    if (!gpu) return std::unexpected(gpu.error());
    auto program=programs.provide(device,lighting_style(document));
    if(!program)return std::unexpected(program.error());
    return MeshResource{document, std::move(mesh), BlueprintMeshRenderer{std::move(*gpu),(*program)->instanced}};
}
resources::Result<MeshResource> realize_mesh(opengl::Device& device,
                                            MeshPrograms& programs,
                                            const content::vmesh::Document& document) {
    auto mesh = result(cpu_mesh(document));
    if (!mesh) return std::unexpected(mesh.error());
    return realize_mesh(device, programs, document, std::move(*mesh));
}
// CPU preparation is separate from writes: if a later asset fails to decode or
// allocate, previously usable buffers and their source topology are untouched.
struct MeshUpdate {
    MeshResource* previous{};
    const content::vmesh::Document* next{};
    std::size_t position_field{}, first{}, surface_first{};
    std::vector<Vertex> records;
    std::vector<Surface> surface_records;
    std::optional<Mesh> decoded;
    std::optional<content::vmesh::Document> source_document;
    std::optional<std::string> name;
    bool edges_changed{};
    std::optional<std::vector<gfx::Edge>> edges;
};
bool position_update(MeshUpdate& update) {
    const auto& next = *update.next;
    const auto& old = update.previous->source_document;
    if (next.vertex_count != old.vertex_count || next.faces != old.faces ||
        next.vertex_fields.size() != old.vertex_fields.size()) return false;
    for (std::size_t i = 0; i < next.vertex_fields.size(); ++i) {
        const auto& before = old.vertex_fields[i];
        const auto& after = next.vertex_fields[i];
        if (before.name != after.name || before.type != after.type) return false;
        if (after.name == "position") update.position_field = i;
        else if (before.values != after.values) return false;
    }
    const auto& before = std::get<std::vector<f32>>(old.vertex_fields[update.position_field].values);
    const auto& after = std::get<std::vector<f32>>(next.vertex_fields[update.position_field].values);
    auto first = next.vertex_count;
    std::size_t last{};
    for (std::size_t i = 0; i < next.vertex_count; ++i) {
        const auto at = i * 3;
        if (before[at] != after[at] || before[at + 1] != after[at + 1] ||
            before[at + 2] != after[at + 2]) {
            first = std::min(first, i);
            last = i + 1;
        }
    }
    update.first = first;
    if (first < last) {
        const auto& vertices = update.previous->mesh.vertices<Vertex>();
        update.records.assign(vertices.begin() + static_cast<std::ptrdiff_t>(first),
                              vertices.begin() + static_cast<std::ptrdiff_t>(last));
        for (std::size_t i = first; i < last; ++i)
            update.records[i - first].set(Position{}, Vec3{after[i * 3], after[i * 3 + 1], after[i * 3 + 2]});
    }
    if (old.metadata != next.metadata || old.edges != next.edges) {
        // Rare authored metadata changes may copy the document. Pointer drags
        // take the bounded-record path above and never copy the whole mesh.
        update.source_document = next;
        const auto name = next.metadata.find("name");
        update.name = name == next.metadata.end() ? std::string{} : name->second;
        update.edges_changed = true;
        update.edges = next.edges;
    }
    return true;
}
bool attribute_update(MeshUpdate& update) {
    const auto& next=*update.next;const auto& old=update.previous->source_document;
    if(next.vertex_count!=old.vertex_count || next.faces!=old.faces || next.vertex_fields.size()!=old.vertex_fields.size())return false;
    std::size_t first=next.vertex_count,last{},surface_first=next.vertex_count,surface_last{};
    for(std::size_t f=0;f<next.vertex_fields.size();++f) {
        const auto& a=old.vertex_fields[f];const auto& b=next.vertex_fields[f];
        if(a.name!=b.name || a.type!=b.type)return false;
        const bool vertex=b.name=="position" || b.name=="color/0";
        const bool surface=b.name=="normal" || b.name=="emission";
        if(!vertex && !surface)continue; // Authoring-only attributes have no GPU stream.
        const auto components=b.name=="color/0"?4:b.name=="emission"?1:3;
        if(b.type!=content::vmesh::FieldType{content::vmesh::ScalarType::Float32,static_cast<u8>(components)})return false;
        const auto& before=std::get<std::vector<f32>>(a.values);const auto& after=std::get<std::vector<f32>>(b.values);
        for(std::size_t i=0;i<next.vertex_count;++i)for(unsigned c=0;c<b.type.components;++c)if(before[i*b.type.components+c]!=after[i*b.type.components+c]) {
            if(vertex){first=std::min(first,i);last=i+1;}else{surface_first=std::min(surface_first,i);surface_last=i+1;}
            break;
        }
    }
    const auto values=[&](std::string_view name)->const std::vector<f32>* {
        auto f=std::ranges::find(next.vertex_fields,name,&content::vmesh::VertexField::name);
        return f==next.vertex_fields.end()?nullptr:&std::get<std::vector<f32>>(f->values);
    };
    if(first<last) {
        update.first=first;const auto& records=update.previous->mesh.vertices<Vertex>();
        update.records.assign(records.begin()+static_cast<std::ptrdiff_t>(first),records.begin()+static_cast<std::ptrdiff_t>(last));
        const auto* p=values("position");const auto* color=values("color/0");
        for(std::size_t i=first;i<last;++i) {
            if(p)update.records[i-first].set(Position{},Vec3{(*p)[i*3],(*p)[i*3+1],(*p)[i*3+2]});
            if(color)update.records[i-first].set(Color{},Vec4{(*color)[i*4],(*color)[i*4+1],(*color)[i*4+2],(*color)[i*4+3]});
        }
    }
    if(surface_first<surface_last) {
        update.surface_first=surface_first;const auto& records=update.previous->mesh.vertices<Surface>();
        update.surface_records.assign(records.begin()+static_cast<std::ptrdiff_t>(surface_first),records.begin()+static_cast<std::ptrdiff_t>(surface_last));
        const auto* normal=values("normal");const auto* emission=values("emission");
        for(std::size_t i=surface_first;i<surface_last;++i) {
            if(normal)update.surface_records[i-surface_first].set(Normal{},Vec3{(*normal)[i*3],(*normal)[i*3+1],(*normal)[i*3+2]});
            if(emission)update.surface_records[i-surface_first].set(Emission{},(*emission)[i]);
        }
    }
    update.source_document=next;
    const auto name=next.metadata.find("name");update.name=name==next.metadata.end()?std::string{}:name->second;
    update.edges_changed=old.edges!=next.edges;if(update.edges_changed)update.edges=next.edges;
    return true;
}
void commit_mesh_update(MeshUpdate& update) {
    auto& previous = *update.previous;
    if (update.decoded) {
        previous.mesh = std::move(*update.decoded);
    } else {
        std::copy(update.records.begin(), update.records.end(),
                  previous.mesh.vertices<Vertex>().begin() + static_cast<std::ptrdiff_t>(update.first));
        std::copy(update.surface_records.begin(),update.surface_records.end(),
                  previous.mesh.vertices<Surface>().begin()+static_cast<std::ptrdiff_t>(update.surface_first));
        if (update.name) previous.mesh.info().name = std::move(*update.name);
        if (update.edges_changed) previous.mesh.explicit_edges() = std::move(update.edges);
    }
    if (update.source_document) {
        previous.source_document = std::move(*update.source_document);
    } else if (!update.records.empty()) {
        auto& stored = std::get<std::vector<f32>>(
            previous.source_document.vertex_fields[update.position_field].values);
        const auto& next = std::get<std::vector<f32>>(
            update.next->vertex_fields[update.position_field].values);
        const auto first = static_cast<std::ptrdiff_t>(update.first * 3);
        const auto last = first + static_cast<std::ptrdiff_t>(update.records.size() * 3);
        std::copy(next.begin() + first, next.begin() + last, stored.begin() + first);
    }
}
gfx::ImageData diagnostic_image(const analysis::FrameEvidence& evidence) {
    const auto extent = evidence.capture().extent();
    gfx::ImageData out{{extent.width, extent.height}, {}};
    out.pixels.resize(static_cast<std::size_t>(extent.width) * extent.height * 4);
    for (u32 y = 0; y < extent.height; ++y)
        for (u32 x = 0; x < extent.width; ++x) {
            const auto hit = evidence.capture().surface_at({x, y});
            const auto i = (static_cast<std::size_t>(y) * extent.width + x) * 4;
            const auto face = hit ? hit->face + 1 : 0;
            out.pixels[i] = std::byte(face ? 50 + (face * 97) % 206 : 12);
            out.pixels[i + 1] = std::byte(face ? 40 + (face * 53) % 216 : 16);
            out.pixels[i + 2] = std::byte(face ? 60 + (face * 137) % 196 : 26);
            out.pixels[i + 3] = std::byte{255};
        }
    return out;
}
} // namespace
struct Runtime::Impl {
    // Stable program addresses; blueprint renderers are destroyed before them.
    MeshPrograms mesh_programs;
    std::map<BlueprintId, MeshResource> meshes;
    example::sun::SunRenderer sun;
    opengl::RenderTarget hdr, soft;
    example::sun::SunSoftening softening;
    opengl::Bloom bloom;
    example::DisplaySurface display;
    std::string diagnostic;
    RuntimeStats stats{};
    std::optional<analysis::AnalysisManifest> manifest;
    std::optional<opengl::RenderTarget> preview_target;
    std::optional<opengl::Rgba8ReadbackQueue> readback_queue;
    // Borrows softened color and the original sun depth. Mesh rendering can
    // then stay sharp without losing occlusion or copying another HDR image.
    std::optional<opengl::Framebuffer> composite;
    bool frame_ready{};
    std::optional<Starfield> stars;
    u32 star_count{}, star_seed{};
    MeshVisibility mesh_visibility;
    u64 visibility_revision{1};
    SceneSamples samples;
    std::optional<SceneAnnotationRenderer> annotations;
    std::optional<opengl::Framebuffer> annotation_target;
    std::vector<SceneLine> annotation_lines;
    std::vector<SceneTriangle> annotation_triangles;
    u64 annotation_revision{};
    f32 annotation_time{-1};
    u32 annotation_selection{}, annotation_flags{};
};
Runtime::Runtime(std::unique_ptr<Impl> p) : impl_(std::move(p)) {
}
Runtime::Runtime(Runtime&&) noexcept = default;
Runtime& Runtime::operator=(Runtime&&) noexcept = default;
Runtime::~Runtime() = default;
void Runtime::mesh_visibility(MeshVisibility value) {
    if(impl_->mesh_visibility==value) return;
    impl_->mesh_visibility=std::move(value);++impl_->visibility_revision;
}
resources::Result<Runtime> Runtime::create(opengl::Device& device, const State& state) {
    MeshPrograms mesh_programs;
    std::map<BlueprintId, MeshResource> meshes;
    for (const auto& blueprint : blueprint_catalog(state)) {
        if (blueprint.kind != BlueprintKind::mesh) continue;
        const auto* geometry = mesh_view_geometry(state, blueprint.id);
        if (!geometry || meshes.contains(blueprint.id)) return invalid("Invalid mesh blueprint catalog");
        auto resource = realize_mesh(device, mesh_programs, geometry->document());
        if (!resource) return std::unexpected(resource.error());
        meshes.emplace(blueprint.id, std::move(*resource));
    }
    auto sun = example::sun::SunRenderer::create(device);
    if (!sun)
        return std::unexpected(sun.error());
    constexpr Extent2D extent{640, 480};
    auto hdr = providers::render_target().provide(device, extent);
    if (!hdr)
        return std::unexpected(hdr.error());
    auto soft = providers::render_target({.depth = false}).provide(device, extent);
    if (!soft)
        return std::unexpected(soft.error());
    auto soften = example::sun::SunSoftening::create(device);
    if (!soften)
        return std::unexpected(soften.error());
    auto bloom = render::bloom_builder(device).levels(6).build(extent);
    if (!bloom)
        return std::unexpected(bloom.error());
    auto display = example::DisplaySurface::create(device, extent);
    if (!display)
        return std::unexpected(display.error());
    const auto count = meshes.size();
    return Runtime{std::make_unique<Impl>(std::move(mesh_programs), std::move(meshes), std::move(*sun),
                                          std::move(*hdr), std::move(*soft), std::move(*soften),
                                          std::move(*bloom), std::move(*display), std::string{},
                                          RuntimeStats{.full_mesh_uploads = count, .resident_meshes = count})};
}
resources::Result<void> Runtime::update_mesh(opengl::Device& device, const State& state, std::span<const BlueprintId> only) {
    if (!impl_)
        return invalid("Moved-from editor runtime");
    Timing timer{impl_->stats.last_mesh_update_ms};
    auto& p = *impl_;
    if (!p.display.target().belongs_to(device))
        return invalid("Mesh updates require the runtime's creating device");
    if (auto allowed = result(device.require_resource_update("Editor mesh update")); !allowed)
        return std::unexpected(allowed.error());
    std::map<BlueprintId, MeshResource> replacements;
    std::vector<MeshUpdate> updates;
    const auto catalog = blueprint_catalog(state);
    std::vector<BlueprintId> seen;
    seen.reserve(catalog.size());
    updates.reserve(catalog.size());
    for (const auto& blueprint : catalog) {
        if (blueprint.kind != BlueprintKind::mesh) continue;
        if(!only.empty() && std::ranges::find(only,blueprint.id)==only.end())continue;
        if (std::ranges::find(seen, blueprint.id) != seen.end()) return invalid("Duplicate mesh blueprint identity");
        seen.push_back(blueprint.id);
        const auto* geometry = mesh_view_geometry(state, blueprint.id);
        if (!geometry) return invalid("Missing mesh blueprint geometry");
        const auto& next = geometry->document();
        const auto cached = p.meshes.find(blueprint.id);
        if (cached == p.meshes.end() || lighting_style(cached->second.source_document)!=lighting_style(next)) {
            auto resource = realize_mesh(device, p.mesh_programs, next);
            if (!resource) return std::unexpected(resource.error());
            replacements.emplace(blueprint.id, std::move(*resource));
            continue;
        }
        auto& previous = cached->second;
        if (previous.source_document == next) continue;
        MeshUpdate update{};
        update.previous = &previous;
        update.next = &next;
        if (position_update(update) || attribute_update(update)) {
            updates.push_back(std::move(update));
            continue;
        }
        auto source = result(cpu_mesh(next));
        if (!source) return std::unexpected(source.error());
        if (source->vertex_count() == previous.mesh.vertex_count() &&
            source->faces() == previous.mesh.faces()) {
            update.records.assign(source->vertices<Vertex>().begin(), source->vertices<Vertex>().end());
            if (!std::ranges::equal(source->vertices<Surface>().bytes(), previous.mesh.vertices<Surface>().bytes()))
                update.surface_records.assign(source->vertices<Surface>().begin(), source->vertices<Surface>().end());
            update.decoded = std::move(*source);
            update.source_document = next;
            updates.push_back(std::move(update));
        } else {
            auto resource = realize_mesh(device, p.mesh_programs, next, std::move(*source));
            if (!resource) return std::unexpected(resource.error());
            replacements.emplace(blueprint.id, std::move(*resource));
        }
    }
    for (std::size_t i = 0; i < updates.size(); ++i) {
        auto& update = updates[i];
        if (update.records.empty() && update.surface_records.empty()) continue;
        auto uploaded = update.records.empty()?resources::Result<void>{}:result(update.previous->renderer.mesh().update_vertices(
            device, std::span<const Vertex>{update.records}, update.first));
        if (uploaded && !update.surface_records.empty())
            uploaded = result(update.previous->renderer.mesh().update_vertices(
                device, std::span<const Surface>{update.surface_records},update.surface_first));
        if (!uploaded) {
            auto error = std::move(uploaded.error());
            // CPU mirrors still contain the old values. Restore every attempted
            // write before returning a rejected multi-asset snapshot.
            for (std::size_t rollback = 0; rollback <= i; ++rollback) {
                auto& old = updates[rollback];
                if (old.records.empty() && old.surface_records.empty()) continue;
                const std::span<const Vertex> records{old.previous->mesh.vertices<Vertex>().data() + old.first,
                                                       old.records.size()};
                if (!records.empty())if (auto restored = old.previous->renderer.mesh().update_vertices(device, records, old.first); !restored)
                    error.context.push_back("Mesh rollback failed: " + restored.error().message);
                if (!old.surface_records.empty())
                    if (auto restored = old.previous->renderer.mesh().update_vertices(device,
                            std::span<const Surface>{old.previous->mesh.vertices<Surface>().data()+old.surface_first,old.surface_records.size()},old.surface_first); !restored)
                        error.context.push_back("Surface rollback failed: " + restored.error().message);
            }
            return std::unexpected(std::move(error));
        }
    }
    for (auto& update : updates) {
        if (!update.records.empty() || !update.surface_records.empty()) {
            ++p.stats.vertex_updates;
            p.stats.vertex_bytes_uploaded += update.records.size() * Vertex::stride;
            p.stats.vertex_bytes_uploaded += update.surface_records.size() * Surface::stride;
        }
        commit_mesh_update(update);
    }
    p.stats.full_mesh_uploads += replacements.size();
    for (auto it = replacements.begin(); it != replacements.end();) {
        const auto old = p.meshes.find(it->first);
        if (old == p.meshes.end()) { ++it; continue; }
        old->second = std::move(it->second);
        it = replacements.erase(it);
    }
    p.meshes.merge(replacements); // Transfers preallocated nodes, no allocation at commit.
    if(only.empty())std::erase_if(p.meshes, [&](const auto& item) { return !mesh_geometry(state, item.first); });
    p.stats.resident_meshes = p.meshes.size();
    return {};
}
resources::Result<void> Runtime::update_positions(opengl::Device& device, BlueprintId blueprint,
                                                 std::span<const VertexPosition> changes) {
    if (!impl_) return invalid("Moved-from editor runtime");
    auto& p=*impl_;
    Timing timer{p.stats.last_mesh_update_ms};
    auto found=p.meshes.find(blueprint);
    if (found==p.meshes.end()) return invalid("Position patch refers to an unrealized blueprint");
    auto& resource=found->second;
    auto& vertices=resource.mesh.vertices<Vertex>();
    auto field=std::ranges::find(resource.source_document.vertex_fields,std::string{"position"},&content::vmesh::VertexField::name);
    if (field==resource.source_document.vertex_fields.end()) return invalid("Position field is missing");
    auto* stored=std::get_if<std::vector<f32>>(&field->values);
    if (!stored || stored->size()!=vertices.size()*3) return invalid("Invalid realized position layout");
    std::vector<VertexPosition> sorted(changes.begin(),changes.end());
    std::ranges::sort(sorted,{},&VertexPosition::index);
    struct Range { std::size_t first; std::vector<Vertex> values; };
    std::vector<Range> ranges;
    for (std::size_t i=0;i<sorted.size();++i) {
        const auto& change=sorted[i];
        if (change.index>=vertices.size() || (i && sorted[i-1].index==change.index) ||
            !std::isfinite(change.position.x) || !std::isfinite(change.position.y) || !std::isfinite(change.position.z))
            return invalid("Invalid or duplicate position patch index/value");
        if (ranges.empty() || ranges.back().first+ranges.back().values.size()!=change.index)
            ranges.push_back({change.index,{}});
        auto record=vertices[change.index];
        record.set(Position{},change.position);
        ranges.back().values.push_back(record);
    }
    for (std::size_t i=0;i<ranges.size();++i) {
        const auto& range=ranges[i];
        auto uploaded=result(resource.renderer.mesh().update_vertices(device,std::span<const Vertex>{range.values},range.first));
        if (!uploaded) {
            auto error=std::move(uploaded.error());
            for (std::size_t j=0;j<=i;++j) {
                const auto& old=ranges[j];
                if (auto restored=resource.renderer.mesh().update_vertices(device,
                        std::span<const Vertex>{vertices.data()+old.first,old.values.size()},old.first); !restored)
                    error.context.push_back("Position rollback failed: " + restored.error().message);
            }
            return std::unexpected(std::move(error));
        }
    }
    for (const auto& range:ranges) {
        std::copy(range.values.begin(),range.values.end(),vertices.begin()+static_cast<std::ptrdiff_t>(range.first));
        ++p.stats.vertex_updates;
        p.stats.vertex_bytes_uploaded+=range.values.size()*Vertex::stride;
    }
    for (const auto& change:sorted)
        for (std::size_t axis=0;axis<3;++axis) (*stored)[change.index*3+axis]=change.position[axis];
    return {};
}
resources::Result<std::optional<gfx::ImageData>> Runtime::draw(opengl::Device& device,
                                                               const State& state, const gfx::Camera& camera, Extent2D extent,
                                                               bool diagnostic,
                                                               std::optional<f32> time, bool annotations) {
    if (!impl_)
        return invalid("Moved-from editor runtime");
    auto& p = *impl_;
    Timing timer{p.stats.last_render_ms};
    p.frame_ready = false;
    p.manifest.reset();
    if (time && !std::isfinite(*time))
        return invalid("Preview time override must be finite");
    const auto sampled_time = time.value_or(state.viewport.time);
    std::span<const SceneInstance> values;
    if (state.viewport.mode != ViewMode::mesh) {
        values = p.samples.sample(state, sampled_time);
    }
    p.stats.sampled_instances = p.samples.evaluations();
    // Scene mode already sampled its selected instance. Avoid an additional
    // round of timeline lookups just to obtain diagnostic metadata.
    std::optional<MeshPreview> selected_mesh;
    if (state.viewport.mode == ViewMode::mesh) selected_mesh = preview_mesh(state, sampled_time);
    else if (const auto found = std::ranges::find(values, state.viewport.selected_object, &SceneInstance::id);
             found != values.end()) {
        if (const auto* settings = std::get_if<MeshSettings>(&found->settings); settings && mesh_target(state))
            selected_mesh = MeshPreview{{found->blueprint, found->id}, *settings, found->transform};
    }
    const auto hidden_object=annotations && state.viewport.hides_surface(state.viewport.selected_object)
        ? state.viewport.selected_object : 0U;
    const auto render_instance=[&](const SceneInstance& instance) {
        return instance.id!=hidden_object && instance_in_view(state,instance);
    };
    if(selected_mesh && selected_mesh->target.instance==hidden_object)selected_mesh.reset();
    std::vector<MeshPreview> mesh_draws;
    if (state.viewport.mode == ViewMode::mesh) {
        if (selected_mesh) mesh_draws.push_back(*selected_mesh);
    } else {
        mesh_draws.reserve(values.size());
        for (const auto& instance : values)
            if (const auto* settings = std::get_if<MeshSettings>(&instance.settings);
                settings && render_instance(instance))
                mesh_draws.push_back({{instance.blueprint, instance.id}, *settings, instance.transform});
    }
    const bool sun_visible =
        std::ranges::any_of(values, [&](const auto& instance) {
            const auto* sun = std::get_if<SunSettings>(&instance.settings);
            return sun && sun->visible && render_instance(instance);
        });
    const auto selected = std::ranges::find(values, state.viewport.selected_object, &SceneInstance::id);
    const auto* selected_sun = selected != values.end() && selected->id!=hidden_object &&
        std::holds_alternative<SunSettings>(selected->settings) ? &*selected : nullptr;
    const bool captures_surface = diagnostic && (selected_mesh || selected_sun);
    for (const auto& mesh : mesh_draws)
        if (!p.meshes.contains(mesh.target.blueprint))
            return invalid("Mesh blueprint is not uploaded; update the runtime after importing geometry");
    // Only a mask/view/topology change touches the element buffer. Camera
    // movement and vertex drags do not scan or rebuild geometry for visibility.
    for(const auto& mesh:mesh_draws) {
        auto& resource=p.meshes.at(mesh.target.blueprint);
        const auto& visibility=p.mesh_visibility;
        const bool masked=state.viewport.mode==ViewMode::mesh && visibility.blueprint==mesh.target.blueprint &&
            visibility.topology==resource.renderer.mesh().topology_fingerprint() && !visibility.hidden_faces.empty();
        if(masked!=resource.masked || (masked && resource.visibility_revision!=p.visibility_revision)) {
            if(auto updated=result(resource.renderer.mesh().set_hidden_faces(device,resource.mesh,
                masked?std::span<const u32>{visibility.hidden_faces}:std::span<const u32>{}));!updated)
                return std::unexpected(updated.error());
            resource.masked=masked;
        }
        resource.visibility_revision=p.visibility_revision;
    }
    const auto& environment = state.document.environment;
    f32 bloom_strength = environment.bloom_strength;
    bool color_fallback{};
    if (p.hdr.extent() != extent || p.soft.extent() != extent) {
        p.composite.reset(); // Release borrowed attachments before their storage.
        p.annotation_target.reset();
    }
    if (p.hdr.extent() != extent)
        if (auto v = p.hdr.resize(device, extent); !v)
            return std::unexpected(v.error());
    if (p.soft.extent() != extent)
        if (auto v = p.soft.resize(device, extent); !v)
            return std::unexpected(v.error());
    if (p.bloom.extent() != extent)
        if (auto v = p.bloom.resize(device, extent); !v)
            return std::unexpected(v.error());
    if (p.display.target().extent() != extent)
        if (auto v = p.display.resize(device, extent); !v)
            return std::unexpected(v.error());
    const bool stars_visible = state.viewport.mode == ViewMode::scene && environment.stars > 0;
    if (stars_visible && (!p.stars || p.star_count != environment.stars ||
                         p.star_seed != environment.star_seed)) {
        auto stars = Starfield::create(device, environment.stars, environment.star_seed);
        if (!stars) return std::unexpected(stars.error());
        p.stars = std::move(*stars);
        p.star_count = environment.stars;
        p.star_seed = environment.star_seed;
    }
    if (sun_visible && !captures_surface && !p.composite) {
        auto composite = result(opengl::Framebuffer::create(device));
        if (!composite)
            return std::unexpected(composite.error());
        if (auto v = result(composite->attach_color(0, p.soft.color())); !v)
            return std::unexpected(v.error());
        if (auto v = result(composite->attach_depth(*p.hdr.depth())); !v)
            return std::unexpected(v.error());
        if (auto v = result(composite->check_complete()); !v)
            return std::unexpected(v.error());
        p.composite = std::move(*composite);
    }
    auto view = result(render::RenderView::create(camera, extent));
    if (!view)
        return std::unexpected(view.error());
    std::vector<Vec3> sun_positions;
    for (const auto& instance : values) {
        const auto* sun = std::get_if<SunSettings>(&instance.settings);
        if (sun && sun->visible && instance_in_view(state, instance))
            sun_positions.push_back(instance.transform.position);
    }
    const auto mesh_lighting = [&](const MeshPreview& mesh) {
        Lighting lights;
        lights.set(Eye{}, view->camera()->position);
        lights.set(Brightness{}, mesh.settings.brightness);
        // Blueprint inspection must expose authored colors from every angle.
        // This camera light is private to the asset view; scene lighting still
        // follows its sun and does not change when the editor camera moves.
        if (state.viewport.mode == ViewMode::mesh) {
            const auto eye = view->camera()->position;
            lights.set(Light{}, Vec4{eye.x, eye.y, eye.z, 1});
            return lights;
        }
        Vec4 light{-.35F, .8F, .65F, 0}; // Directional inspection light without a visible sun.
        f32 nearest = std::numeric_limits<f32>::max();
        for (const auto position : sun_positions) {
            ++p.stats.light_candidates;
            const auto origin = mesh.transform.position;
            const auto dx = position.x - origin.x, dy = position.y - origin.y,
                       dz = position.z - origin.z;
            const auto distance = dx*dx + dy*dy + dz*dz;
            if (distance < nearest) {
                nearest = distance;
                light = {position.x, position.y, position.z, 1};
            }
        }
        lights.set(Light{}, light);
        return lights;
    };
    auto frame =
        result(render::begin_frame(device, p.hdr,
                                   {.extent = extent,
                                    .color_encoding = render::ColorEncoding::linear,
                                    .clear_color = std::array<f32, 4>{.001F, .002F, .005F, 1},
                                    .clear_depth = 1}));
    if (!frame)
        return std::unexpected(frame.error());
    const auto sun_draw = [&](const SceneInstance& instance) {
        const auto& sun = std::get<SunSettings>(instance.settings);
        const auto placement = state.viewport.mode == ViewMode::sun ? InstanceTransform{} : instance.transform;
        const auto transform = mesh_transform(ViewMode::scene, placement);
        Mat3 orientation{};
        for (std::size_t col = 0; col < 3; ++col)
            for (std::size_t row = 0; row < 3; ++row)
                orientation[col][row] = transform[col][row] / (placement.scale*placement.axis_scale[col]);
        return example::sun::SunDraw{.time = sampled_time,
                                     .displacement = sun.displacement,
                                     .white_spots = sun.white_spots,
                                     .position = placement.position,
                                     .radius = sun.radius * placement.scale,
                                     .orientation = orientation,
                                     .axis_scale = placement.axis_scale};
    };
    std::vector<example::sun::SunDraw> sun_tickets;
    for (const auto& instance : values) {
        const auto* sun = std::get_if<SunSettings>(&instance.settings);
        if (!sun || !sun->visible || !render_instance(instance)) continue;
        bloom_strength = std::max(bloom_strength, sun->bloom);
        sun_tickets.push_back(sun_draw(instance));
    }
    if(auto v=p.sun.render(*frame,*view,std::span<const example::sun::SunDraw>{sun_tickets});!v)
        return std::unexpected(v.error());
    if (sun_visible && !captures_surface) {
        if (auto v = result(frame->end()); !v)
            return std::unexpected(v.error());
        frame = result(render::begin_frame(device, *p.composite,
                                           {.extent = extent,
                                            .color_encoding = render::ColorEncoding::linear,
                                            .clear_color = {},
                                            .clear_depth = {}}));
        if (!frame)
            return std::unexpected(frame.error());
        // Only the sun's fine surface/strand detail receives the intentional
        // filter. Softening disables depth writes; the next opaque draw uses
        // the original sun depth and unfiltered mesh edges.
        if (auto v = p.softening.apply(*frame, p.hdr.color()); !v)
            return std::unexpected(v.error());
    }
    if (stars_visible && !captures_surface)
        if (auto v = p.stars->render(*frame, *view); !v)
            return std::unexpected(v.error());
    std::map<BlueprintId,std::vector<MeshDraw>> tickets;
    for(const auto& mesh:mesh_draws) {
        if(!mesh.settings.visible)continue;
        tickets[mesh.target.blueprint].push_back({
            mesh_transform(state,mesh.target.blueprint,mesh.transform),mesh_lighting(mesh),mesh.settings.wireframe});
    }
    p.stats.last_mesh_draw_calls=0;
    p.stats.last_mesh_renderer_calls=0;
    p.stats.last_mesh_instances=0;
    for(auto& [blueprint,batch]:tickets) {
        auto& resource=p.meshes.at(blueprint);
        color_fallback |= ignored_color(resource.source_document);
        if(auto drawn=resource.renderer.render(*frame,*view,batch);!drawn)
            return std::unexpected(drawn.error());
        ++p.stats.last_mesh_renderer_calls;
        p.stats.last_mesh_draw_calls+=resource.renderer.stats().draw_calls;
        p.stats.last_mesh_instances+=resource.renderer.stats().instances;
    }
    if (captures_surface) {
        if (selected_sun) {
            auto sweep =
                p.sun.diagnose(*frame, *view, sun_draw(*selected_sun), analysis::CaptureRequest::diagnostic());
            if (!sweep)
                return std::unexpected(sweep.error());
            p.diagnostic = "Sun surface / pre-bloom: " +
                           std::to_string(sweep->production().summary().covered_pixel_count) +
                           " covered pixels / " + selected_sun->name + " #" + std::to_string(selected_sun->id);
            const auto& manifest = sweep->production().capture().manifest();
            p.manifest.emplace(manifest.frame());
            for (const auto& item : manifest.items()) {
                auto provenance = item.provenance;
                provenance.entity = analysis::EntityId{selected_sun->id};
                provenance.mesh = analysis::MeshAssetId{static_cast<u32>(selected_sun->blueprint)};
                provenance.mesh_revision = analysis::AssetRevision{state.document.revision};
                (void)p.manifest->add(std::move(provenance), item.primitives);
            }
            auto image = diagnostic_image(sweep->production());
            if (auto v = result(frame->end()); !v)
                return std::unexpected(v.error());
            ++p.stats.readbacks;
            ++p.stats.rendered_frames;
            return std::optional<gfx::ImageData>{std::move(image)};
        }
        const auto& target = selected_mesh->target;
        auto& resource = p.meshes.at(target.blueprint);
        auto& diagnostic_program=p.mesh_programs.at(lighting_style(resource.source_document)).diagnostic;
        const auto transform = mesh_transform(state,target.blueprint,selected_mesh->transform);
        if (auto v = result(diagnostic_program.set_arguments(transform,
                                                    mesh_lighting(*selected_mesh)));
            !v)
            return std::unexpected(v.error());
        render::AnalysisOptions analysis_options;
        if (target.instance) {
            analysis_options.label = object_name(state, *target.instance);
            analysis_options.provenance.entity = analysis::EntityId{*target.instance};
        } else {
            const auto catalog = blueprint_catalog(state);
            const auto asset = std::ranges::find(catalog, target.blueprint, &Blueprint::id);
            analysis_options.label = asset == catalog.end() ? "Mesh blueprint" : std::string(asset->name);
        }
        const auto label = analysis_options.label;
        analysis_options.provenance.mesh = analysis::MeshAssetId{static_cast<u32>(target.blueprint)};
        // Scene revisions cover this source asset snapshot too; conservative
        // invalidation is preferable to presenting stale topology as current.
        analysis_options.provenance.mesh_revision = analysis::AssetRevision{state.document.revision};
        analysis_options.provenance.object_to_world = transform;
        const opengl::GraphicsStateSnapshot diagnostic_raster{
                .depth = {true, true, render::DepthCompare::less}, .cull = render::CullMode::none,
                .front_face = render::FrontFace::counter_clockwise, .blend = render::BlendMode::disabled,
                .polygon = opengl::PolygonMode::fill};
        auto evidence = result(diagnostic_program.capture(device, resource.mesh, resource.renderer.mesh(), *view,
                                                 opengl::CaptureState{diagnostic_raster, frame->color_encoding()},
                                                 analysis::CaptureRequest::diagnostic(),
                                                 std::move(analysis_options)));
        if (!evidence)
            return std::unexpected(evidence.error());
        p.manifest = evidence->capture().manifest();
        p.diagnostic = "Mesh source faces / isolated solid draw: " +
                       std::to_string(evidence->summary().covered_pixel_count) + " covered pixels / " +
                       label + (target.instance ? " #" + std::to_string(*target.instance) : "") +
                       " / blueprint #" + std::to_string(static_cast<u32>(target.blueprint));
        if (ignored_color(resource.source_document))
            p.diagnostic += " / optional color/0 ignored; preview uses white";
        auto image = diagnostic_image(*evidence);
        if (auto v = result(frame->end()); !v)
            return std::unexpected(v.error());
        ++p.stats.readbacks;
        ++p.stats.rendered_frames;
        return std::optional<gfx::ImageData>{std::move(image)};
    }
    if (auto v = result(frame->end()); !v)
        return std::unexpected(v.error());
    const bool decorate=annotations && state.viewport.mode==ViewMode::scene &&
        (state.viewport.show_regions||state.viewport.show_world_bounds||has_camera(state));
    if(decorate) {
        if(!p.annotations) {
            auto renderer=SceneAnnotationRenderer::create(device);
            if(!renderer)return std::unexpected(resources::to_diagnostic(renderer.error()));
            p.annotations=std::move(*renderer);
        }
        if(!p.annotation_target) {
            auto target=result(opengl::Framebuffer::create(device));
            if(!target)return std::unexpected(target.error());
            if(auto r=result(target->attach_color(0,p.display.target().color()));!r)return std::unexpected(r.error());
            if(auto r=result(target->attach_depth(*p.hdr.depth()));!r)return std::unexpected(r.error());
            if(auto r=result(target->check_complete());!r)return std::unexpected(r.error());
            p.annotation_target=std::move(*target);
        }
        const u32 flags=static_cast<u32>(state.viewport.show_regions)|(static_cast<u32>(state.viewport.show_world_bounds)<<1)|
            (static_cast<u32>(state.viewport.gizmo_only)<<2);
        if(p.annotation_revision!=state.document.revision || p.annotation_time!=sampled_time ||
           p.annotation_flags!=flags || p.annotation_selection!=state.viewport.selected_object) {
            p.annotation_lines=scene_annotation_lines(state,sampled_time,hidden_object);
            p.annotation_triangles=scene_annotation_triangles(state,sampled_time,hidden_object);
            p.annotation_revision=state.document.revision;p.annotation_time=sampled_time;
            p.annotation_flags=flags;p.annotation_selection=state.viewport.selected_object;
        }
    }
    // Reuse original scene depth with final encoded color. No depth readback,
    // duplicate mesh pass, translucency or bloom on editor annotations.
    auto output = result(render::begin_frame(device, decorate ? *p.annotation_target : p.display.target().framebuffer(),
                                             {.extent = extent,
                                              .color_encoding = render::ColorEncoding::srgb,
                                              .clear_color = {},
                                              .clear_depth = {}}));
    if (!output)
        return std::unexpected(output.error());
    if (auto v = p.bloom.apply(
            *output, sun_visible ? p.soft.color() : p.hdr.color(),
            {.threshold = environment.bloom_threshold, .strength = bloom_strength,
             .exposure = environment.exposure});
        !v)
        return std::unexpected(v.error());
    if(decorate)
        if(auto r=result(p.annotations->render(*output,*view,p.annotation_lines,p.annotation_triangles));!r)return std::unexpected(r.error());
    if (auto v = result(output->end()); !v)
        return std::unexpected(v.error());
    ++p.stats.rendered_frames;
    p.frame_ready = true;
    p.diagnostic = sun_visible ? "Final sRGB / same sun shaders + HDR softening + bloom"
                               : "Final sRGB / mesh + tone mapping";
    if (color_fallback) p.diagnostic += " / optional color/0 ignored; preview uses white";
    return std::optional<gfx::ImageData>{};
}
resources::Result<void> Runtime::render_frame(opengl::Device& device, const State& state,
                                              Extent2D extent, std::optional<f32> time) {
    auto rendered = draw(device, state, camera(preview_camera_pose(state, time.value_or(state.viewport.time)),
                                               state.viewport.mode), extent, false, time);
    if (!rendered)
        return std::unexpected(rendered.error());
    return {};
}
resources::Result<gfx::ImageData> Runtime::readback(opengl::Device& device) {
    if (!impl_)
        return invalid("Moved-from editor runtime");
    return readback(device, impl_->display.target().extent());
}
resources::Result<const opengl::Framebuffer*> Runtime::prepare_readback(opengl::Device& device, Extent2D extent) {
    if (!impl_ || !impl_->frame_ready)
        return invalid("Render a normal frame before requesting preview readback");
    auto& p = *impl_;
    if (auto allowed = result(device.require_resource_update("Editor preview readback")); !allowed)
        return std::unexpected(allowed.error());
    if (!p.display.target().belongs_to(device))
        return invalid("Preview readback requires its creating device");
    if (extent.empty() || extent.width > 8192 || extent.height > 8192 ||
        u64(extent.width) * extent.height > 16 * 1024 * 1024)
        return invalid("Invalid preview readback extent");
    const opengl::Framebuffer* framebuffer = &p.display.target().framebuffer();
    const auto source_extent = p.display.target().extent();
    if (extent != source_extent) {
        if (!p.preview_target) {
            auto target = opengl::RenderTarget::create(
                device, {.color = gfx::ImageFormat::rgba8, .depth = false}, extent);
            if (!target)
                return std::unexpected(resources::to_diagnostic(target.error()));
            p.preview_target = std::move(*target);
        } else if (p.preview_target->extent() != extent) {
            if (auto resized = p.preview_target->resize(device, extent); !resized)
                return std::unexpected(resized.error());
        }
        auto scope = result(opengl::RenderStateScope::capture(device));
        if (!scope)
            return std::unexpected(scope.error());
        if (auto v = result(device.set_scissor_enabled(false)); !v)
            return std::unexpected(v.error());
        // Copy already-encoded display bytes; do not apply sRGB conversion a second time.
        if (auto v = result(device.set_framebuffer_srgb_enabled(false)); !v)
            return std::unexpected(v.error());
        glBlitNamedFramebuffer(
            framebuffer->native_handle(), p.preview_target->framebuffer().native_handle(), 0, 0,
            static_cast<GLint>(source_extent.width), static_cast<GLint>(source_extent.height), 0, 0,
            static_cast<GLint>(extent.width), static_cast<GLint>(extent.height),
            GL_COLOR_BUFFER_BIT, GL_LINEAR);
        if (const auto error = glGetError(); error != GL_NO_ERROR)
            return invalid("Preview GPU rescale failed: OpenGL error " + std::to_string(error));
        if (auto restored = result(scope->restore()); !restored)
            return std::unexpected(restored.error());
        framebuffer = &p.preview_target->framebuffer();
        ++p.stats.preview_rescales;
    }
    return framebuffer;
}
bool Runtime::readback_available() const {
    return impl_ && (!impl_->readback_queue || impl_->readback_queue->available());
}
bool Runtime::readback_pending() const {
    return impl_ && impl_->readback_queue && impl_->readback_queue->pending();
}
void Runtime::discard_readbacks() {
    if(impl_ && impl_->readback_queue) impl_->readback_queue->discard();
}
resources::Result<bool> Runtime::queue_readback(opengl::Device& device,Extent2D extent,u64 id) {
    if(!impl_) return invalid("Moved-from editor runtime");
    if(!readback_available()) return false;
    auto framebuffer=prepare_readback(device,extent);
    if(!framebuffer) return std::unexpected(framebuffer.error());
    if(!impl_->readback_queue) {
        auto queue=result(opengl::Rgba8ReadbackQueue::create(device));
        if(!queue) return std::unexpected(queue.error());
        impl_->readback_queue=std::move(*queue);
    }
    auto submitted=result(impl_->readback_queue->try_submit(**framebuffer,0,extent,id));
    if(submitted && *submitted) ++impl_->stats.readbacks;
    return submitted;
}
resources::Result<std::optional<opengl::Rgba8Readback>> Runtime::poll_readback() {
    if(!impl_) return invalid("Moved-from editor runtime");
    if(!impl_->readback_queue) return std::optional<opengl::Rgba8Readback>{};
    Timing timer{impl_->stats.last_readback_ms};
    auto completed=result(impl_->readback_queue->try_take());
    if(!completed) impl_->readback_queue.reset();
    return completed;
}
resources::Result<gfx::ImageData> Runtime::readback(opengl::Device& device, Extent2D extent) {
    if(!impl_) return invalid("Moved-from editor runtime");
    auto& p=*impl_;
    Timing timer{p.stats.last_readback_ms};
    auto framebuffer=prepare_readback(device,extent);
    if(!framebuffer) return std::unexpected(framebuffer.error());
    auto pixels = result((*framebuffer)->read_rgba8(0, 0, 0, extent.width, extent.height));
    if (!pixels)
        return std::unexpected(pixels.error());
    const auto row = static_cast<std::size_t>(extent.width) * 4;
    // Flip the returned allocation in place; the old path allocated/copied a second image.
    std::vector<std::byte> scratch(row);
    for (u32 y = 0; y < extent.height / 2; ++y) {
        auto* top = pixels->data() + static_cast<std::size_t>(y) * row;
        auto* bottom = pixels->data() + static_cast<std::size_t>(extent.height - 1 - y) * row;
        std::memcpy(scratch.data(), top, row);
        std::memcpy(top, bottom, row);
        std::memcpy(bottom, scratch.data(), row);
    }
    gfx::ImageData image{extent, std::move(*pixels)};
    ++p.stats.readbacks;
    return image;
}
resources::Result<void> Runtime::present(opengl::Device& device) {
    if (!impl_ || !impl_->frame_ready)
        return invalid("Render a normal frame before presenting it");
    Timing timer{impl_->stats.last_present_ms};
    auto presented = impl_->display.copy_to_window(device);
    if (presented)
        ++impl_->stats.presented_frames;
    return presented;
}
resources::Result<gfx::ImageData> Runtime::render(opengl::Device& device, const State& state,
                                                  Extent2D extent, bool diagnostic,
                                                  std::optional<f32> time) {
    const auto sampled_time = time.value_or(state.viewport.time);
    return render(device,RenderRequest{state,camera(preview_camera_pose(state, sampled_time), state.viewport.mode),
                                      extent,sampled_time,diagnostic});
}
resources::Result<void> Runtime::render_frame(opengl::Device& device, const RenderRequest& request) {
    auto rendered=draw(device,request.state,request.camera,request.extent,false,request.time,request.annotations);
    if (!rendered) return std::unexpected(rendered.error());
    return {};
}
resources::Result<gfx::ImageData> Runtime::render(opengl::Device& device, const RenderRequest& request) {
    auto rendered = draw(device, request.state, request.camera, request.extent, request.diagnostic, request.time, request.annotations);
    if (!rendered) {
        if (impl_) impl_->manifest.reset();
        return std::unexpected(rendered.error());
    }
    if (*rendered)
        return std::move(**rendered);
    return readback(device);
}
std::string_view Runtime::diagnostics() const {
    return impl_ ? std::string_view{impl_->diagnostic} : std::string_view{};
}
const analysis::AnalysisManifest* Runtime::diagnostic_manifest() const {
    return impl_ && impl_->manifest ? &*impl_->manifest : nullptr;
}
const RuntimeStats& Runtime::stats() const {
    static const RuntimeStats empty;
    return impl_ ? impl_->stats : empty;
}
} // namespace editor_example
