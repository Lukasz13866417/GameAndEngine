#include <vng/timeline/timeline.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <type_traits>

TEST_CASE("Timeline cache tokens follow content across copies moves and edits", "[timeline][version]") {
    using namespace vng;
    timeline::Timeline original;
    REQUIRE(original.set({1,"scale"},{0,1.F}));
    const auto version = original.version();
    auto fork = original;
    CHECK(fork.version() == version);
    REQUIRE(fork.set({1,"scale"},{0,2.F}));
    CHECK(fork.version() != version);
    auto other = original;
    REQUIRE(other.set({1,"scale"},{0,3.F}));
    CHECK(other.version() != fork.version());
    other = fork;
    CHECK(other.version() == fork.version());
    const auto before_error = other.version();
    CHECK_FALSE(other.set({1,"scale"},{0,true}));
    CHECK_FALSE(other.erase({1,"missing"}));
    CHECK(other.version() == before_error);
    REQUIRE(other.set({1,"scale"},{1,4.F}));
    const auto before_move = other.version();
    REQUIRE(other.move({1,"scale"},1,2));
    CHECK(other.version() != before_move);
    auto moved = std::move(other);
    CHECK(other.tracks().empty()); CHECK(other.version() == 0);
    CHECK(moved.version() != 0);
    const auto before_erase = moved.version();
    REQUIRE(moved.erase({1,"scale"},2)); CHECK(moved.version() != before_erase);
    const auto before_track = moved.version();
    REQUIRE(moved.replace_track({{1,"scale"},"","",{{0,5.F}}}));
    CHECK(moved.version() != before_track);
    const auto before_replace = moved.version();
    REQUIRE(moved.replace({})); CHECK(moved.version() != before_replace);
    CHECK(moved == timeline::Timeline{}); // Tokens are not authored equality.
}

namespace {
using namespace vng;
using namespace vng::timeline;
using Catch::Approx;

const Target emission{1, "emission"};
const Target position{1, "position"};
constexpr auto hold = Interpolation::hold;
constexpr auto linear = Interpolation::linear;

template<class T>
T sampled(const Timeline& timeline, const Target& target, f32 time)
{
    auto value = timeline.sample(target, time);
    REQUIRE(value);
    REQUIRE(std::holds_alternative<T>(*value));
    return std::get<T>(*value);
}

std::vector<Track> copy_tracks(const Timeline& timeline)
{
    return {timeline.tracks().begin(), timeline.tracks().end()};
}

Track many_keys(u64 object, std::size_t count)
{
    Track track{{object, "number"}, "Number", "Layer", {}};
    track.keys.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        track.keys.push_back({static_cast<f32>(i), static_cast<f32>(i), linear});
    }
    return track;
}
} // namespace

TEST_CASE("timeline owns ordered property tracks independent of rendering", "[timeline]")
{
    static_assert(std::same_as<decltype(std::declval<const Timeline&>().tracks()), std::span<const Track>>);
    static_assert(std::same_as<decltype(std::declval<const Timeline&>().find(emission)), const Track*>);
    Timeline timeline;
    CHECK(timeline.tracks().empty());
    CHECK(timeline.find(emission) == nullptr);
    CHECK_FALSE(timeline.sample(emission, 0));
    REQUIRE(timeline.set({3, "a"}, {2, 1.0F}));
    REQUIRE(timeline.set({1, "z"}, {2, true}));
    REQUIRE(timeline.set(position, {2, Vec3{1, 2, 3}}, "Position", "Motion"));
    REQUIRE(timeline.set(emission, {2, 3.0F}, "Emission", "Surface"));
    REQUIRE(timeline.set(emission, {0, 1.0F}));
    REQUIRE(timeline.set(emission, {1, 2.0F}));
    REQUIRE(timeline.tracks().size() == 4);
    CHECK(timeline.tracks()[0].target == emission);
    CHECK(timeline.tracks()[1].target == position);
    CHECK(timeline.tracks()[2].target == Target{1, "z"});
    CHECK(timeline.tracks()[3].target == Target{3, "a"});
    const auto* track = timeline.find(emission);
    REQUIRE(track);
    CHECK(track->label == "Emission");
    CHECK(track->layer == "Surface");
    REQUIRE(track->keys.size() == 3);
    CHECK(track->keys[0].time == 0.0F);
    CHECK(track->keys[1].time == 1.0F);
    CHECK(track->keys[2].time == 2.0F);
    REQUIRE(timeline.set(emission, {1, 8.0F, linear}, "Brightness", "Effects"));
    track = timeline.find(emission);
    REQUIRE(track);
    CHECK(track->keys.size() == 3);
    CHECK(track->keys[1] == Keyframe{1, 8.0F, linear});
    CHECK(track->label == "Brightness");
    CHECK(track->layer == "Effects");
    Timeline copy = timeline;
    CHECK(copy == timeline);
    REQUIRE(copy.erase(emission, 1));
    CHECK(copy != timeline);
}

TEST_CASE("arriving key owns interpolation and exact keys own their values", "[timeline][sample]")
{
    Timeline timeline;
    REQUIRE(timeline.set(emission, {2, 10.0F, linear}));
    REQUIRE(timeline.set(emission, {6, 30.0F, hold}));
    REQUIRE(timeline.set(emission, {10, 50.0F, linear}));
    CHECK_FALSE(timeline.sample(emission, 0));
    CHECK_FALSE(timeline.sample(emission, std::nextafter(2.0F, 0.0F)));
    CHECK(sampled<f32>(timeline, emission, 2) == 10.0F);
    CHECK(sampled<f32>(timeline, emission, 4) == 10.0F); // departing key is linear, arriving key is hold
    CHECK(sampled<f32>(timeline, emission, std::nextafter(6.0F, 0.0F)) == 10.0F);
    CHECK(sampled<f32>(timeline, emission, 6) == 30.0F);
    CHECK(sampled<f32>(timeline, emission, 7) == Approx(35));
    CHECK(sampled<f32>(timeline, emission, 8) == Approx(40));
    CHECK(sampled<f32>(timeline, emission, 10) == 50.0F);
    CHECK(sampled<f32>(timeline, emission, max_time) == 50.0F);
    CHECK_FALSE(timeline.sample(emission, -1));
    CHECK_FALSE(timeline.sample(emission, max_time + 1));
    CHECK_FALSE(timeline.sample(emission, std::numeric_limits<f32>::infinity()));
    CHECK_FALSE(timeline.sample(emission, std::numeric_limits<f32>::quiet_NaN()));
}

TEST_CASE("vector interpolation is componentwise and layers do not mute sampling", "[timeline][sample]")
{
    Timeline timeline;
    REQUIRE(timeline.set(position, {0, Vec3{-2, 4, 10}}, "Position", "Hidden in UI"));
    REQUIRE(timeline.set(position, {4, Vec3{6, 0, -10}, linear}));
    CHECK(sampled<Vec3>(timeline, position, 1) == Vec3{0, 3, 5});
    CHECK(sampled<Vec3>(timeline, position, 2) == Vec3{2, 2, 0});
    CHECK(sampled<Vec3>(timeline, position, 4) == Vec3{6, 0, -10});
    CHECK(sampled<Vec3>(timeline, position, 20) == Vec3{6, 0, -10});
    auto changed = copy_tracks(timeline);
    changed[0].label.clear();
    changed[0].layer = "Unrelated category";
    Timeline other;
    REQUIRE(other.replace(std::move(changed)));
    CHECK(other.sample(position, 2) == timeline.sample(position, 2));
}

TEST_CASE("discrete timeline values step without numerical or implicit type conversions", "[timeline][sample]")
{
    Timeline timeline;
    const std::vector<std::pair<Value, Value>> pairs{
        {false, true}, {i32{-8}, i32{9}}, {u32{1}, std::numeric_limits<u32>::max()},
        {std::string{"before"}, std::string{"after\0value", 11}},
    };
    for (std::size_t index = 0; index < pairs.size(); ++index) {
        const Target target{static_cast<u64>(index + 1), "value"};
        REQUIRE(timeline.set(target, {1, pairs[index].first}));
        REQUIRE(timeline.set(target, {3, pairs[index].second}));
        CHECK_FALSE(timeline.sample(target, 0));
        CHECK(timeline.sample(target, 1) == pairs[index].first);
        CHECK(timeline.sample(target, 2) == pairs[index].first);
        CHECK(timeline.sample(target, std::nextafter(3.0F, 0.0F)) == pairs[index].first);
        CHECK(timeline.sample(target, 3) == pairs[index].second);
        CHECK(timeline.sample(target, 9) == pairs[index].second);
        const Timeline before = timeline;
        const auto result = timeline.set(target, {4, pairs[index].second, linear});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == ErrorCode::invalid_interpolation);
        CHECK(timeline == before);
    }
}

TEST_CASE("timeline interpolation remains finite for extreme valid floats", "[timeline][sample]")
{
    Timeline timeline;
    constexpr f32 large = std::numeric_limits<f32>::max();
    REQUIRE(timeline.set(emission, {0, -large}));
    REQUIRE(timeline.set(emission, {2, large, linear}));
    CHECK(sampled<f32>(timeline, emission, 1) == 0.0F);
    CHECK(std::isfinite(sampled<f32>(timeline, emission, 0.5F)));
    CHECK(std::isfinite(sampled<f32>(timeline, emission, 1.5F)));
    const f32 tiny = std::numeric_limits<f32>::denorm_min();
    REQUIRE(timeline.set(position, {0, Vec3{-large, large, 0}}));
    REQUIRE(timeline.set(position, {tiny * 2, Vec3{large, -large, 2}, linear}));
    CHECK(sampled<Vec3>(timeline, position, tiny) == Vec3{0, 0, 1});
}

TEST_CASE("timeline set validation is atomic and preserves metadata on failure", "[timeline][validation]")
{
    Timeline timeline;
    REQUIRE(timeline.set(emission, {0, 1.0F}, "Original", "Layer"));
    const auto before = timeline;
    const auto rejected = [&](Target target, Keyframe key, ErrorCode code,
                              std::string label = {}, std::string layer = {}) {
        const auto result = timeline.set(std::move(target), std::move(key), std::move(label), std::move(layer));
        REQUIRE_FALSE(result);
        CHECK(result.error().code == code);
        CHECK_FALSE(result.error().message.empty());
        CHECK(timeline == before);
    };
    rejected({0, "x"}, {0, 1.0F}, ErrorCode::invalid_target);
    rejected({1, ""}, {0, 1.0F}, ErrorCode::invalid_target);
    rejected({1, std::string(max_property_bytes + 1, 'a')}, {0, 1.0F}, ErrorCode::invalid_target);
    rejected({1, std::string("bad\0key", 7)}, {0, 1.0F}, ErrorCode::invalid_target);
    rejected(emission, {1, 3.0F}, ErrorCode::invalid_metadata, std::string(max_label_bytes + 1, 'x'));
    rejected(emission, {1, 3.0F}, ErrorCode::invalid_metadata, {}, std::string(max_layer_bytes + 1, 'x'));
    rejected(emission, {1, 3.0F}, ErrorCode::invalid_metadata, std::string("bad\0label", 9));
    rejected(emission, {1, 3.0F}, ErrorCode::invalid_metadata, {}, std::string("bad\0layer", 9));
    for (const f32 time : {-1.0F, max_time + 1, std::numeric_limits<f32>::infinity(),
                           -std::numeric_limits<f32>::infinity(), std::numeric_limits<f32>::quiet_NaN()}) {
        rejected(emission, {time, 3.0F}, ErrorCode::invalid_time);
    }
    rejected(emission, {1, std::numeric_limits<f32>::infinity()}, ErrorCode::invalid_value);
    rejected(emission, {1, std::numeric_limits<f32>::quiet_NaN()}, ErrorCode::invalid_value);
    rejected(position, {1, Vec3{0, std::numeric_limits<f32>::quiet_NaN(), 0}}, ErrorCode::invalid_value);
    rejected(position, {1, Vec3{0, 0, std::numeric_limits<f32>::infinity()}}, ErrorCode::invalid_value);
    rejected({2, "text"}, {1, std::string(max_string_value_bytes + 1, 'x')}, ErrorCode::invalid_value);
    rejected(emission, {1, 3.0F, static_cast<Interpolation>(123)}, ErrorCode::invalid_interpolation);
    rejected(emission, {0, i32{3}}, ErrorCode::type_mismatch, "Changed", "Changed");
    rejected(emission, {1, Vec3{1, 2, 3}}, ErrorCode::type_mismatch);
    rejected({2, "discrete"}, {0, true, linear}, ErrorCode::invalid_interpolation);
    REQUIRE(timeline.set({3, std::string(max_property_bytes, 'p')},
                         {max_time, std::string(max_string_value_bytes, 's')},
                         std::string(max_label_bytes, 'l'), std::string(max_layer_bytes, 'c')));
}

TEST_CASE("replace validates all tracks before committing and canonicalizes order", "[timeline][validation]")
{
    Timeline timeline;
    REQUIRE(timeline.set(emission, {0, 1.0F}));
    const Timeline before = timeline;
    auto invalid = std::vector<Track>{
        {position, "Position", "Movement", {{2, Vec3{1, 2, 3}}, {1, Vec3{4, 5, 6}}}},
        {emission, "Emission", "Surface", {{2, 2.0F}, {0, 1.0F}}},
    };
    auto duplicate = invalid;
    duplicate.push_back(duplicate.front());
    auto result = timeline.replace(std::move(duplicate));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::duplicate_target);
    CHECK(timeline == before);
    auto bad_keys = invalid;
    bad_keys[1].keys.push_back({-0.0F, 8.0F});
    result = timeline.replace(std::move(bad_keys));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::duplicate_time);
    CHECK(timeline == before);
    auto empty = invalid;
    empty[0].keys.clear();
    result = timeline.replace(std::move(empty));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::empty_track);
    CHECK(timeline == before);
    auto mismatch = invalid;
    mismatch[1].keys[1].value = true;
    result = timeline.replace(std::move(mismatch));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::type_mismatch);
    CHECK(timeline == before);
    auto nonfinite = invalid;
    nonfinite[0].keys[0].time = std::numeric_limits<f32>::quiet_NaN();
    result = timeline.replace(std::move(nonfinite));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::invalid_time);
    CHECK(timeline == before);
    REQUIRE(timeline.replace(std::move(invalid)));
    REQUIRE(timeline.tracks().size() == 2);
    CHECK(timeline.tracks()[0].target == emission);
    CHECK(timeline.tracks()[1].target == position);
    CHECK(timeline.find(emission)->keys.front().time == 0.0F);
    CHECK(timeline.find(position)->keys.front().time == 1.0F);
    auto reversed = copy_tracks(timeline);
    std::reverse(reversed.begin(), reversed.end());
    for (auto& track : reversed) std::reverse(track.keys.begin(), track.keys.end());
    Timeline equivalent;
    REQUIRE(equivalent.replace(std::move(reversed)));
    CHECK(timeline == equivalent);
    REQUIRE(timeline.replace({}));
    CHECK(timeline.tracks().empty());
}

TEST_CASE("keyframe moves preserve value and incoming mode while retaining sorted order", "[timeline][edit]")
{
    Timeline timeline;
    REQUIRE(timeline.set(emission, {1, 10.0F, hold}));
    REQUIRE(timeline.set(emission, {3, 30.0F, linear}));
    REQUIRE(timeline.set(emission, {5, 50.0F, hold}));
    REQUIRE(timeline.move(emission, 3, 2)); // same slot, smaller time
    CHECK(timeline.find(emission)->keys[1] == Keyframe{2, 30.0F, linear});
    REQUIRE(timeline.move(emission, 2, 4)); // same slot, larger time
    CHECK(timeline.find(emission)->keys[1] == Keyframe{4, 30.0F, linear});
    REQUIRE(timeline.move(emission, 4, 0)); // rotate to beginning
    CHECK(timeline.find(emission)->keys[0] == Keyframe{0, 30.0F, linear});
    REQUIRE(timeline.move(emission, 0, 8)); // rotate to end
    CHECK(timeline.find(emission)->keys[2] == Keyframe{8, 30.0F, linear});
    CHECK(sampled<f32>(timeline, emission, 6.5F) == Approx(40));
    const Timeline before = timeline;
    REQUIRE(timeline.move(emission, 8, 8));
    CHECK(timeline == before);
    auto result = timeline.move(emission, 8, 1);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::duplicate_time);
    CHECK(timeline == before);
    result = timeline.move(emission, 7, 3);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::not_found);
    CHECK(timeline == before);
    result = timeline.move(position, 1, 3);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::not_found);
    CHECK(timeline == before);
    result = timeline.move(emission, 1, -1);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::invalid_time);
    CHECK(timeline == before);
    result = timeline.move(emission, std::numeric_limits<f32>::quiet_NaN(), 2);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::invalid_time);
    CHECK(timeline == before);
}

TEST_CASE("keyframe erasure removes empty tracks and permits a new type afterward", "[timeline][edit]")
{
    Timeline timeline;
    REQUIRE(timeline.set(emission, {-0.0F, 1.0F}));
    CHECK_FALSE(std::signbit(timeline.find(emission)->keys[0].time));
    REQUIRE(timeline.set(emission, {2, 3.0F}));
    REQUIRE(timeline.set(position, {0, Vec3{1, 2, 3}}));
    CHECK_FALSE(timeline.erase(emission, 1));
    CHECK_FALSE(timeline.erase(emission, -1));
    CHECK_FALSE(timeline.erase(emission, std::numeric_limits<f32>::quiet_NaN()));
    CHECK_FALSE(timeline.erase({9, "absent"}, 0));
    REQUIRE(timeline.erase(emission, 0));
    CHECK(timeline.find(emission)->keys.size() == 1);
    CHECK_FALSE(timeline.sample(emission, 1));
    REQUIRE(timeline.erase(emission, 2));
    CHECK(timeline.find(emission) == nullptr);
    CHECK(timeline.tracks().size() == 1);
    REQUIRE(timeline.set(emission, {0, false}));
    CHECK(sampled<bool>(timeline, emission, 0) == false);
    REQUIRE(timeline.move(position, 0, 2));
    REQUIRE(timeline.move(position, 2, -0.0F));
    CHECK_FALSE(std::signbit(timeline.find(position)->keys[0].time));
}

TEST_CASE("timeline track and per-track limits are inclusive and transactional", "[timeline][limits]")
{
    Timeline timeline;
    std::vector<Track> full;
    for (std::size_t i = 0; i < max_tracks; ++i) {
        full.push_back({{static_cast<u64>(i + 1), "value"}, {}, {}, {{0, 1.0F}}});
    }
    REQUIRE(timeline.replace(std::move(full)));
    CHECK(timeline.tracks().size() == max_tracks);
    auto before = timeline;
    auto result = timeline.set({max_tracks + 1, "value"}, {0, 1.0F});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::limit_exceeded);
    CHECK(timeline == before);
    REQUIRE(timeline.set({1, "value"}, {0, 2.0F}));
    auto too_many = copy_tracks(timeline);
    too_many.push_back({{max_tracks + 1, "value"}, {}, {}, {{0, 1.0F}}});
    before = timeline;
    result = timeline.replace(std::move(too_many));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::limit_exceeded);
    CHECK(timeline == before);

    REQUIRE(timeline.replace({many_keys(1, max_keys_per_track)}));
    before = timeline;
    result = timeline.set({1, "number"}, {static_cast<f32>(max_keys_per_track), 2.0F});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::limit_exceeded);
    CHECK(timeline == before);
    REQUIRE(timeline.set({1, "number"}, {0, 8.0F}));
    before = timeline;
    result = timeline.replace({many_keys(1, max_keys_per_track + 1)});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::limit_exceeded);
    CHECK(timeline == before);
}

TEST_CASE("total key limit spans tracks and deleting a key releases capacity", "[timeline][limits]")
{
    Timeline timeline;
    std::vector<Track> tracks;
    for (std::size_t i = 0; i < max_total_keys / max_keys_per_track; ++i) {
        tracks.push_back(many_keys(static_cast<u64>(i + 1), max_keys_per_track));
    }
    REQUIRE(timeline.replace(std::move(tracks)));
    const Timeline before = timeline;
    auto result = timeline.set({10, "another"}, {0, 1.0F});
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::limit_exceeded);
    CHECK(timeline == before);
    auto overflow = copy_tracks(timeline);
    overflow.push_back({{10, "another"}, {}, {}, {{0, 1.0F}}});
    result = timeline.replace(std::move(overflow));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::limit_exceeded);
    CHECK(timeline == before);
    REQUIRE(timeline.set({1, "number"}, {0, -8.0F})); // replacing a key consumes no capacity
    REQUIRE(timeline.erase({1, "number"}, 0));
    REQUIRE(timeline.set({10, "another"}, {0, 1.0F}));
    CHECK(timeline.tracks().size() == before.tracks().size() + 1);
    REQUIRE(timeline.move({1, "number"}, 1, max_time));
    CHECK(sampled<f32>(timeline, {1, "number"}, max_time) == 1.0F);
}

TEST_CASE("single-track replacement preserves unrelated key storage and canonical ordering", "[timeline]")
{
    Timeline timeline;
    REQUIRE(timeline.set(emission, {0, 1.F}, "Emission", "Lighting"));
    REQUIRE(timeline.set(position, {0, Vec3{}}, "Position", "Motion"));
    const auto* unrelated = timeline.find(emission)->keys.data();
    Track replacement{position, "", "", {{4, Vec3{4, 3, 2}, linear}, {-0.F, Vec3{1, 2, 3}, hold}}};
    REQUIRE(timeline.replace_track(std::move(replacement)));
    REQUIRE(timeline.find(position));
    CHECK(timeline.find(position)->label.empty());
    CHECK(timeline.find(position)->layer.empty());
    CHECK(timeline.find(position)->keys[0].time == 0.F);
    CHECK_FALSE(std::signbit(timeline.find(position)->keys[0].time));
    CHECK(timeline.find(position)->keys[1].time == 4.F);
    CHECK(timeline.find(emission)->keys.data() == unrelated);
    REQUIRE(timeline.replace_track({{2, "visible"}, "Visibility", "", {{0, true, hold}}}));
    CHECK(timeline.find(emission)->keys.data() == unrelated);
    REQUIRE(timeline.erase(position));
    CHECK_FALSE(timeline.find(position));
    CHECK_FALSE(timeline.erase(position));
    CHECK(timeline.find(emission)->keys.data() == unrelated);
    REQUIRE(timeline.replace({})); // The preexisting empty-list clear stays unambiguous.
}

TEST_CASE("single-track validation is atomic for malformed replacements", "[timeline]")
{
    Timeline timeline;
    REQUIRE(timeline.set(emission, {0, 1.F}));
    const auto original = timeline;
    const auto* keys = timeline.find(emission)->keys.data();
    Track replacement{emission, "", "", {{0, 2.F, linear}}};
    SECTION("invalid target") { replacement.target.object = 0; }
    SECTION("invalid metadata") { replacement.label.assign(257, 'a'); }
    SECTION("empty track") { replacement.keys.clear(); }
    SECTION("duplicate timestamp") { replacement.keys.push_back({0, 3.F}); }
    SECTION("mixed types") { replacement.keys.push_back({1, Vec3{}}); }
    SECTION("invalid time") { replacement.keys[0].time = -1; }
    SECTION("invalid value") { replacement.keys[0].value = std::numeric_limits<f32>::infinity(); }
    SECTION("invalid interpolation") { replacement.keys[0].incoming = static_cast<Interpolation>(7); }
    CHECK_FALSE(timeline.replace_track(std::move(replacement)));
    CHECK(timeline == original);
    CHECK(timeline.find(emission)->keys.data() == keys);
}

TEST_CASE("single-track replacement accounts for removed keys before enforcing limits", "[timeline][limits]")
{
    Timeline timeline;
    for (std::size_t i = 0; i < max_total_keys / max_keys_per_track; ++i)
        REQUIRE(timeline.replace_track(many_keys(static_cast<u64>(i + 1), max_keys_per_track)));
    REQUIRE(timeline.replace_track(many_keys(1, max_keys_per_track)));
    const auto original = timeline;
    CHECK_FALSE(timeline.replace_track({{77, "extra"}, "", "", {{0, 1.F}}}));
    CHECK(timeline == original);
    REQUIRE(timeline.replace_track(many_keys(1, max_keys_per_track - 1)));
    REQUIRE(timeline.replace_track({{77, "extra"}, "", "", {{0, 1.F}}}));
    CHECK_FALSE(timeline.replace_track(many_keys(1, max_keys_per_track)));
    REQUIRE(timeline.erase({77, "extra"}));
    REQUIRE(timeline.replace_track(many_keys(1, max_keys_per_track)));
}
