#include <vng/resources/resources.hpp>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace res = vng::resources;

struct Context {
    int creations{};
};

struct Product {
    int value{};
    int instance{};
};

struct MoveOnlySource {
    std::unique_ptr<const int> value;

    [[nodiscard]] res::Result<std::unique_ptr<Product>> provide(Context& context) const
    {
        return std::make_unique<Product>(*value, ++context.creations);
    }
};

struct CpuSource {
    int value{};

    [[nodiscard]] res::Result<int> provide() const { return value; }
};

struct SizedSource {
    int levels{};

    [[nodiscard]] res::Result<Product> provide(Context& context, const int& extent) const
    {
        return Product{levels * extent, ++context.creations};
    }
};

struct CpuRequestSource {
    [[nodiscard]] res::Result<int> provide(const int& extent) const { return extent * 2; }
};

struct MutableSource {
    [[nodiscard]] res::Result<int> provide(Context&) { return 1; }
};

struct UncheckedSource {
    [[nodiscard]] int provide(Context&) const { return 1; }
};

struct NativeDiagnostic {
    enum class Code { corrupt_asset };
    Code code{Code::corrupt_asset};
    std::string message;
    std::string driver_log;
    std::string generated_source;
    std::vector<std::string> notes;
    std::vector<int> source_map;
};

static_assert(res::ProviderFor<MoveOnlySource, std::unique_ptr<Product>, Context>);
static_assert(res::ProviderFor<CpuSource, int>);
static_assert(res::ProviderFor<CpuSource, int, Context>);
static_assert(res::ProviderFor<CpuSource, int, const Context>);
static_assert(res::ProviderFor<SizedSource, Product, Context, int>);
static_assert(res::ProviderFor<CpuRequestSource, int, Context, int>);
static_assert(!res::ProviderFor<CpuSource, int, Context, int>);
static_assert(!res::ProviderFor<CpuSource, int, void, int>);
static_assert(!std::constructible_from<res::Provider<int, void, int>, CpuSource>);
static_assert(!res::ProviderFor<MutableSource, int, Context>);
static_assert(!res::ProviderFor<UncheckedSource, int, Context>);
static_assert(!res::ProviderFor<CpuSource, std::unique_ptr<Product>, Context>);
static_assert(std::copy_constructible<res::Provider<std::unique_ptr<Product>, Context>>);

} // namespace

TEST_CASE("owning provider copies retain move-only recipes and produce independent products",
          "[resources][provider]")
{
    Context context;
    auto make_source = [] {
        return res::Provider<std::unique_ptr<Product>, Context>{
            MoveOnlySource{std::make_unique<const int>(42)}};
    };
    const auto original = make_source();
    auto copy = original;
    auto first = original.provide(context);
    auto second = copy.provide(context);
    REQUIRE(first);
    REQUIRE(second);
    CHECK((*first)->value == 42);
    CHECK((*second)->value == 42);
    CHECK((*first)->instance == 1);
    CHECK((*second)->instance == 2);
    CHECK(first->get() != second->get());

    // A builder can disappear after handing the provider to its owner.
    copy = {};
    auto third = original.provide(context);
    REQUIRE(third);
    CHECK((*third)->instance == 3);
}

TEST_CASE("callable providers retain const move-only captures", "[resources][provider]")
{
    Context context;
    const auto recipe = res::provider(
        [value = std::make_unique<const int>(27)](Context& device)
            -> res::Result<Product> {
            return Product{*value, ++device.creations};
        });
    static_assert(res::ProviderFor<decltype(recipe), Product, Context>);
    auto first = res::provide(recipe, context);
    auto second = res::provide(recipe, context);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->value == 27);
    CHECK(second->value == 27);
    CHECK(context.creations == 2);

    auto mutable_recipe = res::provider([calls = 0](Context&) mutable -> res::Result<int> {
        return ++calls;
    });
    static_assert(!res::ProviderFor<decltype(mutable_recipe), int, Context>);
}

TEST_CASE("context-free providers can be used by CPU and device owners", "[resources][provider]")
{
    Context context;
    const Context immutable_context;
    const CpuSource source{19};
    REQUIRE(res::provide(source));
    CHECK(*res::provide(source) == 19);
    CHECK(*res::provide(source, context) == 19);
    CHECK(*res::provide(source, immutable_context) == 19);
    const res::Provider<int> cpu_provider{source};
    const res::Provider<int, Context> device_provider{source};
    const res::Provider<int, const Context> immutable_provider{source};
    CHECK(*cpu_provider.provide() == 19);
    CHECK(*device_provider.provide(context) == 19);
    CHECK(*immutable_provider.provide(immutable_context) == 19);
    const res::Provider<char> converted{CpuSource{19}};
    CHECK(*converted.provide() == 19);
}

TEST_CASE("request providers receive per-instance state and prefer full signatures",
          "[resources][provider]")
{
    Context context;
    const res::Provider<Product, Context, int> provider{SizedSource{5}};
    auto small = provider.provide(context, 16);
    auto large = provider.provide(context, 128);
    REQUIRE(small);
    REQUIRE(large);
    CHECK(small->value == 80);
    CHECK(large->value == 640);
    CHECK(context.creations == 2);

    const res::Provider<int, Context, int> cpu_request{CpuRequestSource{}};
    CHECK(*cpu_request.provide(context, 32) == 64);
    const res::Provider<int, void, int> cpu_only_request{CpuRequestSource{}};
    CHECK(*cpu_only_request.provide(32) == 64);

    struct Both {
        res::Result<int> provide() const { return 1; }
        res::Result<int> provide(Context&) const { return 2; }
        res::Result<int> provide(const int&) const { return 3; }
        res::Result<int> provide(Context&, const int&) const { return 4; }
    };
    CHECK(*res::provide(Both{}) == 1);
    CHECK(*res::provide(Both{}, context) == 2);
    CHECK(*res::provide(Both{}, context, 5) == 4);
}

TEST_CASE("missing and moved providers return diagnostics", "[resources][provider]")
{
    Context context;
    res::Provider<int, Context> missing;
    CHECK_FALSE(missing);
    auto result = missing.provide(context);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == res::ErrorCode::no_provider);
    CHECK_FALSE(result.error().message.empty());

    res::Provider<int, Context> original{CpuSource{7}};
    auto moved = std::move(original);
    CHECK_FALSE(original);
    REQUIRE(moved);
    CHECK(*moved.provide(context) == 7);
    CHECK_FALSE(original.provide(context));

    auto shared = res::share_provider(CpuSource{7});
    auto retained = std::move(shared);
    CHECK(*retained.provide() == 7);
    auto moved_shared = shared.provide();
    REQUIRE_FALSE(moved_shared);
    CHECK(moved_shared.error().code == res::ErrorCode::no_provider);
}

TEST_CASE("shared recipes and shared resources have distinct ownership", "[resources][provider]")
{
    Context context;
    auto source = res::share_provider(MoveOnlySource{std::make_unique<const int>(8)});
    const auto reused_source = source;
    auto first = source.provide(context);
    auto second = reused_source.provide(context);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->get() != second->get());
    CHECK(context.creations == 2);

    auto shared = res::share_resource(Product{42, 12});
    auto shared_copy = shared;
    static_assert(std::same_as<decltype(shared.get()), const Product&>);
    static_assert(std::same_as<decltype(*shared), const Product&>);
    CHECK(&shared.get() == &shared_copy.get());
    CHECK(shared->value == 42);
    CHECK(context.creations == 2);
}

TEST_CASE("provided packages transfer ready products and retain source lifetime",
          "[resources][provider]")
{
    Context context;
    auto create_package = [&] {
        auto source = res::share_provider(MoveOnlySource{std::make_unique<const int>(91)});
        auto product = source.provide(context);
        REQUIRE(product);
        return res::provided(std::move(*product), source);
    };
    auto package = create_package();
    CHECK(context.creations == 1);
    CHECK(package.value->value == 91);
    auto adopted = std::move(package);
    CHECK(adopted.value->instance == 1);
    auto candidate = adopted.provider.provide(context);
    REQUIRE(candidate);
    CHECK((*candidate)->value == 91);
    CHECK((*candidate)->instance == 2);
    CHECK(adopted.value->instance == 1);
}

TEST_CASE("diagnostic normalization retains complete original errors", "[resources][diagnostic]")
{
    auto source = res::provider([] -> std::expected<int, NativeDiagnostic> {
        return std::unexpected(NativeDiagnostic{
            .message = "Shader failed at line 17.",
            .driver_log = "vendor compiler details",
            .generated_source = "generated shader",
            .notes = {"loading material"},
            .source_map = {4, 17, 38},
        });
    });
    const res::Provider<int> owner{source};
    auto result = owner.provide();
    REQUIRE_FALSE(result);
    auto error = result.error();
    result = 7;
    CHECK(error.code == res::ErrorCode::provision_failed);
    CHECK(error.message == "Shader failed at line 17.");
    CHECK(error.driver_log == "vendor compiler details");
    CHECK(error.generated_source == "generated shader");
    CHECK(error.context == std::vector<std::string>{"loading material"});
    const auto* native = error.cause_as<NativeDiagnostic>();
    REQUIRE(native);
    CHECK(native->code == NativeDiagnostic::Code::corrupt_asset);
    CHECK(native->source_map == std::vector<int>{4, 17, 38});
    CHECK(error.cause_as<std::string>() == nullptr);
    auto copied = res::to_diagnostic(error);
    CHECK(copied.cause_as<NativeDiagnostic>() == native);
    copied.note("reloading character");
    CHECK(copied.context.size() == 2);
    CHECK(error.context.size() == 1);
}

TEST_CASE("normalization preserves move-only errors and void results", "[resources][diagnostic]")
{
    struct MoveOnlyError {
        std::string message;
        std::unique_ptr<int> evidence;
    };
    std::expected<std::unique_ptr<int>, MoveOnlyError> failure =
        std::unexpected(MoveOnlyError{"read failed", std::make_unique<int>(53)});
    auto normalized = res::into_result(std::move(failure));
    REQUIRE_FALSE(normalized);
    auto copied_error = normalized.error();
    const auto* native = copied_error.cause_as<MoveOnlyError>();
    REQUIRE(native);
    CHECK(*native->evidence == 53);

    std::expected<void, std::string> success;
    CHECK(res::into_result(std::move(success)));
    auto void_failure = res::into_result(
        std::expected<void, std::string>{std::unexpected("void failed")});
    REQUIRE_FALSE(void_failure);
    CHECK(void_failure.error().message == "void failed");

    const res::Provider<void> action{res::provider([] -> res::Result<void> { return {}; })};
    CHECK(action.provide());
}
