#include <vng/content/document.hpp>
#include <vng/content/detail/document_data.hpp>

#include <array>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace vng::content {
namespace {

struct ParseFailure final {
    Diagnostic error;
};

[[nodiscard]] Diagnostic diagnostic(
    ErrorCode code,
    std::string message,
    std::optional<SourceLocation> location = std::nullopt)
{
    return Diagnostic{
        .code = code,
        .message = std::move(message),
        .location = location,
        .path = {},
        .notes = {},
    };
}

[[nodiscard]] bool ascii_alpha(char value) noexcept
{
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

[[nodiscard]] bool ascii_digit(char value) noexcept
{
    return value >= '0' && value <= '9';
}

[[nodiscard]] bool key_start(char value) noexcept
{
    return ascii_alpha(value) || value == '_';
}

[[nodiscard]] bool key_continue(char value) noexcept
{
    return key_start(value) || ascii_digit(value) || value == '/'
           || value == '.' || value == '-';
}

[[nodiscard]] bool bare_key(std::string_view value) noexcept
{
    if (value.empty() || !key_start(value.front())) {
        return false;
    }
    for (char character : value) {
        if (!key_continue(character)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_utf8(std::string_view value) noexcept
{
    std::size_t position = 0;
    while (position < value.size()) {
        const auto first = static_cast<unsigned char>(value[position++]);
        if (first <= 0x7fU) {
            continue;
        }
        std::uint32_t code{};
        std::size_t continuation{};
        if (first >= 0xc2U && first <= 0xdfU) {
            code = first & 0x1fU;
            continuation = 1;
        } else if (first >= 0xe0U && first <= 0xefU) {
            code = first & 0x0fU;
            continuation = 2;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            code = first & 0x07U;
            continuation = 3;
        } else {
            return false;
        }
        if (continuation > value.size() - position) {
            return false;
        }
        for (std::size_t index = 0; index < continuation; ++index) {
            const auto next = static_cast<unsigned char>(value[position++]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            code = (code << 6U) | (next & 0x3fU);
        }
        if ((continuation == 1 && code < 0x80U)
            || (continuation == 2 && code < 0x800U)
            || (continuation == 3 && code < 0x10000U)
            || (code >= 0xd800U && code <= 0xdfffU)
            || code > 0x10ffffU) {
            return false;
        }
    }
    return true;
}

void append_utf8(std::string& target, std::uint32_t code)
{
    if (code <= 0x7fU) {
        target.push_back(static_cast<char>(code));
    } else if (code <= 0x7ffU) {
        target.push_back(static_cast<char>(0xc0U | (code >> 6U)));
        target.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    } else if (code <= 0xffffU) {
        target.push_back(static_cast<char>(0xe0U | (code >> 12U)));
        target.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
        target.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    } else {
        target.push_back(static_cast<char>(0xf0U | (code >> 18U)));
        target.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3fU)));
        target.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
        target.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    }
}

[[nodiscard]] int hex_digit(char value) noexcept
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

void append_path_key(std::string& result, std::string_view key)
{
    const bool simple = !key.empty() && key_start(key.front())
                        && std::ranges::all_of(key, [](char character) {
                               return key_start(character) || ascii_digit(character);
                           });
    if (simple) {
        if (!result.empty()) {
            result.push_back('.');
        }
        result.append(key);
    } else {
        result.append("[\"");
        for (char character : key) {
            if (character == '\\' || character == '"') {
                result.push_back('\\');
            }
            if (static_cast<unsigned char>(character) < 0x20U) {
                constexpr char hex[] = "0123456789abcdef";
                const auto code = static_cast<unsigned char>(character);
                result.append("\\u00");
                result.push_back(hex[code >> 4U]);
                result.push_back(hex[code & 0x0fU]);
            } else {
                result.push_back(character);
            }
        }
        result.append("\"]");
    }
}

class Parser final {
public:
    Parser(std::string_view source, const DocumentReadOptions& options)
        : source_(source), limits_(options.limits),
          data_(std::make_shared<document_detail::DocumentData>())
    {
        data_->source_path = options.source_path;
    }

    [[nodiscard]] Document parse()
    {
        skip_trivia();
        const auto header_location = location();
        data_->kind = identifier();
        consume_bytes(data_->kind.size());
        if (at_end() || (!whitespace(current()) && current() != '#')) {
            fail(ErrorCode::unexpected_token, "Expected whitespace after the document kind");
        }
        skip_trivia();
        const auto version_location = location();
        const auto start = position_;
        while (!at_end() && !whitespace(current()) && current() != '#') {
            advance();
            if (position_ - start > limits_.max_number_bytes) {
                fail(ErrorCode::limit_exceeded, "Document version exceeds the token byte limit",
                     version_location);
            }
        }
        const auto version = source_.substr(start, position_ - start);
        if (version != "1.0") {
            fail(ErrorCode::unsupported_version,
                 "Expected document format version 1.0", version_location);
        }
        data_->version = {1, 0};
        const auto root = add_node(ValueKind::object, header_location,
                                   document_detail::invalid_node, {}, {});
        parse_members(root, 0, false);
        return document_detail::DocumentAccess::make(std::move(data_));
    }

private:
    [[nodiscard]] static bool whitespace(char value) noexcept
    {
        return value == ' ' || value == '\t' || value == '\n' || value == '\r';
    }

    [[nodiscard]] bool at_end() const noexcept { return position_ == source_.size(); }
    [[nodiscard]] char current() const noexcept { return at_end() ? '\0' : source_[position_]; }
    [[nodiscard]] SourceLocation location() const noexcept
    {
        return {.byte_offset = position_, .line = line_, .column = column_};
    }

    void advance() noexcept
    {
        if (source_[position_++] == '\n') {
            ++line_;
            column_ = 1;
        } else {
            ++column_;
        }
    }

    [[noreturn]] void fail(
        ErrorCode code,
        std::string message,
        std::optional<SourceLocation> where = std::nullopt) const
    {
        auto error = diagnostic(code, std::move(message), where.value_or(location()));
        error.path = data_->source_path;
        error.property_path = property_path_;
        throw ParseFailure{std::move(error)};
    }

    void skip_trivia()
    {
        while (!at_end()) {
            if (whitespace(current())) {
                advance();
            } else if (current() == '#') {
                while (!at_end() && current() != '\n') {
                    advance();
                }
            } else {
                break;
            }
        }
    }

    void expect(char character)
    {
        skip_trivia();
        if (at_end() || current() != character) {
            fail(ErrorCode::unexpected_token,
                 "Expected '" + std::string(1, character) + "'");
        }
        advance();
    }

    void consume_bytes(std::size_t bytes)
    {
        if (bytes > limits_.max_decoded_bytes - decoded_bytes_) {
            fail(ErrorCode::limit_exceeded, "Document exceeds the decoded-byte limit");
        }
        decoded_bytes_ += bytes;
    }

    [[nodiscard]] std::string identifier()
    {
        if (!key_start(current())) {
            fail(ErrorCode::unexpected_token, "Expected a name or a quoted property key");
        }
        const auto start = position_;
        while (!at_end() && key_continue(current())) {
            advance();
            if (position_ - start > limits_.max_string_bytes) {
                fail(ErrorCode::limit_exceeded, "Name exceeds the string byte limit");
            }
            if (position_ - start > limits_.max_decoded_bytes - decoded_bytes_) {
                fail(ErrorCode::limit_exceeded, "Name exceeds the remaining decoded-byte budget");
            }
        }
        return std::string(source_.substr(start, position_ - start));
    }

    [[nodiscard]] std::uint32_t unicode_quad()
    {
        std::uint32_t code{};
        for (unsigned index = 0; index < 4; ++index) {
            const int value = at_end() ? -1 : hex_digit(current());
            if (value < 0) {
                fail(ErrorCode::invalid_escape, "Expected four hexadecimal digits after \\u");
            }
            code = (code << 4U) | static_cast<std::uint32_t>(value);
            advance();
        }
        return code;
    }

    [[nodiscard]] std::string string()
    {
        const auto start = location();
        expect('"');
        std::string value;
        const auto ensure_room = [&](std::size_t bytes) {
            if (bytes > limits_.max_string_bytes - value.size()) {
                fail(ErrorCode::limit_exceeded,
                     "String exceeds the decoded-string byte limit", start);
            }
            if (bytes > limits_.max_decoded_bytes - decoded_bytes_ - value.size()) {
                fail(ErrorCode::limit_exceeded,
                     "String exceeds the remaining decoded-byte budget", start);
            }
        };
        const auto push = [&](char character) {
            ensure_room(1);
            value.push_back(character);
        };
        while (!at_end() && current() != '"') {
            const auto character = static_cast<unsigned char>(current());
            if (character < 0x20U) {
                fail(ErrorCode::invalid_token, "Unescaped control character in a string");
            }
            advance();
            if (character != '\\') {
                push(static_cast<char>(character));
            } else {
                if (at_end()) {
                    fail(ErrorCode::invalid_escape, "Unterminated string escape");
                }
                const char escaped = current();
                advance();
                switch (escaped) {
                case '"': push('"'); break;
                case '\\': push('\\'); break;
                case '/': push('/'); break;
                case 'b': push('\b'); break;
                case 'f': push('\f'); break;
                case 'n': push('\n'); break;
                case 'r': push('\r'); break;
                case 't': push('\t'); break;
                case 'u': {
                    auto code = unicode_quad();
                    if (code >= 0xd800U && code <= 0xdbffU) {
                        if (at_end() || current() != '\\') {
                            fail(ErrorCode::invalid_escape,
                                 "A high Unicode surrogate requires a low surrogate");
                        }
                        advance();
                        if (at_end() || current() != 'u') {
                            fail(ErrorCode::invalid_escape,
                                 "A high Unicode surrogate requires a \\u low surrogate");
                        }
                        advance();
                        const auto low = unicode_quad();
                        if (low < 0xdc00U || low > 0xdfffU) {
                            fail(ErrorCode::invalid_escape, "Invalid low Unicode surrogate");
                        }
                        code = 0x10000U + ((code - 0xd800U) << 10U) + low - 0xdc00U;
                    } else if (code >= 0xdc00U && code <= 0xdfffU) {
                        fail(ErrorCode::invalid_escape, "Unpaired low Unicode surrogate");
                    }
                    ensure_room(code <= 0x7fU ? 1U : code <= 0x7ffU ? 2U : code <= 0xffffU ? 3U : 4U);
                    append_utf8(value, code);
                    break;
                }
                default:
                    fail(ErrorCode::invalid_escape, "Unknown string escape");
                }
            }
        }
        if (at_end()) {
            fail(ErrorCode::unexpected_token, "Unterminated string", start);
        }
        advance();
        if (!valid_utf8(value)) {
            fail(ErrorCode::invalid_utf8, "String contains invalid UTF-8", start);
        }
        return value;
    }

    [[nodiscard]] document_detail::NodeId add_node(
        ValueKind kind, SourceLocation where, document_detail::NodeId parent,
        std::string key, SourceLocation key_location)
    {
        if (data_->nodes.size() >= limits_.max_nodes) {
            fail(ErrorCode::limit_exceeded, "Document exceeds the node-count limit", where);
        }
        consume_bytes(sizeof(document_detail::NodeData));
        consume_bytes(key.size());
        const auto id = data_->nodes.size();
        data_->nodes.push_back(document_detail::NodeData{
            .kind = kind,
            .location = where,
            .key_location = key_location,
            .parent = parent,
            .key = std::move(key),
            .scalar = {},
            .children = {},
        });
        if (parent != document_detail::invalid_node) {
            consume_bytes(sizeof(document_detail::NodeId));
            data_->nodes[parent].children.push_back(id);
        }
        return id;
    }

    void parse_members(document_detail::NodeId object, std::size_t depth, bool braced)
    {
        std::unordered_set<std::string> names;
        skip_trivia();
        while (!at_end() && (!braced || current() != '}')) {
            const auto key_location = location();
            auto key = current() == '"' ? string() : identifier();
            const auto parent_path_size = property_path_.size();
            append_path_key(property_path_, key);
            if (!names.insert(key).second) {
                fail(ErrorCode::duplicate_property,
                     "Duplicate property '" + key + "'", key_location);
            }
            expect('=');
            parse_value(object, std::move(key), key_location, depth + 1);
            expect(';');
            property_path_.resize(parent_path_size);
            skip_trivia();
        }
        if (braced) {
            expect('}');
        }
    }

    void parse_value(
        document_detail::NodeId parent, std::string key,
        SourceLocation key_location, std::size_t depth)
    {
        skip_trivia();
        if (depth > std::min<std::size_t>(limits_.max_depth, 256)) {
            fail(ErrorCode::limit_exceeded, "Document exceeds the nesting-depth limit (maximum 256)");
        }
        const auto where = location();
        if (at_end()) {
            fail(ErrorCode::unexpected_token, "Expected a value");
        }
        if (current() == '{') {
            advance();
            const auto object = add_node(ValueKind::object, where, parent, std::move(key), key_location);
            parse_members(object, depth, true);
        } else if (current() == '[') {
            advance();
            const auto array = add_node(ValueKind::array, where, parent, std::move(key), key_location);
            skip_trivia();
            std::size_t index{};
            while (!at_end() && current() != ']') {
                const auto parent_path_size = property_path_.size();
                property_path_ += '[' + std::to_string(index++) + ']';
                parse_value(array, {}, location(), depth + 1);
                property_path_.resize(parent_path_size);
                skip_trivia();
                if (current() == ']') {
                    break;
                }
                expect(',');
                skip_trivia();
            }
            expect(']');
        } else if (current() == '"') {
            auto value = string();
            consume_bytes(value.size());
            const auto id = add_node(ValueKind::string, where, parent, std::move(key), key_location);
            data_->nodes[id].scalar = std::move(value);
        } else if (current() == '-' || ascii_digit(current())) {
            parse_number(parent, std::move(key), key_location);
        } else if (key_start(current())) {
            const auto literal = identifier();
            if (literal != "true" && literal != "false" && literal != "null") {
                fail(ErrorCode::unexpected_token,
                     "Expected a value; strings must be quoted", where);
            }
            const auto id = add_node(literal == "null" ? ValueKind::null : ValueKind::boolean,
                                      where, parent, std::move(key), key_location);
            if (literal != "null") {
                data_->nodes[id].scalar = literal == "true";
            }
        } else {
            fail(ErrorCode::unexpected_token, "Expected an object, array, string, number, boolean, or null");
        }
    }

    void parse_number(
        document_detail::NodeId parent, std::string key, SourceLocation key_location)
    {
        const auto where = location();
        const auto start = position_;
        while (!at_end() && !whitespace(current()) && current() != '#'
               && current() != ',' && current() != ';' && current() != ']'
               && current() != '}') {
            advance();
            if (position_ - start > limits_.max_number_bytes) {
                fail(ErrorCode::limit_exceeded, "Number exceeds the token byte limit", where);
            }
        }
        const auto token = source_.substr(start, position_ - start);
        std::size_t offset = token.front() == '-' ? 1 : 0;
        if (offset == token.size() || !ascii_digit(token[offset])) {
            fail(ErrorCode::invalid_number, "Expected digits in a number", where);
        }
        if (token[offset] == '0') {
            ++offset;
        } else {
            while (offset < token.size() && ascii_digit(token[offset])) {
                ++offset;
            }
        }
        bool floating{};
        if (offset < token.size() && token[offset] == '.') {
            floating = true;
            const auto first = ++offset;
            while (offset < token.size() && ascii_digit(token[offset])) {
                ++offset;
            }
            if (first == offset) {
                fail(ErrorCode::invalid_number, "Expected digits after the decimal point", where);
            }
        }
        if (offset < token.size() && (token[offset] == 'e' || token[offset] == 'E')) {
            floating = true;
            ++offset;
            if (offset < token.size() && (token[offset] == '+' || token[offset] == '-')) {
                ++offset;
            }
            const auto first = offset;
            while (offset < token.size() && ascii_digit(token[offset])) {
                ++offset;
            }
            if (first == offset) {
                fail(ErrorCode::invalid_number, "Expected exponent digits", where);
            }
        }
        if (offset != token.size()) {
            fail(ErrorCode::invalid_number, "Invalid number syntax", where);
        }
        const auto last = token.data() + token.size();
        if (floating) {
            f64 value{};
            const auto converted = std::from_chars(token.data(), last, value, std::chars_format::general);
            if (converted.ec != std::errc{} || converted.ptr != last || !std::isfinite(value)) {
                fail(ErrorCode::number_out_of_range, "Number is outside the finite f64 range", where);
            }
            const auto id = add_node(ValueKind::number, where, parent, std::move(key), key_location);
            data_->nodes[id].scalar = value;
        } else if (token.front() == '-') {
            i64 value{};
            const auto converted = std::from_chars(token.data(), last, value);
            if (converted.ec != std::errc{} || converted.ptr != last) {
                fail(ErrorCode::number_out_of_range, "Integer is outside the i64 range", where);
            }
            const auto id = add_node(ValueKind::integer, where, parent, std::move(key), key_location);
            data_->nodes[id].scalar = value;
        } else {
            u64 value{};
            const auto converted = std::from_chars(token.data(), last, value);
            if (converted.ec != std::errc{} || converted.ptr != last) {
                fail(ErrorCode::number_out_of_range, "Integer is outside the u64 range", where);
            }
            const bool signed_range = value <= static_cast<u64>(std::numeric_limits<i64>::max());
            const auto id = add_node(signed_range ? ValueKind::integer : ValueKind::unsigned_integer,
                                      where, parent, std::move(key), key_location);
            if (signed_range) {
                data_->nodes[id].scalar = static_cast<i64>(value);
            } else {
                data_->nodes[id].scalar = value;
            }
        }
    }

    std::string_view source_;
    const DocumentLimits& limits_;
    std::shared_ptr<document_detail::DocumentData> data_;
    std::size_t position_{};
    std::size_t line_{1};
    std::size_t column_{1};
    std::size_t decoded_bytes_{};
    std::string property_path_;
};

class Writer final {
public:
    Writer(const document_detail::DocumentData& data, const DocumentWriteOptions& options)
        : data_(data), limits_(options.limits)
    {}

    [[nodiscard]] std::string write()
    {
        if (!bare_key(data_.kind)) {
            fail(ErrorCode::invalid_document, "Document kind must be a nonempty bare identifier");
        }
        if (data_.version != DocumentVersion{1, 0}) {
            fail(ErrorCode::unsupported_version, "Only document format version 1.0 is supported");
        }
        if (limits_.max_number_bytes < 3) {
            fail(ErrorCode::limit_exceeded, "Document version exceeds the token byte limit");
        }
        if (data_.nodes.empty() || data_.nodes[0].kind != ValueKind::object) {
            fail(ErrorCode::invalid_document, "Document root must be an object");
        }
        if (data_.nodes.size() > limits_.max_nodes) {
            fail(ErrorCode::limit_exceeded, "Document exceeds the node-count limit");
        }
        visited_.resize(data_.nodes.size(), false);
        check_string(data_.kind);
        append(data_.kind);
        append(" 1.0\n");
        write_node(0, document_detail::invalid_node, 0, true);
        if (visited_count_ != data_.nodes.size()) {
            fail(ErrorCode::invalid_document, "Document contains unreachable nodes");
        }
        return std::move(output_);
    }

private:
    [[noreturn]] void fail(ErrorCode code, std::string message) const
    {
        auto error = diagnostic(code, std::move(message));
        error.path = data_.source_path;
        error.property_path = property_path_;
        throw ParseFailure{std::move(error)};
    }

    void consume_bytes(std::size_t bytes)
    {
        if (bytes > limits_.max_decoded_bytes - decoded_bytes_) {
            fail(ErrorCode::limit_exceeded, "Document exceeds the decoded-byte limit");
        }
        decoded_bytes_ += bytes;
    }

    void check_string(std::string_view value)
    {
        if (value.size() > limits_.max_string_bytes) {
            fail(ErrorCode::limit_exceeded, "String exceeds the decoded-string byte limit");
        }
        if (!valid_utf8(value)) {
            fail(ErrorCode::invalid_utf8, "Document contains invalid UTF-8");
        }
        consume_bytes(value.size());
    }

    void append(std::string_view value)
    {
        if (value.size() > limits_.max_source_bytes - output_.size()) {
            fail(ErrorCode::input_too_large, "Serialized document exceeds the source-byte limit");
        }
        output_.append(value);
    }

    void push(char value) { append(std::string_view(&value, 1)); }

    void indent(std::size_t depth)
    {
        for (std::size_t index = 0; index < depth; ++index) {
            append("    ");
        }
    }

    void escaped(std::string_view value)
    {
        constexpr char hex[] = "0123456789abcdef";
        push('"');
        for (char raw : value) {
            const auto character = static_cast<unsigned char>(raw);
            switch (character) {
            case '"': append("\\\""); break;
            case '\\': append("\\\\"); break;
            case '\b': append("\\b"); break;
            case '\f': append("\\f"); break;
            case '\n': append("\\n"); break;
            case '\r': append("\\r"); break;
            case '\t': append("\\t"); break;
            default:
                if (character < 0x20U) {
                    const std::array<char, 6> escape{
                        '\\', 'u', '0', '0', hex[character >> 4U], hex[character & 0x0fU],
                    };
                    append(std::string_view(escape.data(), escape.size()));
                } else {
                    push(raw);
                }
                break;
            }
        }
        push('"');
    }

    template<class T>
    [[nodiscard]] const T& scalar(const document_detail::NodeData& node) const
    {
        const auto* value = std::get_if<T>(&node.scalar);
        if (!value) {
            fail(ErrorCode::invalid_document, "Node kind disagrees with its stored scalar value");
        }
        return *value;
    }

    template<class T>
    void number(T value)
    {
        std::array<char, 128> text{};
        std::to_chars_result converted;
        if constexpr (std::is_floating_point_v<T>) {
            if (!std::isfinite(value)) {
                fail(ErrorCode::number_out_of_range, "Cannot serialize a non-finite number");
            }
            converted = std::to_chars(text.data(), text.data() + text.size(), value,
                                      std::chars_format::general);
        } else {
            converted = std::to_chars(text.data(), text.data() + text.size(), value);
        }
        if (converted.ec != std::errc{}) {
            fail(ErrorCode::invalid_number, "Could not serialize a number");
        }
        const std::string_view token(text.data(), converted.ptr);
        bool suffix{};
        if constexpr (std::is_floating_point_v<T>) {
            suffix = token.find_first_of(".eE") == std::string_view::npos;
        }
        if (token.size() + (suffix ? 2U : 0U) > limits_.max_number_bytes) {
            fail(ErrorCode::limit_exceeded, "Number exceeds the token byte limit");
        }
        append(token);
        if (suffix) {
            append(".0");
        }
    }

    void write_node(
        document_detail::NodeId id, document_detail::NodeId parent,
        std::size_t depth, bool root = false)
    {
        if (depth > std::min<std::size_t>(limits_.max_depth, 256)) {
            fail(ErrorCode::limit_exceeded, "Document exceeds the nesting-depth limit (maximum 256)");
        }
        if (id >= data_.nodes.size() || visited_[id]) {
            fail(ErrorCode::invalid_document, "Document contains an invalid or repeated child node");
        }
        visited_[id] = true;
        ++visited_count_;
        const auto& node = data_.nodes[id];
        if (node.parent != parent) {
            fail(ErrorCode::invalid_document, "Document child has an inconsistent parent");
        }
        consume_bytes(sizeof(document_detail::NodeData));
        check_string(node.key);
        if (parent != document_detail::invalid_node) {
            consume_bytes(sizeof(document_detail::NodeId));
        }
        if (node.kind != ValueKind::array && node.kind != ValueKind::object
            && !node.children.empty()) {
            fail(ErrorCode::invalid_document, "A scalar node cannot contain children");
        }
        switch (node.kind) {
        case ValueKind::null:
            (void)scalar<std::monostate>(node);
            append("null");
            break;
        case ValueKind::boolean: append(scalar<bool>(node) ? "true" : "false"); break;
        case ValueKind::integer: number(scalar<i64>(node)); break;
        case ValueKind::unsigned_integer: number(scalar<u64>(node)); break;
        case ValueKind::number: number(scalar<f64>(node)); break;
        case ValueKind::string:
            check_string(scalar<std::string>(node));
            escaped(scalar<std::string>(node));
            break;
        case ValueKind::array: {
            (void)scalar<std::monostate>(node);
            bool multiline{};
            for (auto child : node.children) {
                if (child >= data_.nodes.size()) {
                    fail(ErrorCode::invalid_document, "Array has an invalid child index");
                }
                const auto kind = data_.nodes[child].kind;
                multiline |= kind == ValueKind::object || kind == ValueKind::array;
            }
            push('[');
            if (multiline) {
                push('\n');
            }
            std::size_t index{};
            for (auto child : node.children) {
                if (!data_.nodes[child].key.empty()) {
                    fail(ErrorCode::invalid_document, "Array elements cannot have property keys");
                }
                if (index != 0) {
                    append(multiline ? ",\n" : ", ");
                }
                if (multiline) {
                    indent(depth);
                }
                const auto parent_path_size = property_path_.size();
                property_path_ += '[' + std::to_string(index++) + ']';
                write_node(child, id, depth + 1);
                property_path_.resize(parent_path_size);
            }
            if (multiline) {
                push('\n');
                indent(depth - 1);
            }
            push(']');
            break;
        }
        case ValueKind::object: {
            (void)scalar<std::monostate>(node);
            if (!root) {
                append(node.children.empty() ? "{" : "{\n");
            }
            std::unordered_set<std::string_view> names;
            for (auto child : node.children) {
                if (child >= data_.nodes.size()) {
                    fail(ErrorCode::invalid_document, "Object has an invalid child index");
                }
                const auto& key = data_.nodes[child].key;
                const auto parent_path_size = property_path_.size();
                append_path_key(property_path_, key);
                if (!names.insert(key).second) {
                    fail(ErrorCode::duplicate_property, "Object has a duplicate property");
                }
                indent(root ? 0 : depth);
                if (bare_key(key)) {
                    append(key);
                } else {
                    escaped(key);
                }
                append(" = ");
                write_node(child, id, depth + 1);
                append(";\n");
                property_path_.resize(parent_path_size);
            }
            if (!root) {
                if (!node.children.empty()) {
                    indent(depth - 1);
                }
                push('}');
            }
            break;
        }
        case ValueKind::invalid:
            fail(ErrorCode::invalid_document, "Cannot serialize an invalid node");
        }
    }

    const document_detail::DocumentData& data_;
    const DocumentLimits& limits_;
    std::vector<bool> visited_;
    std::size_t visited_count_{};
    std::size_t decoded_bytes_{};
    std::string property_path_;
    std::string output_;
};

} // namespace

Result<Document> parse_document(std::string_view source, const DocumentReadOptions& options)
{
    if (source.size() > options.limits.max_source_bytes) {
        auto error = diagnostic(ErrorCode::input_too_large,
                                "Document source exceeds the source-byte limit", SourceLocation{});
        error.path = options.source_path;
        return std::unexpected(std::move(error));
    }
    try {
        return Parser(source, options).parse();
    } catch (ParseFailure& failure) {
        return std::unexpected(std::move(failure.error));
    }
}

Result<Document> read_document(const std::filesystem::path& path, const DocumentReadOptions& options)
{
    const auto io_error = [&](std::string message) -> Result<Document> {
        auto error = diagnostic(ErrorCode::io_error, std::move(message));
        error.path = path;
        return std::unexpected(std::move(error));
    };
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return io_error("Could not open document file");
    }
    const auto end = input.tellg();
    if (end < std::streampos{0}) {
        return io_error("Could not determine document file size");
    }
    const auto size = static_cast<std::uintmax_t>(end);
    if (size > options.limits.max_source_bytes
        || size > std::numeric_limits<std::size_t>::max()
        || size > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        auto error = diagnostic(ErrorCode::input_too_large,
                                "Document file exceeds the source-byte limit");
        error.path = path;
        return std::unexpected(std::move(error));
    }
    std::string source(static_cast<std::size_t>(size), '\0');
    input.seekg(0, std::ios::beg);
    if (!source.empty()) {
        input.read(source.data(), static_cast<std::streamsize>(source.size()));
    }
    if (!input) {
        return io_error("Could not read the complete document file");
    }
    const auto trailing = input.peek();
    if (input.bad()) {
        return io_error("Could not read the complete document file");
    }
    if (trailing != std::char_traits<char>::eof()) {
        return io_error("Document file changed while it was being read");
    }
    auto actual_options = options;
    actual_options.source_path = path;
    return parse_document(source, actual_options);
}

Result<std::string> write_document(const Document& document, const DocumentWriteOptions& options)
{
    const auto* data = document_detail::DocumentAccess::data(document);
    if (!data) {
        return std::unexpected(diagnostic(ErrorCode::invalid_document,
                                          "Cannot serialize an empty or moved-from document"));
    }
    try {
        return Writer(*data, options).write();
    } catch (ParseFailure& failure) {
        return std::unexpected(std::move(failure.error));
    }
}

Result<void> write_document(
    const std::filesystem::path& path, const Document& document, const DocumentWriteOptions& options)
{
    auto serialized = write_document(document, options);
    if (!serialized) {
        (void)serialized.error().note("While serializing document for output to '" + path.string() + "'");
        return std::unexpected(std::move(serialized.error()));
    }
    if (serialized->size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        auto error = diagnostic(ErrorCode::input_too_large,
                                "Document output exceeds the platform stream-size limit");
        error.path = path;
        return std::unexpected(std::move(error));
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (output) {
        output.write(serialized->data(), static_cast<std::streamsize>(serialized->size()));
        output.flush();
        output.close();
    }
    if (!output) {
        auto error = diagnostic(ErrorCode::io_error, "Could not write the complete document file");
        error.path = path;
        return std::unexpected(std::move(error));
    }
    return {};
}

} // namespace vng::content
