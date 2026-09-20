#pragma once

#include <variant>
#include <vng/content/document.hpp>

namespace vng::content::document_detail {

using NodeId = std::size_t;
inline constexpr NodeId invalid_node = std::numeric_limits<NodeId>::max();

struct NodeData final {
    ValueKind kind{ValueKind::null};
    SourceLocation location{};
    SourceLocation key_location{};
    NodeId parent{invalid_node};
    std::string key{};
    std::variant<std::monostate, bool, i64, u64, f64, std::string> scalar{};
    std::vector<NodeId> children{};
};

struct DocumentData final {
    std::string kind{"vscene"};
    DocumentVersion version{};
    std::filesystem::path source_path{};
    std::vector<NodeData> nodes{};
};

struct DocumentAccess final {
    [[nodiscard]] static Document make(std::shared_ptr<const DocumentData> data)
    { return Document{std::move(data)}; }
    [[nodiscard]] static const DocumentData* data(const Document& document) noexcept
    { return document.data_.get(); }
    [[nodiscard]] static NodeView node(const DocumentData* data, NodeId index) noexcept
    { return NodeView{data, index}; }
};

} // namespace vng::content::document_detail
