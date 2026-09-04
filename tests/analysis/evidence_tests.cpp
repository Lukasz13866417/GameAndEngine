#include <vng/analysis/analysis.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Position : vng::gfx::Semantic<vng::Vec3> {};
struct WorldNormal : vng::gfx::Semantic<vng::Vec3> {};
struct Roughness : vng::gfx::Semantic<vng::f32> {};
using Vertex = vng::gfx::Record<Position>;
using Mesh = vng::gfx::Mesh<Vertex>;

[[nodiscard]] Mesh make_mesh()
{
    Mesh mesh(4);
    mesh.add_face(0, 1, 2);
    mesh.add_face(2, 3, 0);
    return mesh;
}

[[nodiscard]] vng::analysis::RenderItemProvenance make_provenance(
    vng::u64 entity)
{
    namespace a = vng::analysis;
    return {
        .entity = a::EntityId{entity},
        .mesh = a::MeshAssetId{20},
        .mesh_revision = a::AssetRevision{3},
        .material = a::MaterialId{40},
    };
}

struct CaptureChanges final {
    vng::analysis::FrameId frame{11};
    vng::u8 changed_red{};
    vng::f32 changed_depth{0.3F};
    bool change_surface{};
    vng::u64 second_entity{101};
};

[[nodiscard]] vng::analysis::AnalysisCapture make_capture(
    CaptureChanges changes = {})
{
    namespace a = vng::analysis;
    const auto mesh = make_mesh();
    const auto sources = a::primitive_sources(mesh);
    a::AnalysisManifest manifest{changes.frame};
    const auto first = manifest.add(make_provenance(100), sources);
    const auto second = manifest.add(
        make_provenance(changes.second_entity), sources);

    constexpr a::Extent2D extent{3, 2};
    a::AnalysisCapture::ColorImage colors(
        extent, a::Rgba8{10, 20, 30, 255});
    a::AnalysisCapture::DepthImage depths(extent, 1.0F);
    a::AnalysisCapture::SurfaceImage keys(
        extent, a::SurfaceKey::background());

    keys.at({1, 0}) = {first, a::PrimitiveId{0}};
    keys.at({2, 0}) = {
        changes.change_surface ? second : first,
        changes.change_surface ? a::PrimitiveId{1} : a::PrimitiveId{0},
    };
    keys.at({0, 1}) = {first, a::PrimitiveId{1}};
    keys.at({1, 1}) = {second, a::PrimitiveId{0}};
    depths.at({1, 0}) = 0.2F;
    depths.at({2, 0}) = changes.changed_depth;
    depths.at({0, 1}) = 0.4F;
    depths.at({1, 1}) = 0.1F;
    colors.at({2, 0}).r = changes.changed_red;

    auto capture = a::AnalysisCapture::create(
        std::move(colors),
        std::move(depths),
        std::move(keys),
        std::move(manifest));
    return std::move(capture).value();
}

[[nodiscard]] vng::analysis::FrameEvidence make_evidence(
    vng::Vec3 normal = {0.0F, 0.0F, 1.0F},
    std::string label = "test frame",
    std::string vertex_source = "#version 460\nvoid main() {}\n")
{
    namespace a = vng::analysis;
    constexpr a::Extent2D extent{3, 2};
    a::Image<vng::Vec3> normals(extent, normal);
    a::Image<vng::Vec3> observed_normals(extent, {0.0F, 0.0F, 1.0F});
    a::Image<vng::f32> observed_roughness(extent, 0.4F);
    a::Image<vng::u32> object_ids(extent, 17);

    auto evidence = a::FrameEvidence::create(
        make_capture(),
        a::CaptureRequest::diagnostic()
            .observe(WorldNormal{})
            .observe(Roughness{}),
        a::FrameEvidenceMetadata{
            .label = std::move(label),
            .renderer = "test renderer",
            .invocation = a::RenderInvocationIdentity{
                .workload_fingerprint = "workload:mesh-20@3",
                .view_fingerprint = "view:test",
                .renderer_fingerprint = "renderer:test-lit",
                .target_fingerprint = "target:3x2-rgba8-depth32f",
            },
            .camera = std::nullopt,
            .shader = std::nullopt,
            .properties = {
                {"z/property", "last"},
                {"a/property", "first"},
            },
        },
        std::vector<a::EvidenceChannel>{
            a::EvidenceChannel{"world/normal", std::move(normals)},
            a::EvidenceChannel{"object/id", std::move(object_ids)},
            a::EvidenceChannel{
                a::observation_channel_name(WorldNormal{}),
                std::move(observed_normals)},
            a::EvidenceChannel{
                a::observation_channel_name(Roughness{}),
                std::move(observed_roughness)},
        },
        std::vector<a::BackendArtifact>{
            {
                .name = "shader/vertex.glsl",
                .media_type = "text/x-glsl",
                .text = std::move(vertex_source),
            },
            {
                .name = "pipeline.txt",
                .media_type = "text/plain",
                .text = "depth_test=less\n",
            },
        });
    return std::move(evidence).value();
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{stream},
            std::istreambuf_iterator<char>{}};
}

class TemporaryDirectories final {
public:
    TemporaryDirectories()
    {
        static unsigned long counter{};
        root_ = std::filesystem::temp_directory_path()
            / ("vng-analysis-evidence-tests-"
               + std::to_string(reinterpret_cast<std::uintptr_t>(this))
               + "-" + std::to_string(++counter));
        std::filesystem::create_directories(root_);
    }

    TemporaryDirectories(const TemporaryDirectories&) = delete;
    TemporaryDirectories& operator=(const TemporaryDirectories&) = delete;

    ~TemporaryDirectories()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] std::filesystem::path first() const
    {
        return root_ / "first";
    }

    [[nodiscard]] std::filesystem::path second() const
    {
        return root_ / "second";
    }

private:
    std::filesystem::path root_;
};

} // namespace

TEST_CASE("capture request presets state their cost and scope", "[analysis][request]")
{
    namespace a = vng::analysis;

    const auto standard = a::CaptureRequest::standard();
    CHECK(standard.preset == a::CapturePreset::standard);
    CHECK(standard.scope == a::CaptureScope::full_frame);
    CHECK(standard.requests(a::Channel::color));
    CHECK(standard.observations().empty());
    CHECK_FALSE(standard.requests(a::DerivedVisualization::coverage));

    auto diagnostic = a::CaptureRequest::diagnostic()
        .observe(WorldNormal{})
        .observe(Roughness{})
        .observe(WorldNormal{});
    CHECK(diagnostic.requests(a::DerivedVisualization::coverage));
    CHECK(diagnostic.requests(a::DerivedVisualization::item));
    CHECK(diagnostic.requests(a::DerivedVisualization::primitive));
    REQUIRE(diagnostic.observations().size() == 2);
    CHECK(diagnostic.observations()[0].semantic_type == typeid(WorldNormal));
    CHECK(diagnostic.observations()[0].value_type == typeid(vng::Vec3));
    CHECK(diagnostic.observations()[0].semantic_name.find("WorldNormal")
          != std::string::npos);
    CHECK(diagnostic.observations()[1].semantic_type == typeid(Roughness));
    CHECK(diagnostic.observes(WorldNormal{}));
    CHECK_FALSE(diagnostic.observes(Position{}));

    const auto probe = a::CaptureRequest::probe({7, 9})
        .observe<Roughness>();
    CHECK(probe.preset == a::CapturePreset::probe);
    CHECK(probe.scope == a::CaptureScope::pixel);
    CHECK(probe.probe_pixel == a::Pixel{7, 9});
    REQUIRE(probe.observations().size() == 1);
    CHECK(probe.observes(Roughness{}));
}

TEST_CASE("capture automatically summarizes visible evidence", "[analysis][summary]")
{
    namespace a = vng::analysis;
    const auto capture = make_capture();
    const auto& summary = capture.summary();

    CHECK(summary.extent == a::Extent2D{3, 2});
    CHECK(summary.pixel_count == 6);
    CHECK(summary.covered_pixel_count == 4);
    CHECK(summary.background_pixel_count == 2);
    CHECK(summary.visible_item_count == 2);
    CHECK(summary.visible_primitive_count == 3);
    CHECK(summary.non_finite_depth_count == 0);
    REQUIRE(summary.covered_bounds);
    CHECK(*summary.covered_bounds
          == a::PixelBounds{{0, 0}, {2, 1}});
    REQUIRE(summary.device_depth);
    CHECK(summary.device_depth->minimum == 0.1F);
    CHECK(summary.device_depth->minimum_pixel == a::Pixel{1, 1});
    CHECK(summary.device_depth->maximum == 0.4F);
    CHECK(summary.device_depth->maximum_pixel == a::Pixel{0, 1});
    CHECK(summary.device_depth->finite_pixel_count == 4);

    REQUIRE(summary.items.size() == 2);
    CHECK(summary.items[0].covered_pixel_count == 3);
    CHECK(summary.items[0].visible_primitive_count == 2);
    CHECK(summary.items[1].covered_pixel_count == 1);
    CHECK(summary.items[1].visible_primitive_count == 1);
}

TEST_CASE("capture summary accounts for non-finite covered depths",
          "[analysis][summary]")
{
    namespace a = vng::analysis;
    const auto capture = make_capture({
        .changed_depth = std::numeric_limits<vng::f32>::quiet_NaN(),
    });

    CHECK(capture.summary().non_finite_depth_count == 1);
    REQUIRE(capture.summary().device_depth);
    CHECK(capture.summary().device_depth->finite_pixel_count == 3);
    CHECK(capture.summary().items[0].non_finite_depth_count == 1);
}

TEST_CASE("inspection distinguishes background, surface, and invalid pixels",
          "[analysis][inspect]")
{
    namespace a = vng::analysis;
    const auto capture = make_capture();

    const auto background = capture.inspect({0, 0});
    REQUIRE(background);
    CHECK(background->background());
    CHECK(background->surface_key == a::SurfaceKey::background());
    CHECK(background->device_depth == 1.0F);

    const auto surface = capture.inspect({0, 1});
    REQUIRE(surface);
    CHECK(surface->has_surface());
    REQUIRE(surface->surface);
    CHECK(surface->surface->face == 1);

    const auto outside = capture.inspect({3, 0});
    REQUIRE_FALSE(outside);
    CHECK(outside.error().code
          == a::CaptureDiagnosticCode::pixel_out_of_bounds);
    CHECK(outside.error().pixel == a::Pixel{3, 0});
}

TEST_CASE("derived visualizations encode grouping deterministically",
          "[analysis][visualization]")
{
    namespace a = vng::analysis;
    const auto capture = make_capture();
    const auto images = a::make_visualizations(capture);

    CHECK(images.coverage.at({0, 0}) == a::Rgba8{0, 0, 0, 255});
    CHECK(images.coverage.at({1, 0}) == a::Rgba8{255, 255, 255, 255});
    CHECK(images.items.at({1, 0}) == images.items.at({0, 1}));
    CHECK(images.items.at({1, 0}) != images.items.at({1, 1}));
    CHECK(images.primitives.at({1, 0}) == images.primitives.at({2, 0}));
    CHECK(images.primitives.at({1, 0}) != images.primitives.at({0, 1}));

    const auto again = a::make_visualizations(capture);
    CHECK(std::ranges::equal(
        again.items.pixels(), images.items.pixels()));
    CHECK(std::ranges::equal(
        again.primitives.pixels(), images.primitives.pixels()));
}

TEST_CASE("frame evidence validates and exposes typed additional channels",
          "[analysis][evidence]")
{
    namespace a = vng::analysis;
    auto evidence = make_evidence();

    REQUIRE(evidence.additional_channels().size() == 4);
    CHECK(std::ranges::is_sorted(
        evidence.additional_channels(), {}, &a::EvidenceChannel::name));
    CHECK(evidence.metadata().properties[0].key == "a/property");
    REQUIRE(evidence.backend_artifacts().size() == 2);
    CHECK(evidence.backend_artifacts()[0].name == "pipeline.txt");
    CHECK(evidence.backend_artifacts()[1].name == "shader/vertex.glsl");
    REQUIRE(evidence.backend_artifact("shader/vertex.glsl"));
    CHECK(evidence.backend_artifact("shader/vertex.glsl")->media_type
          == "text/x-glsl");
    CHECK_FALSE(evidence.backend_artifact("missing.txt"));
    REQUIRE(evidence.channel("world/normal"));
    CHECK(evidence.channel("world/normal")->value_kind()
          == a::EvidenceValueKind::vec3);
    CHECK(evidence.channel("world/normal")->value_type()
          == typeid(vng::Vec3));
    CHECK_FALSE(evidence.channel("missing"));
    REQUIRE(evidence.observation(WorldNormal{}));
    CHECK(evidence.observation(WorldNormal{})->at({1, 1})
          == vng::Vec3{0.0F, 0.0F, 1.0F});
    REQUIRE(evidence.observation(Roughness{}));
    CHECK(evidence.observation(Roughness{})->at({1, 1}) == 0.4F);

    const auto pixel = evidence.inspect({1, 1});
    REQUIRE(pixel);
    REQUIRE(pixel->canonical.surface);
    const auto* normal = pixel->find("world/normal");
    REQUIRE(normal);
    CHECK(std::get<vng::Vec3>(*normal) == vng::Vec3{0.0F, 0.0F, 1.0F});
    const auto* object = pixel->find("object/id");
    REQUIRE(object);
    CHECK(std::get<vng::u32>(*object) == 17);

    auto bad_probe = a::FrameEvidence::create(
        make_capture(), a::CaptureRequest::probe({9, 9}));
    REQUIRE_FALSE(bad_probe);
    CHECK(bad_probe.error().code
          == a::EvidenceDiagnosticCode::probe_out_of_bounds);

    std::vector<a::EvidenceChannel> bad_channels;
    bad_channels.emplace_back(
        "color", a::Image<vng::u32>({3, 2}, 0));
    auto reserved = a::FrameEvidence::create(
        make_capture(),
        a::CaptureRequest::standard(),
        {},
        std::move(bad_channels));
    REQUIRE_FALSE(reserved);
    CHECK(reserved.error().code
          == a::EvidenceDiagnosticCode::reserved_channel_name);

    auto missing_observation = a::FrameEvidence::create(
        make_capture(),
        a::CaptureRequest::standard().observe(WorldNormal{}));
    REQUIRE_FALSE(missing_observation);
    CHECK(missing_observation.error().code
          == a::EvidenceDiagnosticCode::missing_requested_observation);
    CHECK(missing_observation.error().name
          == a::observation_channel_name(WorldNormal{}));

    auto wrong_observation_type = a::FrameEvidence::create(
        make_capture(),
        a::CaptureRequest::standard().observe(WorldNormal{}),
        {},
        {a::EvidenceChannel{
            a::observation_channel_name(WorldNormal{}),
            a::Image<vng::Vec2>{{3, 2}, {0.0F, 1.0F}}}});
    REQUIRE_FALSE(wrong_observation_type);
    CHECK(wrong_observation_type.error().code
          == a::EvidenceDiagnosticCode::mismatched_observation_type);

    auto duplicate_observation = a::FrameEvidence::create(
        make_capture(),
        a::CaptureRequest::standard().observe(WorldNormal{}),
        {},
        {
            a::EvidenceChannel{
                a::observation_channel_name(WorldNormal{}),
                a::Image<vng::Vec3>{{3, 2}, {0.0F, 0.0F, 1.0F}}},
            a::EvidenceChannel{
                a::observation_channel_name(WorldNormal{}),
                a::Image<vng::Vec3>{{3, 2}, {0.0F, 1.0F, 0.0F}}},
        });
    REQUIRE_FALSE(duplicate_observation);
    CHECK(duplicate_observation.error().code
          == a::EvidenceDiagnosticCode::duplicate_channel_name);

    auto invalid_artifact = a::FrameEvidence::create(
        make_capture(),
        a::CaptureRequest::standard(),
        {},
        {},
        {{
            .name = "../outside.txt",
            .media_type = "text/plain",
            .text = "not exported",
        }});
    REQUIRE_FALSE(invalid_artifact);
    CHECK(invalid_artifact.error().code
          == a::EvidenceDiagnosticCode::invalid_artifact_name);

    auto duplicate_artifact = a::FrameEvidence::create(
        make_capture(),
        a::CaptureRequest::standard(),
        {},
        {},
        {
            {.name = "state.txt", .media_type = "text/plain", .text = "a"},
            {.name = "state.txt", .media_type = "text/plain", .text = "b"},
        });
    REQUIRE_FALSE(duplicate_artifact);
    CHECK(duplicate_artifact.error().code
          == a::EvidenceDiagnosticCode::duplicate_artifact_name);
}

TEST_CASE("capture comparison is deterministic and tolerance-aware",
          "[analysis][comparison]")
{
    namespace a = vng::analysis;
    const auto original = make_capture();
    const auto another_frame = make_capture({.frame = a::FrameId{99}});

    auto same = a::compare_captures(original, another_frame);
    REQUIRE(same);
    CHECK(same->equivalent());

    auto compare_frame = a::compare_captures(
        original,
        another_frame,
        {.compare_frame_id = true});
    REQUIRE(compare_frame);
    CHECK_FALSE(compare_frame->equivalent());
    CHECK_FALSE(compare_frame->frame_matches);

    const auto changed = make_capture({
        .changed_red = 3,
        .changed_depth = 0.3005F,
    });
    auto strict = a::compare_captures(original, changed);
    REQUIRE(strict);
    CHECK_FALSE(strict->equivalent());
    CHECK(strict->color_difference_count == 1);
    CHECK(strict->depth_difference_count == 1);
    CHECK(strict->first_color_difference == a::Pixel{2, 0});
    CHECK(strict->first_depth_difference == a::Pixel{2, 0});

    auto tolerant = a::compare_captures(
        original,
        changed,
        {
            .color_absolute_tolerance = 3,
            .depth_absolute_tolerance = 0.001F,
        });
    REQUIRE(tolerant);
    CHECK(tolerant->equivalent());
    CHECK(tolerant->maximum_color_component_difference == 3);

    const auto changed_surface = make_capture({.change_surface = true});
    auto surface = a::compare_captures(original, changed_surface);
    REQUIRE(surface);
    CHECK(surface->surface_key_difference_count == 1);
    CHECK(surface->first_surface_key_difference == a::Pixel{2, 0});

    const auto changed_manifest = make_capture({.second_entity = 999});
    auto manifest = a::compare_captures(original, changed_manifest);
    REQUIRE(manifest);
    CHECK_FALSE(manifest->manifest_matches);
    CHECK_FALSE(manifest->equivalent());

    auto invalid = a::compare_captures(
        original,
        original,
        {.depth_absolute_tolerance = -1.0F});
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code
          == a::ComparisonDiagnosticCode::invalid_depth_tolerance);
}

TEST_CASE("frame evidence comparison includes metadata and extra channels",
          "[analysis][comparison][evidence]")
{
    namespace a = vng::analysis;
    const auto original = make_evidence();
    const auto same = make_evidence();

    auto equal = a::compare_evidence(original, same);
    REQUIRE(equal);
    CHECK(equal->equivalent());

    const auto changed_channel = make_evidence({1.0F, 0.0F, 0.0F});
    auto values = a::compare_evidence(original, changed_channel);
    REQUIRE(values);
    CHECK_FALSE(values->equivalent());
    CHECK(values->channel_layout_matches);
    CHECK(values->additional_value_difference_count == 6);
    CHECK(values->first_additional_channel_difference == "world/normal");
    CHECK(values->first_additional_pixel_difference == a::Pixel{0, 0});

    const auto changed_metadata = make_evidence(
        {0.0F, 0.0F, 1.0F}, "another label");
    auto metadata = a::compare_evidence(original, changed_metadata);
    REQUIRE(metadata);
    CHECK_FALSE(metadata->metadata_matches);

    auto ignore_metadata = a::compare_evidence(
        original,
        changed_metadata,
        {
            .capture = {},
            .compare_request = true,
            .compare_metadata = false,
            .compare_additional_channels = true,
        });
    REQUIRE(ignore_metadata);
    CHECK(ignore_metadata->equivalent());

    const auto changed_backend = make_evidence(
        {0.0F, 0.0F, 1.0F},
        "test frame",
        "#version 460\n// changed\nvoid main() {}\n");
    auto portable = a::compare_evidence(original, changed_backend);
    REQUIRE(portable);
    CHECK(portable->equivalent());

    auto exact_backend = a::compare_evidence(
        original,
        changed_backend,
        {
            .capture = {},
            .compare_backend_artifacts = true,
        });
    REQUIRE(exact_backend);
    CHECK_FALSE(exact_backend->equivalent());
    CHECK_FALSE(exact_backend->backend_artifacts_match);
    CHECK(exact_backend->first_backend_artifact_difference
          == "shader/vertex.glsl");
}

TEST_CASE("evidence directory export is complete and byte deterministic",
          "[analysis][export]")
{
    namespace a = vng::analysis;
    const auto evidence = make_evidence(
        {0.0F, 0.0F, 1.0F}, "test \"frame\"\n");
    TemporaryDirectories temporary;

    auto first = a::export_evidence_directory(evidence, temporary.first());
    REQUIRE(first);
    auto second = a::export_evidence_directory(evidence, temporary.second());
    REQUIRE(second);
    REQUIRE(first->files == second->files);
    CHECK(first->files.size() == 11);

    for (const auto& relative : first->files) {
        CHECK(read_file(first->directory / relative)
              == read_file(second->directory / relative));
    }

    const auto json = read_file(first->directory / "evidence.json");
    CHECK(json.starts_with("{\n  \"format\":\"vng-frame-evidence\""));
    CHECK(json.find("\"covered_pixel_count\":4") != std::string::npos);
    CHECK(json.find("\"world/normal\"") != std::string::npos);
    CHECK(json.find("\"observations\":[{") != std::string::npos);
    CHECK(json.find(
        "\"workload_fingerprint\":\"workload:mesh-20@3\"")
          != std::string::npos);
    CHECK(json.find(
        "\"target_fingerprint\":\"target:3x2-rgba8-depth32f\"")
          != std::string::npos);
    CHECK(json.find("\"backend_artifacts\":[") != std::string::npos);
    CHECK(json.find("\"path\":\"artifacts/shader/vertex.glsl\"")
          != std::string::npos);
    CHECK(json.find("test \\\"frame\\\"\\n") != std::string::npos);
    CHECK(read_file(first->directory / "color.ppm").starts_with("P6\n3 2\n255\n"));
    CHECK(read_file(first->directory / "depth.pgm").starts_with("P5\n3 2\n255\n"));
    CHECK(read_file(first->directory / "pixels.csv").starts_with(
        "x,y,r,g,b,a,device_depth,item,primitive\n"));
    CHECK(read_file(first->directory / "pixels.csv").find(
        "0,0,10,20,30,255,1,0,4294967295\n") != std::string::npos);
    CHECK(read_file(first->directory / "artifacts/pipeline.txt")
          == "depth_test=less\n");
    CHECK(read_file(first->directory / "artifacts/shader/vertex.glsl")
          == "#version 460\nvoid main() {}\n");

    auto occupied = a::export_evidence_directory(evidence, temporary.first());
    REQUIRE_FALSE(occupied);
    CHECK(occupied.error().code
          == a::ExportDiagnosticCode::destination_not_empty);
}
