#pragma once

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

#include <vng/analysis/types.hpp>
#include <vng/core/type_name.hpp>
#include <vng/gfx/semantic.hpp>

namespace vng::analysis {

// A request describes the evidence wanted by a caller, independently of how a
// graphics backend obtains it. In particular, it contains no framebuffer,
// attachment, shader-language, stage, or synchronization details.
enum class CapturePreset {
    standard,
    diagnostic,
    probe,
};

enum class CaptureScope {
    full_frame,
    pixel,
};

enum class DerivedVisualization {
    coverage,
    item,
    primitive,
};

struct ObservationRequest final {
    std::type_index semantic_type{typeid(void)};
    std::type_index value_type{typeid(void)};
    std::string semantic_name;
    std::string value_name;

    friend bool operator==(const ObservationRequest&,
                           const ObservationRequest&) = default;

    [[nodiscard]] std::string channel_name() const
    {
        return "shader/" + semantic_name;
    }
};

template<gfx::SemanticType Semantic>
[[nodiscard]] std::string observation_channel_name(Semantic = {})
{
    using Tag = std::remove_cvref_t<Semantic>;
    return "shader/" + std::string{core::type_name<Tag>()};
}

class CaptureRequest final {
public:
    CapturePreset preset{CapturePreset::standard};
    CaptureScope scope{CaptureScope::full_frame};
    std::optional<Pixel> probe_pixel;

    // These are logical evidence requirements. A backend is free to obtain
    // them in one pass, several passes, or from an existing CPU-side result.
    bool color{true};
    bool device_depth{true};
    bool surface_key{true};

    bool coverage_visualization{};
    bool item_visualization{};
    bool primitive_visualization{};

    CaptureRequest() = default;

    [[nodiscard]] static CaptureRequest standard()
    {
        return {};
    }

    [[nodiscard]] static CaptureRequest diagnostic()
    {
        CaptureRequest result;
        result.preset = CapturePreset::diagnostic;
        result.coverage_visualization = true;
        result.item_visualization = true;
        result.primitive_visualization = true;
        return result;
    }

    [[nodiscard]] static CaptureRequest probe(Pixel pixel)
    {
        CaptureRequest result;
        result.preset = CapturePreset::probe;
        result.scope = CaptureScope::pixel;
        result.probe_pixel = pixel;
        return result;
    }

    // Duplicate semantic requests are idempotent. This keeps composable setup
    // code deterministic without turning the fluent call into a fallible API.
    template<gfx::SemanticType Semantic>
    CaptureRequest& observe(Semantic) &
    {
        using Tag = std::remove_cvref_t<Semantic>;
        if (!observes(typeid(Tag))) {
            observations_.push_back(ObservationRequest{
                .semantic_type = typeid(Tag),
                .value_type = typeid(gfx::semantic_value_t<Tag>),
                .semantic_name = std::string{core::type_name<Tag>()},
                .value_name = std::string{
                    core::type_name<gfx::semantic_value_t<Tag>>()},
            });
        }
        return *this;
    }

    template<gfx::SemanticType Semantic>
    CaptureRequest&& observe(Semantic semantic) &&
    {
        observe(semantic);
        return std::move(*this);
    }

    template<gfx::SemanticType Semantic>
    CaptureRequest& observe() &
    {
        return observe(Semantic{});
    }

    template<gfx::SemanticType Semantic>
    CaptureRequest&& observe() &&
    {
        observe(Semantic{});
        return std::move(*this);
    }

    [[nodiscard]] bool observes(std::type_index semantic) const noexcept
    {
        return std::ranges::any_of(
            observations_,
            [semantic](const ObservationRequest& observation) {
                return observation.semantic_type == semantic;
            });
    }

    template<gfx::SemanticType Semantic>
    [[nodiscard]] bool observes(Semantic) const noexcept
    {
        return observes(typeid(std::remove_cv_t<Semantic>));
    }

    [[nodiscard]] std::span<const ObservationRequest> observations() const noexcept
    {
        return observations_;
    }

    [[nodiscard]] constexpr bool requests(Channel channel) const noexcept
    {
        switch (channel) {
        case Channel::color:
            return color;
        case Channel::device_depth:
            return device_depth;
        case Channel::surface_key:
            return surface_key;
        }
        return false;
    }

    [[nodiscard]] constexpr bool requests(
        DerivedVisualization visualization) const noexcept
    {
        switch (visualization) {
        case DerivedVisualization::coverage:
            return coverage_visualization;
        case DerivedVisualization::item:
            return item_visualization;
        case DerivedVisualization::primitive:
            return primitive_visualization;
        }
        return false;
    }

    friend bool operator==(const CaptureRequest& left,
                           const CaptureRequest& right)
    {
        return left.preset == right.preset
            && left.scope == right.scope
            && left.probe_pixel == right.probe_pixel
            && left.color == right.color
            && left.device_depth == right.device_depth
            && left.surface_key == right.surface_key
            && left.coverage_visualization == right.coverage_visualization
            && left.item_visualization == right.item_visualization
            && left.primitive_visualization == right.primitive_visualization
            && left.observations_ == right.observations_;
    }

private:
    std::vector<ObservationRequest> observations_;
};

} // namespace vng::analysis
