#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

#include <vng/opengl/diagnostic.hpp>

namespace vng::glsl { struct StageSource; }

namespace vng::opengl {

namespace detail { struct ContextState; }
class Device;
class Program;

enum class ShaderStage { vertex, fragment };

struct VertexInputMetadata final {
    std::type_index semantic_type{typeid(void)};
    std::string semantic_name{};
    std::optional<std::uint32_t> location{};

    friend bool operator==(const VertexInputMetadata&,
                           const VertexInputMetadata&) = default;
};

struct ShaderSource;
[[nodiscard]] inline ShaderSource from_glsl(const glsl::StageSource& source);

struct ShaderSource final {
    ShaderStage stage{ShaderStage::vertex};
    std::string text{};
    std::vector<SourceMapEntry> source_map{};

private:
    // Present only when source was emitted from a typed vng shader. Raw GLSL
    // remains supported, but cannot forge metadata-driven mesh binding. Keep
    // the original text as a seal so editing emitted source invalidates its
    // semantic contract before compilation.
    std::optional<std::vector<VertexInputMetadata>> vertex_inputs_{};
    std::string typed_source_text_{};

    void set_typed_vertex_inputs(std::vector<VertexInputMetadata> inputs)
    {
        vertex_inputs_ = std::move(inputs);
        typed_source_text_ = text;
    }

    void validate_typed_metadata()
    {
        if (vertex_inputs_ && text != typed_source_text_) {
            vertex_inputs_.reset();
        }
        std::string{}.swap(typed_source_text_);
    }

    friend class Shader;
    friend class Program;
    friend ShaderSource from_glsl(const glsl::StageSource& source);
};

class Shader final {
public:
    static std::expected<Shader, Diagnostic> compile(
        const Device& device,
        ShaderSource source);

    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    ~Shader();

    [[nodiscard]] ShaderStage stage() const noexcept { return stage_; }
    [[nodiscard]] std::uint32_t native_handle() const noexcept { return handle_; }
    [[nodiscard]] const ShaderSource& source() const noexcept { return source_; }
    [[nodiscard]] const std::string& driver_log() const noexcept { return driver_log_; }
    [[nodiscard]] std::expected<void, Diagnostic> destroy();

private:
    Shader(
        std::shared_ptr<detail::ContextState> state,
        std::uint32_t handle,
        ShaderSource source,
        std::string driver_log) noexcept;
    void release_noexcept() noexcept;

    std::shared_ptr<detail::ContextState> state_;
    std::uint32_t handle_{};
    ShaderStage stage_{ShaderStage::vertex};
    ShaderSource source_;
    std::string driver_log_;

    friend class Program;
};

} // namespace vng::opengl
