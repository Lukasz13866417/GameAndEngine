#pragma once
#include "project.hpp"
#include <vng/content/document.hpp>
#include <ostream>

namespace editor_example {
struct EffectPreset {
    std::string name;
    SunSettings settings;
    friend bool operator==(const EffectPreset&, const EffectPreset&) = default;
};
// .veffect uses the existing vscene document grammar; no shader code is loaded
// or executed. Effect types are validated against the compiled implementations.
[[nodiscard]] bool valid_effect_settings(const SunSettings&);
[[nodiscard]] SunSettings read_effect_settings(vng::content::Reader);
void write_effect_settings(std::ostream&, const SunSettings&);
[[nodiscard]] vng::content::Result<EffectPreset> read_effect_preset(const std::filesystem::path&);
[[nodiscard]] vng::content::Result<EffectPreset> effect_preset(const State&, vng::u32 instance);
[[nodiscard]] vng::content::Result<std::string> encode_effect_preset(const EffectPreset&);
}
