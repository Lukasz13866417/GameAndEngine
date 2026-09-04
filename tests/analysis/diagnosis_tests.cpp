#include <vng/analysis/analysis.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

struct Position : vng::gfx::Semantic<vng::Vec3> {};
using Vertex = vng::gfx::Record<Position>;
using Mesh = vng::gfx::Mesh<Vertex>;

struct CoveredPixel final {
    vng::analysis::Pixel pixel;
    std::size_t item_index{};
    vng::analysis::Rgba8 color{16, 32, 64, 255};
    vng::f32 depth{0.5F};
};

[[nodiscard]] vng::analysis::RenderInvocationIdentity invocation_identity(
    std::string workload = "workload:scene-7")
{
    return {
        .workload_fingerprint = std::move(workload),
        .view_fingerprint = "view:camera-3",
        .renderer_fingerprint = "renderer:opaque-11",
        .target_fingerprint = "target:rgba8-depth32f",
    };
}

[[nodiscard]] vng::analysis::FrameEvidence make_evidence(
    vng::analysis::Extent2D extent,
    std::size_t item_count,
    std::initializer_list<CoveredPixel> covered = {},
    vng::analysis::Rgba8 background = {200, 180, 160, 255},
    std::optional<vng::analysis::RenderInvocationIdentity> identity =
        invocation_identity(),
    vng::analysis::FrameId frame = {1},
    std::string raster_variant = "production")
{
    namespace a = vng::analysis;

    Mesh mesh(3);
    mesh.add_face(0, 1, 2);
    const auto sources = a::primitive_sources(mesh);

    a::AnalysisManifest manifest{frame};
    std::vector<a::FrameItemId> items;
    items.reserve(item_count);
    for (std::size_t index = 0; index < item_count; ++index) {
        items.push_back(manifest.add(
            a::RenderItemProvenance{
                .entity = a::EntityId{static_cast<vng::u64>(index + 1)},
                .mesh = a::MeshAssetId{7},
                .mesh_revision = a::AssetRevision{3},
                .material = a::MaterialId{11},
            },
            sources));
    }

    a::AnalysisCapture::ColorImage colors(extent, background);
    a::AnalysisCapture::DepthImage depths(extent, 1.0F);
    a::AnalysisCapture::SurfaceImage keys(
        extent, a::SurfaceKey::background());
    for (const auto& sample : covered) {
        colors.at(sample.pixel) = sample.color;
        depths.at(sample.pixel) = sample.depth;
        keys.at(sample.pixel) = {
            items.at(sample.item_index), a::PrimitiveId{0}};
    }

    auto capture = a::AnalysisCapture::create(
        std::move(colors),
        std::move(depths),
        std::move(keys),
        std::move(manifest));
    auto evidence = a::FrameEvidence::create(
        std::move(capture).value(),
        a::CaptureRequest::standard(),
        a::FrameEvidenceMetadata{
            .label = {},
            .renderer = {},
            .invocation = std::move(identity),
            .camera = std::nullopt,
            .shader = std::nullopt,
            .properties = {{"raster.variant", std::move(raster_variant)}},
        });
    return std::move(evidence).value();
}

[[nodiscard]] bool has_finding(
    const vng::analysis::DiagnosticSweep& sweep,
    vng::analysis::FindingCode code)
{
    return std::ranges::any_of(
        sweep.findings(),
        [code](const auto& finding) { return finding.code == code; });
}

[[nodiscard]] const vng::analysis::DiagnosticFinding* find_finding(
    const vng::analysis::DiagnosticSweep& sweep,
    vng::analysis::FindingCode code)
{
    const auto found = std::ranges::find(
        sweep.findings(), code, &vng::analysis::DiagnosticFinding::code);
    return found == sweep.findings().end() ? nullptr : &*found;
}

} // namespace

TEST_CASE("diagnostic sweep canonicalizes variants and provides lookup",
          "[analysis][diagnosis]")
{
    namespace a = vng::analysis;
    constexpr a::Extent2D extent{2, 2};

    auto sweep = a::DiagnosticSweep::create({
        {a::DiagnosticVariant::depth_always, make_evidence(extent, 0)},
        {a::DiagnosticVariant::production, make_evidence(extent, 0)},
        {a::DiagnosticVariant::cull_disabled, make_evidence(extent, 0)},
    });

    REQUIRE(sweep);
    REQUIRE(sweep->variants().size() == 3);
    CHECK(sweep->variants()[0].variant
          == a::DiagnosticVariant::production);
    CHECK(sweep->variants()[1].variant
          == a::DiagnosticVariant::cull_disabled);
    CHECK(sweep->variants()[2].variant
          == a::DiagnosticVariant::depth_always);
    CHECK(sweep->find(a::DiagnosticVariant::production)
          == &sweep->production());
    CHECK(sweep->find(a::DiagnosticVariant::cull_disabled) != nullptr);
    CHECK(sweep->find(a::DiagnosticVariant::depth_always) != nullptr);

    auto production_only = a::DiagnosticSweep::create({
        {a::DiagnosticVariant::production, make_evidence(extent, 0)},
    });
    REQUIRE(production_only);
    CHECK(production_only->find(a::DiagnosticVariant::cull_disabled)
          == nullptr);
}

TEST_CASE("diagnostic sweep identity ignores allocation and variant metadata",
          "[analysis][diagnosis][identity]")
{
    namespace a = vng::analysis;
    constexpr a::Extent2D extent{2, 2};
    constexpr a::Rgba8 background{200, 180, 160, 255};

    auto sweep = a::DiagnosticSweep::create({
        {a::DiagnosticVariant::production,
         make_evidence(
             extent, 0, {}, background, invocation_identity(),
             a::FrameId{41}, "production")},
        {a::DiagnosticVariant::cull_disabled,
         make_evidence(
             extent, 0, {}, background, invocation_identity(),
             a::FrameId{42}, "cull_disabled")},
        {a::DiagnosticVariant::depth_always,
         make_evidence(
             extent, 0, {}, background, invocation_identity(),
             a::FrameId{43}, "depth_always")},
    });

    REQUIRE(sweep);
    CHECK(sweep->production().capture().frame() == a::FrameId{41});
    CHECK(sweep->find(a::DiagnosticVariant::depth_always)
              ->metadata().properties.front().value
          == "depth_always");
}

TEST_CASE("diagnostic sweep rejects malformed variant sets",
          "[analysis][diagnosis][validation]")
{
    namespace a = vng::analysis;

    SECTION("production evidence is required") {
        auto result = a::DiagnosticSweep::create({
            {a::DiagnosticVariant::cull_disabled,
             make_evidence({2, 2}, 0)},
        });

        REQUIRE_FALSE(result);
        CHECK(result.error().code
              == a::SweepDiagnosticCode::missing_production);
        CHECK_FALSE(result.error().variant);
    }

    SECTION("each variant may occur only once") {
        auto result = a::DiagnosticSweep::create({
            {a::DiagnosticVariant::production,
             make_evidence({2, 2}, 0)},
            {a::DiagnosticVariant::production,
             make_evidence({2, 2}, 0)},
        });

        REQUIRE_FALSE(result);
        CHECK(result.error().code
              == a::SweepDiagnosticCode::duplicate_variant);
        CHECK(result.error().variant
              == a::DiagnosticVariant::production);
    }

    SECTION("counterfactual images must share the production extent") {
        auto result = a::DiagnosticSweep::create({
            {a::DiagnosticVariant::production,
             make_evidence({2, 2}, 0)},
            {a::DiagnosticVariant::depth_always,
             make_evidence({3, 2}, 0)},
        });

        REQUIRE_FALSE(result);
        CHECK(result.error().code
              == a::SweepDiagnosticCode::mismatched_extent);
        CHECK(result.error().variant
              == a::DiagnosticVariant::depth_always);
    }


    SECTION("every variant requires a complete invocation identity") {
        auto missing = a::DiagnosticSweep::create({
            {a::DiagnosticVariant::production,
             make_evidence(
                 {2, 2}, 0, {}, {200, 180, 160, 255}, std::nullopt)},
        });

        REQUIRE_FALSE(missing);
        CHECK(missing.error().code
              == a::SweepDiagnosticCode::missing_invocation_identity);
        CHECK(missing.error().variant
              == a::DiagnosticVariant::production);
        CHECK_FALSE(missing.error().identity_component);

        auto incomplete_identity = invocation_identity();
        incomplete_identity.view_fingerprint.clear();
        auto incomplete = a::DiagnosticSweep::create({
            {a::DiagnosticVariant::production,
             make_evidence(
                 {2, 2}, 0, {}, {200, 180, 160, 255},
                 std::move(incomplete_identity))},
        });

        REQUIRE_FALSE(incomplete);
        CHECK(incomplete.error().code
              == a::SweepDiagnosticCode::missing_invocation_identity);
        CHECK(incomplete.error().identity_component
              == a::InvocationIdentityComponent::view);
    }

    SECTION("counterfactuals must identify the same render invocation") {
        auto result = a::DiagnosticSweep::create({
            {a::DiagnosticVariant::production,
             make_evidence({2, 2}, 0)},
            {a::DiagnosticVariant::cull_disabled,
             make_evidence(
                 {2, 2}, 0, {}, {200, 180, 160, 255},
                 invocation_identity("workload:another-scene"))},
        });

        REQUIRE_FALSE(result);
        CHECK(result.error().code
              == a::SweepDiagnosticCode::mismatched_invocation_identity);
        CHECK(result.error().variant
              == a::DiagnosticVariant::cull_disabled);
        CHECK(result.error().identity_component
              == a::InvocationIdentityComponent::workload);
    }
}

TEST_CASE("an empty production image is explained by revealing variants",
          "[analysis][diagnosis][inference]")
{
    namespace a = vng::analysis;
    constexpr a::Extent2D extent{2, 2};
    const CoveredPixel visible{{0, 0}};

    SECTION("disabling culling reveals geometry") {
        auto sweep = a::DiagnosticSweep::create({
            {a::DiagnosticVariant::production, make_evidence(extent, 1)},
            {a::DiagnosticVariant::cull_disabled,
             make_evidence(extent, 1, {visible})},
            {a::DiagnosticVariant::depth_always,
             make_evidence(extent, 1)},
        });

        REQUIRE(sweep);
        const auto* finding = find_finding(
            *sweep, a::FindingCode::likely_culling);
        REQUIRE(finding);
        CHECK(finding->confidence == a::FindingConfidence::strong);
        CHECK_FALSE(has_finding(
            *sweep, a::FindingCode::likely_depth_rejection));
        CHECK_FALSE(has_finding(
            *sweep, a::FindingCode::no_visible_geometry));
    }

    SECTION("an always-pass depth test reveals geometry") {
        auto sweep = a::DiagnosticSweep::create({
            {a::DiagnosticVariant::production, make_evidence(extent, 1)},
            {a::DiagnosticVariant::cull_disabled,
             make_evidence(extent, 1)},
            {a::DiagnosticVariant::depth_always,
             make_evidence(extent, 1, {visible})},
        });

        REQUIRE(sweep);
        const auto* finding = find_finding(
            *sweep, a::FindingCode::likely_depth_rejection);
        REQUIRE(finding);
        CHECK(finding->confidence == a::FindingConfidence::strong);
        CHECK_FALSE(has_finding(*sweep, a::FindingCode::likely_culling));
        CHECK_FALSE(has_finding(
            *sweep, a::FindingCode::no_visible_geometry));
    }

    SECTION("both revealing variants retain both independent findings") {
        auto sweep = a::DiagnosticSweep::create({
            {a::DiagnosticVariant::production, make_evidence(extent, 1)},
            {a::DiagnosticVariant::cull_disabled,
             make_evidence(extent, 1, {visible})},
            {a::DiagnosticVariant::depth_always,
             make_evidence(extent, 1, {visible})},
        });

        REQUIRE(sweep);
        CHECK(has_finding(*sweep, a::FindingCode::likely_culling));
        CHECK(has_finding(
            *sweep, a::FindingCode::likely_depth_rejection));
        CHECK_FALSE(has_finding(
            *sweep, a::FindingCode::no_visible_geometry));
    }

    SECTION("no revealing variant points beyond tested raster state") {
        auto sweep = a::DiagnosticSweep::create({
            {a::DiagnosticVariant::production, make_evidence(extent, 0)},
            {a::DiagnosticVariant::cull_disabled,
             make_evidence(extent, 0)},
            {a::DiagnosticVariant::depth_always,
             make_evidence(extent, 0)},
        });

        REQUIRE(sweep);
        const auto* finding = find_finding(
            *sweep, a::FindingCode::no_visible_geometry);
        REQUIRE(finding);
        CHECK(finding->confidence == a::FindingConfidence::hint);
        CHECK_FALSE(has_finding(*sweep, a::FindingCode::likely_culling));
        CHECK_FALSE(has_finding(
            *sweep, a::FindingCode::likely_depth_rejection));
    }
}

TEST_CASE("coverage distinguishes a black result from absent geometry",
          "[analysis][diagnosis][inference]")
{
    namespace a = vng::analysis;
    constexpr a::Extent2D extent{2, 2};

    SECTION("RGB values through two count as near black") {
        auto sweep = a::DiagnosticSweep::create({
            {a::DiagnosticVariant::production,
             make_evidence(
                 extent,
                 1,
                 {{{0, 0}, 0, {0, 1, 2, 0}, 0.5F}},
                 {255, 255, 255, 255})},
        });

        REQUIRE(sweep);
        const auto* finding = find_finding(
            *sweep, a::FindingCode::covered_but_near_black);
        REQUIRE(finding);
        CHECK(finding->confidence == a::FindingConfidence::strong);
        CHECK_FALSE(has_finding(
            *sweep, a::FindingCode::no_visible_geometry));
    }

    SECTION("one visibly bright covered component prevents the finding") {
        auto sweep = a::DiagnosticSweep::create({
            {a::DiagnosticVariant::production,
             make_evidence(
                 extent,
                 1,
                 {{{0, 0}, 0, {0, 3, 0, 255}, 0.5F}})},
        });

        REQUIRE(sweep);
        CHECK_FALSE(has_finding(
            *sweep, a::FindingCode::covered_but_near_black));
    }
}

TEST_CASE("depth anomalies and invisible submissions are reported",
          "[analysis][diagnosis][inference]")
{
    namespace a = vng::analysis;
    constexpr a::Extent2D extent{2, 2};

    auto sweep = a::DiagnosticSweep::create({
        {a::DiagnosticVariant::production,
         make_evidence(
             extent,
             2,
             {{{0, 0},
               0,
               {20, 30, 40, 255},
               std::numeric_limits<vng::f32>::quiet_NaN()}})},
    });

    REQUIRE(sweep);
    const auto* depth = find_finding(
        *sweep, a::FindingCode::non_finite_depth);
    REQUIRE(depth);
    CHECK(depth->confidence == a::FindingConfidence::strong);
    const auto* invisible = find_finding(
        *sweep, a::FindingCode::invisible_items);
    REQUIRE(invisible);
    CHECK(invisible->confidence == a::FindingConfidence::hint);
    CHECK(sweep->production().summary().visible_item_count == 1);
    CHECK(sweep->production().capture().manifest().size() == 2);

    auto all_visible = a::DiagnosticSweep::create({
        {a::DiagnosticVariant::production,
         make_evidence(
             extent,
             2,
             {
                 {{0, 0}, 0, {20, 30, 40, 255}, 0.25F},
                 {{1, 0}, 1, {40, 30, 20, 255}, 0.75F},
             })},
    });
    REQUIRE(all_visible);
    CHECK_FALSE(has_finding(
        *all_visible, a::FindingCode::invisible_items));
    CHECK_FALSE(has_finding(
        *all_visible, a::FindingCode::non_finite_depth));
}
