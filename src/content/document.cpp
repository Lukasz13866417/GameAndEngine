#include <vng/content/detail/document_data.hpp>

#include <algorithm>
#include <cmath>

namespace vng::content {
namespace {

std::string_view kind_name(ValueKind kind) noexcept
{
    switch (kind) {
    case ValueKind::invalid: return "invalid view";
    case ValueKind::null: return "null";
    case ValueKind::boolean: return "boolean";
    case ValueKind::integer: return "signed integer";
    case ValueKind::unsigned_integer: return "unsigned integer";
    case ValueKind::number: return "number";
    case ValueKind::string: return "string";
    case ValueKind::array: return "array";
    case ValueKind::object: return "object";
    }
    return "unknown";
}

bool simple_key(std::string_view key) noexcept
{
    if (key.empty()) return false;
    const auto letter = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    };
    if (!letter(key.front())) return false;
    return std::ranges::all_of(key, [&](char c) { return letter(c) || (c >= '0' && c <= '9'); });
}

std::string property_suffix(std::string_view key)
{
    if (simple_key(key)) return "." + std::string{key};
    std::string result{"[\""};
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char c : key) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 0x20) {
            result += "\\u00";
            result += hex[c >> 4U];
            result += hex[c & 15U];
        } else result += static_cast<char>(c);
    }
    result += "\"]";
    return result;
}

std::string append_property(std::string path, std::string_view key)
{
    auto suffix = property_suffix(key);
    if (path == "$") {
        if (suffix.starts_with('.')) suffix.erase(0, 1);
        return suffix;
    }
    return path + suffix;
}

Diagnostic mismatch(NodeView node, std::string_view expected)
{
    return node.error(node.valid() ? ErrorCode::type_mismatch : ErrorCode::invalid_document,
        "Expected " + std::string{expected} + ", found " + std::string{kind_name(node.kind())});
}

} // namespace

bool NodeView::valid() const noexcept
{
    return data_ && index_ < data_->nodes.size();
}

ValueKind NodeView::kind() const noexcept
{
    return valid() ? data_->nodes[index_].kind : ValueKind::invalid;
}

std::optional<SourceLocation> NodeView::location() const noexcept
{
    return valid() ? std::optional{data_->nodes[index_].location} : std::nullopt;
}

std::string NodeView::property_path() const
{
    if (!valid()) return {};
    std::vector<std::string> suffixes;
    auto current = index_;
    while (data_->nodes[current].parent != document_detail::invalid_node) {
        const auto& node = data_->nodes[current];
        const auto& parent = data_->nodes[node.parent];
        if (parent.kind == ValueKind::object) {
            suffixes.push_back(property_suffix(node.key));
        } else {
            const auto found = std::ranges::find(parent.children, current);
            const auto offset = static_cast<std::size_t>(found - parent.children.begin());
            suffixes.push_back("[" + std::to_string(offset) + "]");
        }
        current = node.parent;
    }
    if (suffixes.empty()) return "$";
    std::string result;
    for (auto it = suffixes.rbegin(); it != suffixes.rend(); ++it) result += *it;
    if (result.starts_with('.')) result.erase(0, 1);
    return result;
}

Diagnostic NodeView::error(ErrorCode code, std::string message) const
{
    return Diagnostic{
        .code = code,
        .message = std::move(message),
        .location = location(),
        .path = data_ ? data_->source_path : std::filesystem::path{},
        .notes = {},
        .property_path = property_path(),
    };
}

Result<std::optional<NodeView>> NodeView::find(std::string_view name) const
{
    if (kind() != ValueKind::object) return std::unexpected(mismatch(*this, "object"));
    for (const auto child : data_->nodes[index_].children) {
        if (data_->nodes[child].key == name) return std::optional{NodeView{data_, child}};
    }
    return std::optional<NodeView>{};
}

Result<NodeView> NodeView::child(std::string_view name) const
{
    auto found = find(name);
    if (!found) return std::unexpected(std::move(found.error()));
    if (*found) return **found;
    auto diagnostic = error(ErrorCode::missing_property, "Required property is missing");
    diagnostic.property_path = append_property(std::move(diagnostic.property_path), name);
    return std::unexpected(std::move(diagnostic));
}

Result<ArrayView> NodeView::elements() const
{
    if (kind() != ValueKind::array) return std::unexpected(mismatch(*this, "array"));
    return ArrayView{*this, data_, data_->nodes[index_].children};
}

Result<ObjectView> NodeView::members() const
{
    if (kind() != ValueKind::object) return std::unexpected(mismatch(*this, "object"));
    return ObjectView{data_, data_->nodes[index_].children};
}

Result<std::string_view> NodeView::borrow_string() const
{
    if (kind() != ValueKind::string) return std::unexpected(mismatch(*this, "string"));
    return std::get<std::string>(data_->nodes[index_].scalar);
}

Result<bool> NodeView::boolean() const
{
    if (kind() != ValueKind::boolean) return std::unexpected(mismatch(*this, "boolean"));
    return std::get<bool>(data_->nodes[index_].scalar);
}

Result<i64> NodeView::signed_integer() const
{
    if (kind() == ValueKind::integer) return std::get<i64>(data_->nodes[index_].scalar);
    if (kind() == ValueKind::unsigned_integer) {
        const auto value = std::get<u64>(data_->nodes[index_].scalar);
        if (value <= static_cast<u64>(std::numeric_limits<i64>::max())) return static_cast<i64>(value);
        return std::unexpected(error(ErrorCode::number_out_of_range, "Integer exceeds the signed 64-bit range"));
    }
    if (kind() == ValueKind::number) {
        const auto value = std::get<f64>(data_->nodes[index_].scalar);
        if (std::trunc(value) != value)
            return std::unexpected(error(ErrorCode::type_mismatch, "A fractional number cannot be read as an integer"));
        // Compare with exact powers of two; converting INT64_MAX to double
        // rounds upward and would admit an undefined out-of-range cast.
        const auto bound = std::ldexp(1.0, 63);
        if (value < -bound || value >= bound)
            return std::unexpected(error(ErrorCode::number_out_of_range, "Number exceeds the signed 64-bit range"));
        return static_cast<i64>(value);
    }
    return std::unexpected(mismatch(*this, "integer"));
}

Result<u64> NodeView::unsigned_integer() const
{
    if (kind() == ValueKind::unsigned_integer) return std::get<u64>(data_->nodes[index_].scalar);
    if (kind() == ValueKind::integer) {
        const auto value = std::get<i64>(data_->nodes[index_].scalar);
        if (value >= 0) return static_cast<u64>(value);
        return std::unexpected(error(ErrorCode::number_out_of_range, "A negative integer cannot be read as unsigned"));
    }
    if (kind() == ValueKind::number) {
        const auto value = std::get<f64>(data_->nodes[index_].scalar);
        if (std::trunc(value) != value)
            return std::unexpected(error(ErrorCode::type_mismatch, "A fractional number cannot be read as an integer"));
        if (value < 0 || value >= std::ldexp(1.0, 64))
            return std::unexpected(error(ErrorCode::number_out_of_range, "Number exceeds the unsigned 64-bit range"));
        return static_cast<u64>(value);
    }
    return std::unexpected(mismatch(*this, "integer"));
}

Result<long double> NodeView::number() const
{
    switch (kind()) {
    case ValueKind::integer: return static_cast<long double>(std::get<i64>(data_->nodes[index_].scalar));
    case ValueKind::unsigned_integer: return static_cast<long double>(std::get<u64>(data_->nodes[index_].scalar));
    case ValueKind::number: return static_cast<long double>(std::get<f64>(data_->nodes[index_].scalar));
    default: return std::unexpected(mismatch(*this, "number"));
    }
}

NodeView ArrayView::iterator::operator*() const noexcept
{
    return document_detail::DocumentAccess::node(data, children[index]);
}

Result<NodeView> ArrayView::at(std::size_t index) const
{
    if (!parent_.valid())
        return std::unexpected(parent_.error(ErrorCode::invalid_document, "Invalid array view"));
    if (index >= size()) {
        auto diagnostic = parent_.error(ErrorCode::index_out_of_range, "Array index is out of range");
        diagnostic.property_path += "[" + std::to_string(index) + "]";
        return std::unexpected(std::move(diagnostic));
    }
    return document_detail::DocumentAccess::node(data_, children_[index]);
}

MemberView ObjectView::iterator::operator*() const noexcept
{
    const auto id = children[index];
    return {data->nodes[id].key, document_detail::DocumentAccess::node(data, id)};
}

NodeView Document::root() const noexcept
{
    return NodeView{document_detail::DocumentAccess::node(data_.get(), 0)};
}

std::string_view Document::kind() const noexcept
{
    return data_ ? std::string_view{data_->kind} : std::string_view{};
}

DocumentVersion Document::version() const noexcept
{
    return data_ ? data_->version : DocumentVersion{0, 0};
}

const std::filesystem::path& Document::source_path() const noexcept
{
    static const std::filesystem::path empty;
    return data_ ? data_->source_path : empty;
}

} // namespace vng::content
