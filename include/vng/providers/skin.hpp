#pragma once

#include <utility>

#include <vng/rig/skin_binding.hpp>

namespace vng::providers {

template<class Mesh>
class Skin final {
public:
    explicit Skin(rig::SkinBinding<Mesh> binding) : binding_(std::move(binding)) {}

    [[nodiscard]] std::expected<rig::SkinBinding<Mesh>, rig::Diagnostic> provide() const
    {
        if (auto valid = binding_.validate(); !valid) {
            return std::unexpected(std::move(valid.error()));
        }
        return binding_;
    }

private:
    rig::SkinBinding<Mesh> binding_;
};

template<class Mesh>
[[nodiscard]] auto skin(rig::SkinBinding<Mesh> binding)
{
    return Skin<Mesh>{std::move(binding)};
}

} // namespace vng::providers
