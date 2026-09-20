#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <vng/content/diagnostic.hpp>
#include <vng/core/types.hpp>

namespace vng::content {

enum class ValueKind {
    invalid, null, boolean, integer, unsigned_integer, number, string, array, object,
};

struct DocumentVersion final {
    u32 major{1};
    u32 minor{};
    friend constexpr bool operator==(DocumentVersion, DocumentVersion) = default;
};

struct DocumentLimits final {
    std::size_t max_source_bytes{64U * 1024U * 1024U};
    std::size_t max_decoded_bytes{128U * 1024U * 1024U};
    std::size_t max_nodes{1024U * 1024U};
    std::size_t max_depth{128};
    std::size_t max_string_bytes{8U * 1024U * 1024U};
    std::size_t max_number_bytes{256};
};

struct DocumentReadOptions final {
    DocumentLimits limits{};
    std::filesystem::path source_path{};
};

struct DocumentWriteOptions final {
    DocumentLimits limits{};
};

template<class T>
using Type = std::type_identity<T>;

class Document;
class NodeView;
class ArrayView;
class ObjectView;
class Reader;

namespace document_detail {
struct DocumentData;
struct DocumentAccess;

// Only the scoped Reader uses this private control-flow exception. Low-level
// accessors return expected directly, and read() never catches arbitrary errors.
struct ReadFailure final { Diagnostic diagnostic; };

template<class T> T unwrap(Result<T> result)
{
    if (!result) throw ReadFailure{std::move(result.error())};
    return std::move(*result);
}

template<class T> struct IsExpected : std::false_type {};
template<class T, class E> struct IsExpected<std::expected<T, E>> : std::true_type {};

template<class T> struct Sequence { static constexpr bool supported = false; };
template<class T, std::size_t N> struct Sequence<std::array<T, N>> {
    using Element = T;
    static constexpr bool supported = true, dynamic = false;
    static constexpr std::size_t size = N;
};
template<class T, std::size_t N> struct Sequence<Vector<T, N>> {
    using Element = T;
    static constexpr bool supported = true, dynamic = false;
    static constexpr std::size_t size = N;
};
template<class T, class A> struct Sequence<std::vector<T, A>> {
    using Element = T;
    static constexpr bool supported = true, dynamic = true;
};
} // namespace document_detail

// A borrowed immutable node. All views/ranges/borrowed strings remain valid
// while any Document owning this snapshot lives. Moving a Document is safe;
// destroying its last owner invalidates its views, just like std::string_view.
class NodeView final {
public:
    NodeView() noexcept = default;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] ValueKind kind() const noexcept;
    [[nodiscard]] std::optional<SourceLocation> location() const noexcept;
    [[nodiscard]] std::string property_path() const;
    [[nodiscard]] Diagnostic error(ErrorCode code, std::string message) const;

    template<class T> [[nodiscard]] Result<T> as() const;
    template<class T> [[nodiscard]] Result<T> get(std::string_view name) const;
    template<class T> [[nodiscard]] Result<T> get_or(std::string_view name, T fallback) const;
    template<class T> [[nodiscard]] Result<std::optional<T>> optional(std::string_view name) const;

    [[nodiscard]] Result<NodeView> child(std::string_view name) const;
    [[nodiscard]] Result<ArrayView> elements() const;
    [[nodiscard]] Result<ObjectView> members() const;
    [[nodiscard]] Result<std::string_view> borrow_string() const;

    // Callback returns a plain owning value (or void). Decoding failures stop
    // immediately and become Result errors. Arbitrary callback exceptions and
    // side effects are NOT caught/rolled back. Decode CPU data before creating
    // resources or modifying existing application state.
    template<class Function> [[nodiscard]] auto read(Function&& function) const;

private:
    NodeView(const document_detail::DocumentData* data, std::size_t index) noexcept
        : data_(data), index_(index) {}
    [[nodiscard]] Result<std::optional<NodeView>> find(std::string_view name) const;
    [[nodiscard]] Result<bool> boolean() const;
    [[nodiscard]] Result<i64> signed_integer() const;
    [[nodiscard]] Result<u64> unsigned_integer() const;
    [[nodiscard]] Result<long double> number() const;
    const document_detail::DocumentData* data_{};
    std::size_t index_{};
    friend struct document_detail::DocumentAccess;
};

class ArrayView final : public std::ranges::view_interface<ArrayView> {
public:
    struct iterator final {
        using value_type = NodeView;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        const document_detail::DocumentData* data{};
        const std::size_t* children{};
        std::size_t index{};
        [[nodiscard]] NodeView operator*() const noexcept;
        iterator& operator++() noexcept { ++index; return *this; }
        iterator operator++(int) noexcept { auto previous = *this; ++*this; return previous; }
        friend bool operator==(const iterator&, const iterator&) = default;
    };
    ArrayView() noexcept = default;
    [[nodiscard]] iterator begin() const noexcept { return {data_, children_.data(), 0}; }
    [[nodiscard]] iterator end() const noexcept { return {data_, children_.data(), size()}; }
    [[nodiscard]] std::size_t size() const noexcept { return children_.size(); }
    [[nodiscard]] bool empty() const noexcept { return children_.empty(); }
    [[nodiscard]] Result<NodeView> at(std::size_t index) const;
private:
    ArrayView(NodeView parent, const document_detail::DocumentData* data,
        std::span<const std::size_t> children) noexcept
        : parent_(parent), data_(data), children_(children) {}
    NodeView parent_;
    const document_detail::DocumentData* data_{};
    std::span<const std::size_t> children_;
    friend class NodeView;
};

struct MemberView final {
    std::string_view name;
    NodeView value;
};

class ObjectView final : public std::ranges::view_interface<ObjectView> {
public:
    struct iterator final {
        using value_type = MemberView;
        using difference_type = std::ptrdiff_t;
        using iterator_concept = std::forward_iterator_tag;
        using iterator_category = std::forward_iterator_tag;
        const document_detail::DocumentData* data{};
        const std::size_t* children{};
        std::size_t index{};
        [[nodiscard]] MemberView operator*() const noexcept;
        iterator& operator++() noexcept { ++index; return *this; }
        iterator operator++(int) noexcept { auto previous = *this; ++index; return previous; }
        friend bool operator==(const iterator&, const iterator&) = default;
    };
    ObjectView() noexcept = default;
    [[nodiscard]] iterator begin() const noexcept { return {data_, children_.data(), 0}; }
    [[nodiscard]] iterator end() const noexcept { return {data_, children_.data(), size()}; }
    [[nodiscard]] std::size_t size() const noexcept { return children_.size(); }
    [[nodiscard]] bool empty() const noexcept { return children_.empty(); }
private:
    ObjectView(const document_detail::DocumentData* data, std::span<const std::size_t> children) noexcept
        : data_(data), children_(children) {}
    const document_detail::DocumentData* data_{};
    std::span<const std::size_t> children_;
    friend class NodeView;
};

class Document final {
public:
    Document(const Document&) noexcept = default;
    Document& operator=(const Document&) noexcept = default;
    Document(Document&&) noexcept = default;
    Document& operator=(Document&&) noexcept = default;
    ~Document() = default;
    [[nodiscard]] NodeView root() const noexcept;
    [[nodiscard]] std::string_view kind() const noexcept;
    [[nodiscard]] DocumentVersion version() const noexcept;
    [[nodiscard]] const std::filesystem::path& source_path() const noexcept;
    template<class Function> [[nodiscard]] auto read(Function&& function) const
    { return root().read(std::forward<Function>(function)); }
private:
    explicit Document(std::shared_ptr<const document_detail::DocumentData> data) noexcept
        : data_(std::move(data)) {}
    std::shared_ptr<const document_detail::DocumentData> data_;
    friend struct document_detail::DocumentAccess;
};

class Reader final {
public:
    template<class T> [[nodiscard]] T as() const
    { return document_detail::unwrap(node_.as<T>()); }
    template<class T> [[nodiscard]] T get(std::string_view name) const
    { return document_detail::unwrap(node_.get<T>(name)); }
    template<class T> [[nodiscard]] T get_or(std::string_view name, T fallback) const
    { return document_detail::unwrap(node_.get_or<T>(name, std::move(fallback))); }
    template<class T> [[nodiscard]] std::optional<T> optional(std::string_view name) const
    { return document_detail::unwrap(node_.optional<T>(name)); }
    [[nodiscard]] Reader child(std::string_view name) const
    { return Reader{document_detail::unwrap(node_.child(name))}; }
    [[nodiscard]] std::string_view borrow_string() const
    { return document_detail::unwrap(node_.borrow_string()); }
    [[nodiscard]] auto elements() const;
    [[nodiscard]] auto members() const;
    [[nodiscard]] NodeView view() const noexcept { return node_; }
    [[noreturn]] void fail(std::string message, ErrorCode code = ErrorCode::invalid_document) const
    { throw document_detail::ReadFailure{node_.error(code, std::move(message))}; }
private:
    explicit Reader(NodeView node) noexcept : node_(node) {}
    NodeView node_;
    friend class NodeView;
};

struct ReaderMember final {
    std::string_view name;
    Reader value;
};

inline auto Reader::elements() const
{
    return document_detail::unwrap(node_.elements())
        | std::views::transform([](NodeView node) { return Reader{node}; });
}

inline auto Reader::members() const
{
    return document_detail::unwrap(node_.members())
        | std::views::transform([](MemberView member) {
            return ReaderMember{member.name, Reader{member.value}};
        });
}

template<class T>
Result<T> NodeView::as() const
{
    static_assert(std::same_as<T, std::remove_cvref_t<T>>, "Decode an unqualified value type");
    if (!valid()) return std::unexpected(error(ErrorCode::invalid_document, "Invalid document view"));
    // ADL finds a single optional overload in the application's type namespace.
    if constexpr (requires { { decode(*this, Type<T>{}) } -> std::same_as<Result<T>>; }) {
        auto result = decode(*this, Type<T>{});
        if (!result) {
            auto& diagnostic = result.error();
            auto context = error(diagnostic.code, {});
            const bool same_source = diagnostic.path.empty() || diagnostic.path == context.path;
            if (diagnostic.path.empty()) diagnostic.path = std::move(context.path);
            // Never combine another source file's error with this file's
            // line/column. A decoder may deliberately report external evidence.
            if (same_source) {
                if (!diagnostic.location) diagnostic.location = context.location;
                if (diagnostic.property_path.empty()) diagnostic.property_path = std::move(context.property_path);
            }
        }
        return result;
    } else if constexpr (std::same_as<T, bool>) {
        return boolean();
    } else if constexpr (std::same_as<T, std::nullptr_t>) {
        if (kind() != ValueKind::null)
            return std::unexpected(error(ErrorCode::type_mismatch, "Expected null"));
        return nullptr;
    } else if constexpr (std::signed_integral<T>) {
        auto value = signed_integer();
        if (!value) return std::unexpected(std::move(value.error()));
        if (*value < std::numeric_limits<T>::min() || *value > std::numeric_limits<T>::max())
            return std::unexpected(error(ErrorCode::number_out_of_range, "Integer is outside the requested signed type's range"));
        return static_cast<T>(*value);
    } else if constexpr (std::unsigned_integral<T>) {
        auto value = unsigned_integer();
        if (!value) return std::unexpected(std::move(value.error()));
        if (*value > std::numeric_limits<T>::max())
            return std::unexpected(error(ErrorCode::number_out_of_range, "Integer is outside the requested unsigned type's range"));
        return static_cast<T>(*value);
    } else if constexpr (std::floating_point<T>) {
        auto value = number();
        if (!value) return std::unexpected(std::move(value.error()));
        const auto maximum = static_cast<long double>(std::numeric_limits<T>::max());
        if (*value < -maximum || *value > maximum)
            return std::unexpected(error(ErrorCode::number_out_of_range, "Number is outside the requested floating-point type's range"));
        const auto result = static_cast<T>(*value);
        if (result == T{} && *value != 0)
            return std::unexpected(error(ErrorCode::number_out_of_range, "Number underflows the requested floating-point type"));
        return result;
    } else if constexpr (std::same_as<T, std::string>) {
        auto value = borrow_string();
        if (!value) return std::unexpected(std::move(value.error()));
        return std::string{*value};
    } else if constexpr (document_detail::Sequence<T>::supported) {
        using Sequence = document_detail::Sequence<T>;
        using Element = typename Sequence::Element;
        auto array = elements();
        if (!array) return std::unexpected(std::move(array.error()));
        if constexpr (!Sequence::dynamic) {
            if (array->size() != Sequence::size)
                return std::unexpected(error(ErrorCode::count_mismatch,
                    "Expected " + std::to_string(Sequence::size) + " array elements, found " + std::to_string(array->size())));
        }
        if constexpr (Sequence::dynamic) {
            T result;
            result.reserve(array->size());
            for (const auto element : *array) {
                auto decoded = element.template as<Element>();
                if (!decoded) return std::unexpected(std::move(decoded.error()));
                result.push_back(std::move(*decoded));
            }
            return result;
        } else {
            // No default-construction requirement for custom array elements.
            std::array<std::optional<Element>, Sequence::size> values;
            std::size_t index{};
            for (const auto element : *array) {
                auto decoded = element.template as<Element>();
                if (!decoded) return std::unexpected(std::move(decoded.error()));
                values[index++].emplace(std::move(*decoded));
            }
            return [&]<std::size_t... I>(std::index_sequence<I...>) {
                return T{std::move(*values[I])...};
            }(std::make_index_sequence<Sequence::size>{});
        }
    } else {
        static_assert(!std::same_as<T, T>, "Unsupported document type: define decode(NodeView, content::Type<T>) returning content::Result<T>");
    }
}

template<class T>
Result<T> NodeView::get(std::string_view name) const
{
    auto node = child(name);
    if (!node) return std::unexpected(std::move(node.error()));
    return node->template as<T>();
}

template<class T>
Result<std::optional<T>> NodeView::optional(std::string_view name) const
{
    auto node = find(name);
    if (!node) return std::unexpected(std::move(node.error()));
    if (!*node) return std::optional<T>{};
    auto value = (**node).template as<T>();
    if (!value) return std::unexpected(std::move(value.error()));
    return std::optional<T>{std::move(*value)};
}

template<class T>
Result<T> NodeView::get_or(std::string_view name, T fallback) const
{
    auto value = optional<T>(name);
    if (!value) return std::unexpected(std::move(value.error()));
    if (!*value) return std::move(fallback);
    return std::move(**value);
}

template<class Function>
auto NodeView::read(Function&& function) const
{
    using T = std::invoke_result_t<Function, Reader&>;
    static_assert(!std::is_reference_v<T>, "read() callbacks must return a value, not a reference");
    static_assert(!document_detail::IsExpected<T>::value, "read() callbacks return plain values, not expected");
    if (!valid()) return Result<T>{std::unexpected(error(ErrorCode::invalid_document, "Invalid document view"))};
    try {
        Reader reader{*this};
        if constexpr (std::is_void_v<T>) {
            std::invoke(std::forward<Function>(function), reader);
            return Result<void>{};
        } else {
            return Result<T>{std::invoke(std::forward<Function>(function), reader)};
        }
    } catch (document_detail::ReadFailure& failure) {
        return Result<T>{std::unexpected(std::move(failure.diagnostic))};
    }
}

[[nodiscard]] Result<Document> parse_document(std::string_view source,
    const DocumentReadOptions& options = {});
[[nodiscard]] Result<Document> read_document(const std::filesystem::path& path,
    const DocumentReadOptions& options = {});
[[nodiscard]] Result<std::string> write_document(const Document& document,
    const DocumentWriteOptions& options = {});
[[nodiscard]] Result<void> write_document(const std::filesystem::path& path,
    const Document& document, const DocumentWriteOptions& options = {});

} // namespace vng::content
