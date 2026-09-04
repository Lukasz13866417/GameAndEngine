#include <vng/analysis/analysis.hpp>

#include <catch2/catch_test_macros.hpp>

#include <concepts>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {

struct Position : vng::gfx::Semantic<vng::Vec3> {};
using Vertex = vng::gfx::Record<Position>;
using Mesh = vng::gfx::Mesh<Vertex>;

[[nodiscard]] Mesh make_mesh()
{
    Mesh mesh(4);
    mesh.add_face(0, 1, 2);
    mesh.add_face(2, 3, 0);
    return mesh;
}

[[nodiscard]] vng::analysis::RenderItemProvenance provenance(
    vng::u64 entity,
    vng::u64 mesh,
    vng::u64 revision,
    vng::u64 material)
{
    namespace a = vng::analysis;
    return {
        .entity = a::EntityId{entity},
        .mesh = a::MeshAssetId{mesh},
        .mesh_revision = a::AssetRevision{revision},
        .material = a::MaterialId{material},
    };
}

} // namespace

static_assert(!std::convertible_to<vng::analysis::FrameItemId, vng::u32>);
static_assert(!std::same_as<vng::analysis::EntityId,
                           vng::analysis::MeshAssetId>);
static_assert(std::is_trivially_copyable_v<vng::analysis::SurfaceKey>);
static_assert(sizeof(vng::analysis::SurfaceKey) == 8);
static_assert(std::same_as<
              vng::analysis::channel_pixel_t<vng::analysis::Channel::color>,
              vng::analysis::Rgba8>);

TEST_CASE("surface keys are a compact typed pair", "[analysis]")
{
    namespace a = vng::analysis;

    const a::SurfaceKey key{a::FrameItemId{17}, a::PrimitiveId{9}};
    CHECK(key.has_surface());
    CHECK(key.raw() == vng::UVec2{17, 9});
    CHECK(a::SurfaceKey::from_raw({17, 9}) == key);

    const auto background = a::SurfaceKey::background();
    CHECK_FALSE(background.has_surface());
    CHECK(background.item == a::background_item);
    CHECK(background.primitive == a::invalid_primitive);
}

TEST_CASE("analysis images use explicit top-left pixel coordinates", "[analysis]")
{
    namespace a = vng::analysis;

    a::Image<vng::u32> image({3, 2});
    image.at({0, 0}) = 10;
    image.at({2, 0}) = 12;
    image.at({0, 1}) = 20;
    image.at({2, 1}) = 22;

    CHECK(image.row(0).front() == 10);
    CHECK(image.row(0).back() == 12);
    CHECK(image.row(1).front() == 20);
    CHECK(image.row(1).back() == 22);
    CHECK_FALSE(image.contains({3, 1}));
    CHECK_THROWS_AS(image.at({0, 2}), std::out_of_range);
    CHECK_THROWS_AS(
        a::Image<vng::u32>({2, 2}, std::vector<vng::u32>(3)),
        std::invalid_argument);
}

TEST_CASE("mesh primitive maps preserve source faces and support ranges",
          "[analysis][mesh]")
{
    namespace a = vng::analysis;
    const auto mesh = make_mesh();

    const auto all = a::primitive_sources(mesh);
    REQUIRE(all);
    REQUIRE(all->size() == 2);
    REQUIRE(all->find(a::PrimitiveId{0}));
    REQUIRE(all->find(a::PrimitiveId{1}));
    CHECK(all->find(a::PrimitiveId{0})->face == 0);
    CHECK(all->find(a::PrimitiveId{0})->vertices
          == vng::gfx::TriangleFace{0, 1, 2});
    CHECK(all->find(a::PrimitiveId{1})->face == 1);
    CHECK_FALSE(all->find(a::PrimitiveId{2}));

    const auto second = a::primitive_sources(mesh, a::FaceRange{1, 1});
    REQUIRE(second->size() == 1);
    REQUIRE(second->find(a::PrimitiveId{0}));
    CHECK(second->find(a::PrimitiveId{0})->face == 1);
    CHECK(second->find(a::PrimitiveId{0})->vertices
          == vng::gfx::TriangleFace{2, 3, 0});

    CHECK_THROWS_AS(
        a::primitive_sources(mesh, a::FaceRange{2, 1}),
        std::out_of_range);
}

TEST_CASE("manifest assigns dense frame-local IDs and resolves provenance",
          "[analysis][manifest]")
{
    namespace a = vng::analysis;
    const auto mesh = make_mesh();
    const auto sources = a::primitive_sources(mesh);
    a::AnalysisManifest manifest{a::FrameId{41}};

    const auto first = manifest.add(provenance(100, 200, 3, 400), sources);
    const auto second = manifest.add(provenance(101, 200, 3, 400), sources);

    CHECK(first == a::FrameItemId{1});
    CHECK(second == a::FrameItemId{2});
    CHECK(manifest.size() == 2);
    REQUIRE(manifest.find(second));
    CHECK(manifest.find(second)->provenance.entity == a::EntityId{101});

    const auto surface = manifest.resolve({second, a::PrimitiveId{1}});
    REQUIRE(surface);
    CHECK(surface->item->id == second);
    CHECK(surface->primitive->face == 1);

    CHECK_FALSE(manifest.resolve(a::SurfaceKey::background()));
    CHECK_FALSE(manifest.resolve({a::FrameItemId{9}, a::PrimitiveId{0}}));
    CHECK_FALSE(manifest.resolve({first, a::PrimitiveId{7}}));
}

TEST_CASE("a capture resolves one pixel into stable source information",
          "[analysis][capture]")
{
    namespace a = vng::analysis;
    const auto mesh = make_mesh();
    a::AnalysisManifest manifest{a::FrameId{77}};
    const auto item = manifest.add(
        provenance(1001, 2002, 12, 3003),
        a::primitive_sources(mesh));

    constexpr a::Extent2D extent{2, 2};
    a::AnalysisCapture::ColorImage colors(extent, a::Rgba8{1, 2, 3, 255});
    a::AnalysisCapture::DepthImage depths(extent, 1.0F);
    a::AnalysisCapture::SurfaceImage keys(
        extent, a::SurfaceKey::background());

    colors.at({1, 0}) = {90, 80, 70, 255};
    depths.at({1, 0}) = 0.25F;
    keys.at({1, 0}) = {item, a::PrimitiveId{1}};

    auto capture = a::AnalysisCapture::create(
        std::move(colors),
        std::move(depths),
        std::move(keys),
        std::move(manifest));
    REQUIRE(capture);

    CHECK(capture->frame() == a::FrameId{77});
    CHECK(capture->extent() == extent);
    CHECK(&capture->channel<a::Channel::color>() == &capture->color());
    CHECK_FALSE(capture->surface_at({0, 0}));
    CHECK_FALSE(capture->surface_at({2, 0}));

    const auto hit = capture->surface_at({1, 0});
    REQUIRE(hit);
    CHECK(hit->pixel == a::Pixel{1, 0});
    CHECK(hit->key == a::SurfaceKey{item, a::PrimitiveId{1}});
    CHECK(hit->color == a::Rgba8{90, 80, 70, 255});
    CHECK(hit->device_depth == 0.25F);
    CHECK(hit->entity == a::EntityId{1001});
    CHECK(hit->mesh == a::MeshAssetId{2002});
    CHECK(hit->mesh_revision == a::AssetRevision{12});
    CHECK(hit->material == a::MaterialId{3003});
    CHECK(hit->face == 1);
    CHECK(hit->vertices == vng::gfx::TriangleFace{2, 3, 0});
}

TEST_CASE("capture construction rejects inconsistent or unresolvable data",
          "[analysis][capture]")
{
    namespace a = vng::analysis;
    constexpr a::Extent2D extent{2, 1};

    SECTION("channel extents differ")
    {
        auto result = a::AnalysisCapture::create(
            a::AnalysisCapture::ColorImage(extent),
            a::AnalysisCapture::DepthImage({1, 1}),
            a::AnalysisCapture::SurfaceImage(
                extent, a::SurfaceKey::background()),
            a::AnalysisManifest{a::FrameId{1}});

        REQUIRE_FALSE(result);
        CHECK(result.error().code
              == a::CaptureDiagnosticCode::mismatched_extent);
        CHECK(result.error().channel == a::Channel::device_depth);
    }

    SECTION("a nonzero key is missing from the manifest")
    {
        a::AnalysisCapture::SurfaceImage keys(
            extent, a::SurfaceKey::background());
        keys.at({1, 0}) = {a::FrameItemId{5}, a::PrimitiveId{0}};

        auto result = a::AnalysisCapture::create(
            a::AnalysisCapture::ColorImage(extent),
            a::AnalysisCapture::DepthImage(extent),
            std::move(keys),
            a::AnalysisManifest{a::FrameId{2}});

        REQUIRE_FALSE(result);
        CHECK(result.error().code
              == a::CaptureDiagnosticCode::unknown_surface_key);
        CHECK(result.error().pixel == a::Pixel{1, 0});
    }

    SECTION("background has one canonical representation")
    {
        a::AnalysisCapture::SurfaceImage keys(
            extent, a::SurfaceKey::background());
        keys.at({0, 0}) = {a::background_item, a::PrimitiveId{0}};

        auto result = a::AnalysisCapture::create(
            a::AnalysisCapture::ColorImage(extent),
            a::AnalysisCapture::DepthImage(extent),
            std::move(keys),
            a::AnalysisManifest{a::FrameId{3}});

        REQUIRE_FALSE(result);
        CHECK(result.error().code
              == a::CaptureDiagnosticCode::noncanonical_background_key);
    }
}
