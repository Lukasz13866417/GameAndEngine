#include <vng/content/vmesh.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace vng::content::vmesh {
namespace {

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

[[nodiscard]] bool checked_multiply(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept
{
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] bool checked_add(
    std::size_t left,
    std::size_t right,
    std::size_t& result) noexcept
{
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] bool ascii_alpha(char character) noexcept
{
    return (character >= 'a' && character <= 'z')
           || (character >= 'A' && character <= 'Z');
}

[[nodiscard]] bool ascii_digit(char character) noexcept
{
    return character >= '0' && character <= '9';
}

[[nodiscard]] bool name_start(char character) noexcept
{
    return ascii_alpha(character) || character == '_';
}

[[nodiscard]] bool name_continue(char character) noexcept
{
    return name_start(character) || ascii_digit(character)
           || character == '-' || character == '.';
}

[[nodiscard]] bool valid_stable_name(std::string_view name) noexcept
{
    if (name.empty()) {
        return false;
    }

    bool at_segment_start = true;
    bool first_segment = true;
    for (const char character : name) {
        if (character == '/') {
            if (at_segment_start) {
                return false;
            }
            at_segment_start = true;
            first_segment = false;
            continue;
        }

        const bool valid_start = name_start(character)
                                 || (!first_segment && ascii_digit(character));
        if ((at_segment_start && !valid_start)
            || (!at_segment_start && !name_continue(character))) {
            return false;
        }
        at_segment_start = false;
    }
    return !at_segment_start;
}

[[nodiscard]] bool valid_utf8(std::string_view text) noexcept
{
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char first = bytes[index++];
        if (first <= 0x7FU) {
            continue;
        }

        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1;
            code_point = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2;
            code_point = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }

        if (continuation_count > text.size() - index) {
            return false;
        }
        for (std::size_t continuation = 0; continuation < continuation_count;
             ++continuation) {
            const unsigned char byte = bytes[index++];
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (byte & 0x3FU);
        }

        const bool overlong = (continuation_count == 1 && code_point < 0x80U)
                              || (continuation_count == 2 && code_point < 0x800U)
                              || (continuation_count == 3 && code_point < 0x10000U);
        if (overlong || (code_point >= 0xD800U && code_point <= 0xDFFFU)
            || code_point > 0x10FFFFU) {
            return false;
        }
    }
    return true;
}

void append_utf8(std::string& output, std::uint32_t code_point)
{
    if (code_point <= 0x7FU) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

enum class TokenKind {
    identifier,
    number,
    string,
    left_brace,
    right_brace,
    left_bracket,
    right_bracket,
    comma,
    colon,
    equal,
    semicolon,
    end,
};

struct Token final {
    TokenKind kind{TokenKind::end};
    std::string text;
    SourceLocation location{};
};

[[nodiscard]] constexpr std::string_view token_name(TokenKind kind) noexcept
{
    switch (kind) {
    case TokenKind::identifier: return "an identifier";
    case TokenKind::number: return "a number";
    case TokenKind::string: return "a string";
    case TokenKind::left_brace: return "'{'";
    case TokenKind::right_brace: return "'}'";
    case TokenKind::left_bracket: return "'['";
    case TokenKind::right_bracket: return "']'";
    case TokenKind::comma: return "','";
    case TokenKind::colon: return "':'";
    case TokenKind::equal: return "'='";
    case TokenKind::semicolon: return "';'";
    case TokenKind::end: return "end of file";
    }
    return "a token";
}

class Lexer final {
public:
    Lexer(
        std::string_view source,
        std::size_t max_identifier_bytes,
        std::size_t max_string_bytes) noexcept
        : source_(source),
          max_identifier_bytes_(std::max(std::size_t{32}, max_identifier_bytes)),
          max_string_bytes_(max_string_bytes)
    {}

    [[nodiscard]] Result<Token> next()
    {
        skip_trivia();
        const SourceLocation start = location();
        if (at_end()) {
            return Token{.kind = TokenKind::end, .text = {}, .location = start};
        }

        const char character = current();
        switch (character) {
        case '{': return punctuation(TokenKind::left_brace);
        case '}': return punctuation(TokenKind::right_brace);
        case '[': return punctuation(TokenKind::left_bracket);
        case ']': return punctuation(TokenKind::right_bracket);
        case ',': return punctuation(TokenKind::comma);
        case ':': return punctuation(TokenKind::colon);
        case '=': return punctuation(TokenKind::equal);
        case ';': return punctuation(TokenKind::semicolon);
        case '"': return string_token();
        default: break;
        }

        if (name_start(character)) {
            const std::size_t first = index_;
            advance();
            while (!at_end()
                   && (name_continue(current()) || current() == '/')) {
                advance();
            }
            if (index_ - first > max_identifier_bytes_) {
                return std::unexpected(diagnostic(
                    ErrorCode::limit_exceeded,
                    "Identifier exceeds the configured byte limit",
                    start));
            }
            return Token{
                .kind = TokenKind::identifier,
                .text = std::string(source_.substr(first, index_ - first)),
                .location = start,
            };
        }

        if (ascii_digit(character) || character == '-') {
            const std::size_t first = index_;
            advance();
            while (!at_end() && !number_delimiter(current())) {
                advance();
            }
            constexpr std::size_t max_number_bytes = 128;
            if (index_ - first > max_number_bytes) {
                return std::unexpected(diagnostic(
                    ErrorCode::number_out_of_range,
                    "Numeric token is too long",
                    start));
            }
            return Token{
                .kind = TokenKind::number,
                .text = std::string(source_.substr(first, index_ - first)),
                .location = start,
            };
        }

        return std::unexpected(diagnostic(
            ErrorCode::invalid_token,
            "Invalid character in .vmesh input",
            start));
    }

private:
    [[nodiscard]] bool at_end() const noexcept { return index_ >= source_.size(); }
    [[nodiscard]] char current() const noexcept { return source_[index_]; }

    [[nodiscard]] SourceLocation location() const noexcept
    {
        return SourceLocation{.byte_offset = index_, .line = line_, .column = column_};
    }

    void advance() noexcept
    {
        if (at_end()) {
            return;
        }
        if (source_[index_] == '\r') {
            ++index_;
            if (!at_end() && source_[index_] == '\n') {
                ++index_;
            }
            ++line_;
            column_ = 1;
        } else if (source_[index_] == '\n') {
            ++index_;
            ++line_;
            column_ = 1;
        } else {
            ++index_;
            ++column_;
        }
    }

    void skip_trivia() noexcept
    {
        while (!at_end()) {
            if (current() == '#') {
                while (!at_end() && current() != '\r' && current() != '\n') {
                    advance();
                }
                continue;
            }
            if (current() == ' ' || current() == '\t'
                || current() == '\r' || current() == '\n') {
                advance();
                continue;
            }
            break;
        }
    }

    [[nodiscard]] static bool number_delimiter(char character) noexcept
    {
        return character == ' ' || character == '\t' || character == '\r'
               || character == '\n' || character == '#' || character == '{'
               || character == '}' || character == '[' || character == ']'
               || character == ',' || character == ':' || character == '='
               || character == ';';
    }

    [[nodiscard]] Result<Token> punctuation(TokenKind kind)
    {
        const SourceLocation start = location();
        const char character = current();
        advance();
        return Token{
            .kind = kind,
            .text = std::string(1, character),
            .location = start,
        };
    }

    [[nodiscard]] static int hex_value(char character) noexcept
    {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return 10 + character - 'a';
        }
        if (character >= 'A' && character <= 'F') {
            return 10 + character - 'A';
        }
        return -1;
    }

    [[nodiscard]] Result<Token> string_token()
    {
        const SourceLocation start = location();
        advance(); // Opening quote.
        std::string decoded;

        while (!at_end() && current() != '"') {
            const unsigned char raw = static_cast<unsigned char>(current());
            if (raw < 0x20U) {
                return std::unexpected(diagnostic(
                    ErrorCode::invalid_escape,
                    "A string cannot contain an unescaped control character",
                    location()));
            }

            if (current() != '\\') {
                if (decoded.size() >= max_string_bytes_) {
                    return std::unexpected(string_limit_diagnostic(start));
                }
                decoded.push_back(current());
                advance();
                continue;
            }

            const SourceLocation escape_location = location();
            advance();
            if (at_end()) {
                return std::unexpected(diagnostic(
                    ErrorCode::invalid_escape,
                    "Unterminated escape sequence",
                    escape_location));
            }

            const char escaped = current();
            advance();
            if (escaped != 'u' && decoded.size() >= max_string_bytes_) {
                return std::unexpected(string_limit_diagnostic(escape_location));
            }
            switch (escaped) {
            case '"': decoded.push_back('"'); break;
            case '\\': decoded.push_back('\\'); break;
            case '/': decoded.push_back('/'); break;
            case 'b': decoded.push_back('\b'); break;
            case 'f': decoded.push_back('\f'); break;
            case 'n': decoded.push_back('\n'); break;
            case 'r': decoded.push_back('\r'); break;
            case 't': decoded.push_back('\t'); break;
            case 'u': {
                std::uint32_t code_point = 0;
                for (int digit = 0; digit < 4; ++digit) {
                    if (at_end()) {
                        return std::unexpected(diagnostic(
                            ErrorCode::invalid_escape,
                            "Incomplete Unicode escape sequence",
                            escape_location));
                    }
                    const int value = hex_value(current());
                    if (value < 0) {
                        return std::unexpected(diagnostic(
                            ErrorCode::invalid_escape,
                            "Unicode escapes require exactly four hexadecimal digits",
                            location()));
                    }
                    code_point = (code_point << 4U) | static_cast<std::uint32_t>(value);
                    advance();
                }
                if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
                    return std::unexpected(diagnostic(
                        ErrorCode::invalid_escape,
                        "UTF-16 surrogate escapes are not supported; write the UTF-8 character directly",
                        escape_location));
                }
                const std::size_t encoded_size = code_point <= 0x7FU
                    ? 1
                    : (code_point <= 0x7FFU ? 2 : 3);
                if (encoded_size
                    > max_string_bytes_
                        - std::min(max_string_bytes_, decoded.size())) {
                    return std::unexpected(string_limit_diagnostic(escape_location));
                }
                append_utf8(decoded, code_point);
                break;
            }
            default:
                return std::unexpected(diagnostic(
                    ErrorCode::invalid_escape,
                    "Unknown string escape sequence",
                    escape_location));
            }
        }

        if (at_end()) {
            return std::unexpected(diagnostic(
                ErrorCode::unexpected_token,
                "Unterminated string",
                start));
        }
        advance(); // Closing quote.

        if (!valid_utf8(decoded)) {
            return std::unexpected(diagnostic(
                ErrorCode::invalid_utf8,
                "Metadata strings must contain valid UTF-8",
                start));
        }
        return Token{
            .kind = TokenKind::string,
            .text = std::move(decoded),
            .location = start,
        };
    }

    [[nodiscard]] static Diagnostic string_limit_diagnostic(
        SourceLocation location)
    {
        return diagnostic(
            ErrorCode::limit_exceeded,
            "Metadata string exceeds the configured decoded byte limit",
            location);
    }

    std::string_view source_;
    std::size_t max_identifier_bytes_{};
    std::size_t max_string_bytes_{};
    std::size_t index_{};
    std::size_t line_{1};
    std::size_t column_{1};
};

[[nodiscard]] std::optional<FieldType> parse_field_type_name(std::string_view name) noexcept
{
    constexpr std::array scalar_names{"f32", "i32", "u32"};
    constexpr std::array scalar_types{
        ScalarType::Float32,
        ScalarType::Int32,
        ScalarType::UInt32,
    };

    for (std::size_t scalar = 0; scalar < scalar_names.size(); ++scalar) {
        if (name == scalar_names[scalar]) {
            return FieldType{.scalar = scalar_types[scalar], .components = 1};
        }
        for (u8 components = 2; components <= 4; ++components) {
            std::string expected(scalar_names[scalar]);
            expected += 'x';
            expected += static_cast<char>('0' + components);
            if (name == expected) {
                return FieldType{.scalar = scalar_types[scalar], .components = components};
            }
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string field_type_name(FieldType type)
{
    std::string result;
    switch (type.scalar) {
    case ScalarType::Float32: result = "f32"; break;
    case ScalarType::Int32: result = "i32"; break;
    case ScalarType::UInt32: result = "u32"; break;
    }
    if (type.components != 1) {
        result += 'x';
        result += static_cast<char>('0' + type.components);
    }
    return result;
}

[[nodiscard]] FieldValues empty_values(ScalarType scalar)
{
    switch (scalar) {
    case ScalarType::Float32: return std::vector<f32>{};
    case ScalarType::Int32: return std::vector<i32>{};
    case ScalarType::UInt32: return std::vector<u32>{};
    }
    return std::vector<f32>{};
}

class Parser final {
public:
    Parser(std::string_view source, const ReadOptions& options)
        : lexer_(
              source,
              std::min(
                  options.limits.max_metadata_key_bytes,
                  options.limits.max_decoded_bytes),
              std::min(
                  options.limits.max_metadata_value_bytes,
                  options.limits.max_decoded_bytes)),
          limits_(options.limits)
    {}

    [[nodiscard]] Result<Document> parse()
    {
        advance();
        Document document;
        parse_header();
        parse_info(document);
        parse_vertices(document);
        parse_faces(document);
        if (!error_ && is_identifier("edges")) {
            parse_edges(document);
        }
        if (!error_ && current_.kind != TokenKind::end) {
            fail(
                ErrorCode::unexpected_token,
                "Expected end of file after the optional edges section",
                current_.location);
        }
        if (error_) {
            return std::unexpected(std::move(*error_));
        }
        return document;
    }

private:
    void advance()
    {
        if (error_) {
            return;
        }
        auto token = lexer_.next();
        if (!token) {
            error_ = std::move(token.error());
            return;
        }
        current_ = std::move(*token);
    }

    void fail(ErrorCode code, std::string message, SourceLocation location)
    {
        if (!error_) {
            error_ = diagnostic(code, std::move(message), location);
        }
    }

    [[nodiscard]] bool is(TokenKind kind) const noexcept
    {
        return !error_ && current_.kind == kind;
    }

    [[nodiscard]] bool is_identifier(std::string_view value) const noexcept
    {
        return is(TokenKind::identifier) && current_.text == value;
    }

    [[nodiscard]] bool expect(TokenKind kind, std::string_view context = {})
    {
        if (error_) {
            return false;
        }
        if (current_.kind != kind) {
            std::string message = "Expected ";
            message += token_name(kind);
            if (!context.empty()) {
                message += ' ';
                message += context;
            }
            fail(ErrorCode::unexpected_token, std::move(message), current_.location);
            return false;
        }
        advance();
        return true;
    }

    [[nodiscard]] bool expect_identifier(std::string_view value)
    {
        if (error_) {
            return false;
        }
        if (!is_identifier(value)) {
            fail(
                ErrorCode::unexpected_token,
                "Expected '" + std::string(value) + "'",
                current_.location);
            return false;
        }
        advance();
        return true;
    }

    void parse_header()
    {
        if (!expect_identifier("vmesh") || error_) {
            return;
        }
        if (!is(TokenKind::number)) {
            fail(
                ErrorCode::unexpected_token,
                "Expected the .vmesh version after 'vmesh'",
                current_.location);
            return;
        }
        if (current_.text != "1.0") {
            fail(
                ErrorCode::unsupported_version,
                "Only .vmesh version 1.0 is supported",
                current_.location);
            return;
        }
        advance();
    }

    [[nodiscard]] std::optional<std::size_t> parse_count(
        std::size_t maximum,
        std::string_view what)
    {
        if (error_) {
            return std::nullopt;
        }
        if (!is(TokenKind::number)) {
            fail(
                ErrorCode::unexpected_token,
                "Expected a count for " + std::string(what),
                current_.location);
            return std::nullopt;
        }

        Token token = std::move(current_);
        advance();
        std::uint64_t parsed{};
        const auto conversion = std::from_chars(
            token.text.data(), token.text.data() + token.text.size(), parsed);
        if (conversion.ec == std::errc::result_out_of_range) {
            fail(
                ErrorCode::number_out_of_range,
                std::string(what) + " count is out of range",
                token.location);
            return std::nullopt;
        }
        if (conversion.ec != std::errc{}
            || conversion.ptr != token.text.data() + token.text.size()) {
            fail(
                ErrorCode::invalid_number,
                "Invalid unsigned count for " + std::string(what),
                token.location);
            return std::nullopt;
        }
        if (parsed > static_cast<std::uint64_t>(maximum)
            || parsed > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            fail(
                ErrorCode::limit_exceeded,
                std::string(what) + " count exceeds the configured limit",
                token.location);
            return std::nullopt;
        }
        return static_cast<std::size_t>(parsed);
    }

    void parse_info(Document& document)
    {
        if (!expect_identifier("info")
            || !expect(TokenKind::left_brace, "after 'info'")) {
            return;
        }

        while (!error_ && !is(TokenKind::right_brace)) {
            if (document.metadata.size() >= limits_.max_metadata_entries) {
                fail(
                    ErrorCode::limit_exceeded,
                    "The info section has too many metadata entries",
                    current_.location);
                return;
            }
            if (!is(TokenKind::identifier)) {
                fail(
                    ErrorCode::unexpected_token,
                    "Expected a metadata key or '}' in the info section",
                    current_.location);
                return;
            }

            Token key = std::move(current_);
            advance();
            if (!valid_stable_name(key.text)) {
                fail(
                    ErrorCode::invalid_token,
                    "Metadata keys must be stable slash-separated identifiers",
                    key.location);
                return;
            }
            if (key.text.size() > limits_.max_metadata_key_bytes) {
                fail(
                    ErrorCode::limit_exceeded,
                    "Metadata key exceeds the configured byte limit",
                    key.location);
                return;
            }
            if (!expect(TokenKind::equal, "after a metadata key")) {
                return;
            }
            if (!is(TokenKind::string)) {
                fail(
                    ErrorCode::unexpected_token,
                    "Expected a quoted metadata value",
                    current_.location);
                return;
            }
            Token value = std::move(current_);
            advance();
            if (value.text.size() > limits_.max_metadata_value_bytes) {
                fail(
                    ErrorCode::limit_exceeded,
                    "Metadata value exceeds the configured byte limit",
                    value.location);
                return;
            }
            if (!expect(TokenKind::semicolon, "after a metadata entry")) {
                return;
            }
            if (!consume_decoded_bytes(
                    key.text.size(),
                    sizeof(char),
                    key.location,
                    "Metadata storage")
                || !consume_decoded_bytes(
                    value.text.size(),
                    sizeof(char),
                    value.location,
                    "Metadata storage")) {
                return;
            }
            if (!document.metadata.emplace(key.text, std::move(value.text)).second) {
                fail(
                    ErrorCode::duplicate_metadata,
                    "Duplicate metadata key '" + key.text + "'",
                    key.location);
                return;
            }
        }
        static_cast<void>(expect(TokenKind::right_brace, "to close the info section"));
    }

    void parse_vertices(Document& document)
    {
        if (!expect_identifier("vertices")) {
            return;
        }
        const auto count = parse_count(
            std::min(
                limits_.max_vertices,
                static_cast<std::size_t>(std::numeric_limits<u32>::max())),
            "vertex");
        if (!count || !expect(TokenKind::left_brace, "after the vertex count")) {
            return;
        }
        document.vertex_count = *count;

        if (!expect_identifier("fields")
            || !expect(TokenKind::left_brace, "after 'fields'")) {
            return;
        }

        std::unordered_set<std::string> field_names;
        std::size_t total_values = 0;
        while (!error_ && !is(TokenKind::right_brace)) {
            if (document.vertex_fields.size() >= limits_.max_vertex_fields) {
                fail(
                    ErrorCode::limit_exceeded,
                    "The vertices section has too many fields",
                    current_.location);
                return;
            }
            if (!is(TokenKind::identifier)) {
                fail(
                    ErrorCode::unexpected_token,
                    "Expected a vertex field name or '}'",
                    current_.location);
                return;
            }
            Token name = std::move(current_);
            advance();
            if (!valid_stable_name(name.text)) {
                fail(
                    ErrorCode::invalid_token,
                    "Vertex field names must be stable slash-separated identifiers",
                    name.location);
                return;
            }
            if (name.text.size() > limits_.max_metadata_key_bytes) {
                fail(
                    ErrorCode::limit_exceeded,
                    "Vertex field name exceeds the configured byte limit",
                    name.location);
                return;
            }
            if (!field_names.insert(name.text).second) {
                fail(
                    ErrorCode::duplicate_field,
                    "Duplicate vertex field '" + name.text + "'",
                    name.location);
                return;
            }
            if (!expect(TokenKind::colon, "after a vertex field name")) {
                return;
            }
            if (!is(TokenKind::identifier)) {
                fail(
                    ErrorCode::unexpected_token,
                    "Expected a logical vertex field type",
                    current_.location);
                return;
            }
            Token type_token = std::move(current_);
            advance();
            const auto type = parse_field_type_name(type_token.text);
            if (!type) {
                fail(
                    ErrorCode::invalid_field_type,
                    "Unsupported vertex field type '" + type_token.text + "'",
                    type_token.location);
                return;
            }
            if (!expect(TokenKind::semicolon, "after a vertex field declaration")) {
                return;
            }

            std::size_t field_values = 0;
            std::size_t next_total = 0;
            if (!checked_multiply(*count, type->components, field_values)
                || !checked_add(total_values, field_values, next_total)
                || next_total > limits_.max_scalar_values) {
                fail(
                    ErrorCode::limit_exceeded,
                    "Vertex scalar count exceeds the configured limit",
                    name.location);
                return;
            }
            total_values = next_total;

            VertexField field{
                .name = name.text,
                .type = *type,
                .values = empty_values(type->scalar),
            };
            if (!consume_decoded_bytes(
                    name.text.size(),
                    sizeof(char),
                    name.location,
                    "Vertex field-name storage")) {
                return;
            }
            if (!consume_decoded_bytes(
                    field_values,
                    sizeof(f32),
                    name.location,
                    "Vertex field storage")) {
                return;
            }
            const bool fits_vector = std::visit(
                [field_values](const auto& values) {
                    return field_values <= values.max_size();
                },
                field.values);
            if (!fits_vector) {
                fail(
                    ErrorCode::limit_exceeded,
                    "Vertex field count exceeds the container limit",
                    name.location);
                return;
            }
            std::visit(
                [field_values](auto& values) { values.reserve(field_values); },
                field.values);
            document.vertex_fields.push_back(std::move(field));
        }

        if (document.vertex_fields.empty()) {
            fail(
                ErrorCode::count_mismatch,
                "A vertices section must declare at least one field",
                current_.location);
            return;
        }
        if (!expect(TokenKind::right_brace, "to close the fields block")
            || !expect_identifier("data")
            || !expect(TokenKind::left_brace, "after 'data'")) {
            return;
        }

        for (std::size_t row = 0; row < *count && !error_; ++row) {
            if (is(TokenKind::right_brace)) {
                fail(
                    ErrorCode::count_mismatch,
                    "Vertex data ended before the declared vertex count",
                    current_.location);
                return;
            }
            for (auto& field : document.vertex_fields) {
                parse_field_value(field);
            }
            static_cast<void>(expect(TokenKind::semicolon, "after a vertex row"));
        }
        if (error_) {
            return;
        }
        if (!is(TokenKind::right_brace)) {
            fail(
                ErrorCode::count_mismatch,
                "Vertex data contains more rows than the declared vertex count",
                current_.location);
            return;
        }
        if (!expect(TokenKind::right_brace, "to close the data block")
            || !expect(TokenKind::right_brace, "to close the vertices section")) {
            return;
        }
    }

    void parse_field_value(VertexField& field)
    {
        if (!expect(TokenKind::left_bracket, "to begin a vertex field value")) {
            return;
        }
        for (u8 component = 0; component < field.type.components && !error_; ++component) {
            if (component != 0
                && !expect(TokenKind::comma, "between vector components")) {
                return;
            }
            switch (field.type.scalar) {
            case ScalarType::Float32:
                if (auto value = parse_float()) {
                    std::get<std::vector<f32>>(field.values).push_back(*value);
                }
                break;
            case ScalarType::Int32:
                if (auto value = parse_i32()) {
                    std::get<std::vector<i32>>(field.values).push_back(*value);
                }
                break;
            case ScalarType::UInt32:
                if (auto value = parse_u32("vertex component")) {
                    std::get<std::vector<u32>>(field.values).push_back(*value);
                }
                break;
            }
        }
        static_cast<void>(expect(TokenKind::right_bracket, "after a vertex field value"));
    }

    [[nodiscard]] std::optional<f32> parse_float()
    {
        if (!is(TokenKind::number)) {
            fail(
                ErrorCode::unexpected_token,
                "Expected a floating-point component",
                current_.location);
            return std::nullopt;
        }
        Token token = std::move(current_);
        advance();
        f32 value{};
        const auto conversion = std::from_chars(
            token.text.data(), token.text.data() + token.text.size(), value,
            std::chars_format::general);
        if (conversion.ec == std::errc::result_out_of_range) {
            fail(
                ErrorCode::number_out_of_range,
                "Floating-point component is out of f32 range",
                token.location);
            return std::nullopt;
        }
        if (conversion.ec != std::errc{}
            || conversion.ptr != token.text.data() + token.text.size()) {
            fail(
                ErrorCode::invalid_number,
                "Invalid floating-point component",
                token.location);
            return std::nullopt;
        }
        if (!std::isfinite(value)) {
            fail(
                ErrorCode::invalid_number,
                "Vertex floating-point values must be finite",
                token.location);
            return std::nullopt;
        }
        return value;
    }

    [[nodiscard]] std::optional<i32> parse_i32()
    {
        if (!is(TokenKind::number)) {
            fail(
                ErrorCode::unexpected_token,
                "Expected a signed integer component",
                current_.location);
            return std::nullopt;
        }
        Token token = std::move(current_);
        advance();
        i32 value{};
        const auto conversion = std::from_chars(
            token.text.data(), token.text.data() + token.text.size(), value);
        if (conversion.ec == std::errc::result_out_of_range) {
            fail(
                ErrorCode::number_out_of_range,
                "Signed vertex component is out of i32 range",
                token.location);
            return std::nullopt;
        }
        if (conversion.ec != std::errc{}
            || conversion.ptr != token.text.data() + token.text.size()) {
            fail(
                ErrorCode::invalid_number,
                "Invalid signed integer component",
                token.location);
            return std::nullopt;
        }
        return value;
    }

    [[nodiscard]] std::optional<u32> parse_u32(std::string_view what)
    {
        if (!is(TokenKind::number)) {
            fail(
                ErrorCode::unexpected_token,
                "Expected an unsigned integer for " + std::string(what),
                current_.location);
            return std::nullopt;
        }
        Token token = std::move(current_);
        advance();
        u32 value{};
        const auto conversion = std::from_chars(
            token.text.data(), token.text.data() + token.text.size(), value);
        if (conversion.ec == std::errc::result_out_of_range) {
            fail(
                ErrorCode::number_out_of_range,
                std::string(what) + " is out of u32 range",
                token.location);
            return std::nullopt;
        }
        if (conversion.ec != std::errc{}
            || conversion.ptr != token.text.data() + token.text.size()) {
            fail(
                ErrorCode::invalid_number,
                "Invalid unsigned integer for " + std::string(what),
                token.location);
            return std::nullopt;
        }
        return value;
    }

    void parse_faces(Document& document)
    {
        if (!expect_identifier("faces")) {
            return;
        }
        const auto count = parse_count(limits_.max_faces, "face");
        if (!count || !expect(TokenKind::left_brace, "after the face count")) {
            return;
        }
        if (*count > document.faces.max_size()
            || !consume_decoded_bytes(
                *count,
                sizeof(gfx::TriangleFace),
                current_.location,
                "Face storage")) {
            if (!error_) {
                fail(
                    ErrorCode::limit_exceeded,
                    "Face count exceeds the container limit",
                    current_.location);
            }
            return;
        }
        document.faces.reserve(*count);
        for (std::size_t index = 0; index < *count && !error_; ++index) {
            if (is(TokenKind::right_brace)) {
                fail(
                    ErrorCode::count_mismatch,
                    "Face data ended before the declared face count",
                    current_.location);
                return;
            }
            const SourceLocation location = current_.location;
            const auto values = parse_index_tuple<3>("face index");
            if (!values) {
                return;
            }
            for (const u32 vertex : *values) {
                if (static_cast<std::size_t>(vertex) >= document.vertex_count) {
                    fail(
                        ErrorCode::index_out_of_range,
                        "Face index is outside the vertex array",
                        location);
                    return;
                }
            }
            document.faces.emplace_back((*values)[0], (*values)[1], (*values)[2]);
            static_cast<void>(expect(TokenKind::semicolon, "after a face"));
        }
        if (error_) {
            return;
        }
        if (!is(TokenKind::right_brace)) {
            fail(
                ErrorCode::count_mismatch,
                "Face data contains more entries than the declared face count",
                current_.location);
            return;
        }
        static_cast<void>(expect(TokenKind::right_brace, "to close the faces section"));
    }

    void parse_edges(Document& document)
    {
        if (!expect_identifier("edges")) {
            return;
        }
        const auto count = parse_count(limits_.max_edges, "edge");
        if (!count || !expect(TokenKind::left_brace, "after the edge count")) {
            return;
        }
        document.edges.emplace();
        if (*count > document.edges->max_size()
            || !consume_decoded_bytes(
                *count,
                sizeof(gfx::Edge),
                current_.location,
                "Edge storage")) {
            if (!error_) {
                fail(
                    ErrorCode::limit_exceeded,
                    "Edge count exceeds the container limit",
                    current_.location);
            }
            return;
        }
        document.edges->reserve(*count);
        for (std::size_t index = 0; index < *count && !error_; ++index) {
            if (is(TokenKind::right_brace)) {
                fail(
                    ErrorCode::count_mismatch,
                    "Edge data ended before the declared edge count",
                    current_.location);
                return;
            }
            const SourceLocation location = current_.location;
            const auto values = parse_index_tuple<2>("edge index");
            if (!values) {
                return;
            }
            for (const u32 vertex : *values) {
                if (static_cast<std::size_t>(vertex) >= document.vertex_count) {
                    fail(
                        ErrorCode::index_out_of_range,
                        "Edge index is outside the vertex array",
                        location);
                    return;
                }
            }
            document.edges->emplace_back((*values)[0], (*values)[1]);
            static_cast<void>(expect(TokenKind::semicolon, "after an edge"));
        }
        if (error_) {
            return;
        }
        if (!is(TokenKind::right_brace)) {
            fail(
                ErrorCode::count_mismatch,
                "Edge data contains more entries than the declared edge count",
                current_.location);
            return;
        }
        static_cast<void>(expect(TokenKind::right_brace, "to close the edges section"));
    }

    template<std::size_t Size>
    [[nodiscard]] std::optional<std::array<u32, Size>> parse_index_tuple(
        std::string_view what)
    {
        if (!expect(TokenKind::left_bracket, "to begin an index tuple")) {
            return std::nullopt;
        }
        std::array<u32, Size> result{};
        for (std::size_t index = 0; index < Size; ++index) {
            if (index != 0 && !expect(TokenKind::comma, "between indices")) {
                return std::nullopt;
            }
            const auto value = parse_u32(what);
            if (!value) {
                return std::nullopt;
            }
            result[index] = *value;
        }
        if (!expect(TokenKind::right_bracket, "after an index tuple")) {
            return std::nullopt;
        }
        return result;
    }

    [[nodiscard]] bool consume_decoded_bytes(
        std::size_t count,
        std::size_t element_size,
        SourceLocation location,
        std::string_view what)
    {
        std::size_t bytes = 0;
        std::size_t next = 0;
        if (!checked_multiply(count, element_size, bytes)
            || !checked_add(decoded_bytes_, bytes, next)
            || next > limits_.max_decoded_bytes) {
            fail(
                ErrorCode::limit_exceeded,
                std::string(what) + " exceeds the configured decoded-byte budget",
                location);
            return false;
        }
        decoded_bytes_ = next;
        return true;
    }

    Lexer lexer_;
    const Limits& limits_;
    std::size_t decoded_bytes_{};
    Token current_{};
    std::optional<Diagnostic> error_;
};

[[nodiscard]] Result<void> validate_document(
    const Document& document,
    const Limits* limits)
{
    if (limits && document.vertex_count > limits->max_vertices) {
        return std::unexpected(diagnostic(
            ErrorCode::limit_exceeded,
            "Vertex count exceeds the configured limit"));
    }
    if (document.vertex_count > static_cast<std::size_t>(
            std::numeric_limits<u32>::max())) {
        return std::unexpected(diagnostic(
            ErrorCode::limit_exceeded,
            "Vertex count exceeds the .vmesh v1 u32 count limit"));
    }
    std::size_t decoded_bytes = 0;
    const auto consume_decoded_bytes = [&](std::size_t count, std::size_t size) {
        std::size_t bytes = 0;
        std::size_t next = 0;
        if (!checked_multiply(count, size, bytes)
            || !checked_add(decoded_bytes, bytes, next)) {
            return false;
        }
        decoded_bytes = next;
        return !limits || decoded_bytes <= limits->max_decoded_bytes;
    };
    if (limits && document.metadata.size() > limits->max_metadata_entries) {
        return std::unexpected(diagnostic(
            ErrorCode::limit_exceeded,
            "Metadata entry count exceeds the configured limit"));
    }
    for (const auto& [key, value] : document.metadata) {
        if (!valid_stable_name(key)) {
            return std::unexpected(diagnostic(
                ErrorCode::invalid_document,
                "Invalid metadata key '" + key + "'"));
        }
        if (limits
            && (key.size() > limits->max_metadata_key_bytes
                || value.size() > limits->max_metadata_value_bytes)) {
            return std::unexpected(diagnostic(
                ErrorCode::limit_exceeded,
                "Metadata key or value exceeds the configured byte limit"));
        }
        if (!valid_utf8(value)) {
            return std::unexpected(diagnostic(
                ErrorCode::invalid_utf8,
                "Metadata value for '" + key + "' is not valid UTF-8"));
        }
        if (!consume_decoded_bytes(key.size(), sizeof(char))
            || !consume_decoded_bytes(value.size(), sizeof(char))) {
            return std::unexpected(diagnostic(
                ErrorCode::limit_exceeded,
                "Metadata exceeds the configured decoded-byte budget"));
        }
    }

    if (document.vertex_fields.empty()) {
        return std::unexpected(diagnostic(
            ErrorCode::invalid_document,
            "A .vmesh document must contain at least one vertex field"));
    }
    if (limits && document.vertex_fields.size() > limits->max_vertex_fields) {
        return std::unexpected(diagnostic(
            ErrorCode::limit_exceeded,
            "Vertex field count exceeds the configured limit"));
    }

    std::unordered_set<std::string> field_names;
    std::size_t total_values = 0;
    for (const auto& field : document.vertex_fields) {
        if (!valid_stable_name(field.name)) {
            return std::unexpected(diagnostic(
                ErrorCode::invalid_document,
                "Invalid vertex field name '" + field.name + "'"));
        }
        if (limits && field.name.size() > limits->max_metadata_key_bytes) {
            return std::unexpected(diagnostic(
                ErrorCode::limit_exceeded,
                "Vertex field name exceeds the configured byte limit"));
        }
        if (!field_names.insert(field.name).second) {
            return std::unexpected(diagnostic(
                ErrorCode::duplicate_field,
                "Duplicate vertex field '" + field.name + "'"));
        }
        if (!consume_decoded_bytes(field.name.size(), sizeof(char))) {
            return std::unexpected(diagnostic(
                ErrorCode::limit_exceeded,
                "Vertex field names exceed the configured decoded-byte budget"));
        }
        if (field.type.components < 1 || field.type.components > 4) {
            return std::unexpected(diagnostic(
                ErrorCode::invalid_field_type,
                "A vertex field must have between one and four components"));
        }

        const bool variant_matches =
            (field.type.scalar == ScalarType::Float32
             && std::holds_alternative<std::vector<f32>>(field.values))
            || (field.type.scalar == ScalarType::Int32
                && std::holds_alternative<std::vector<i32>>(field.values))
            || (field.type.scalar == ScalarType::UInt32
                && std::holds_alternative<std::vector<u32>>(field.values));
        if (!variant_matches) {
            return std::unexpected(diagnostic(
                ErrorCode::invalid_field_type,
                "The value storage for vertex field '" + field.name
                    + "' does not match its declared scalar type"));
        }

        std::size_t expected_values = 0;
        std::size_t next_total = 0;
        if (!checked_multiply(
                document.vertex_count, field.type.components, expected_values)
            || !checked_add(total_values, expected_values, next_total)
            || (limits && next_total > limits->max_scalar_values)) {
            return std::unexpected(diagnostic(
                ErrorCode::limit_exceeded,
                "Vertex scalar count exceeds the configured limit"));
        }
        total_values = next_total;
        if (!consume_decoded_bytes(expected_values, sizeof(f32))) {
            return std::unexpected(diagnostic(
                ErrorCode::limit_exceeded,
                "Vertex fields exceed the configured decoded-byte budget"));
        }
        const std::size_t actual_values = std::visit(
            [](const auto& values) { return values.size(); }, field.values);
        if (actual_values != expected_values) {
            return std::unexpected(diagnostic(
                ErrorCode::count_mismatch,
                "Vertex field '" + field.name
                    + "' does not contain vertex_count * component_count values"));
        }
        if (field.type.scalar == ScalarType::Float32) {
            const auto& values = std::get<std::vector<f32>>(field.values);
            if (std::ranges::any_of(values, [](f32 value) {
                    return !std::isfinite(value);
                })) {
                return std::unexpected(diagnostic(
                    ErrorCode::invalid_number,
                    "Vertex field '" + field.name + "' contains a non-finite value"));
            }
        }
    }

    if (limits && document.faces.size() > limits->max_faces) {
        return std::unexpected(diagnostic(
            ErrorCode::limit_exceeded,
            "Face count exceeds the configured limit"));
    }
    if (!consume_decoded_bytes(document.faces.size(), sizeof(gfx::TriangleFace))) {
        return std::unexpected(diagnostic(
            ErrorCode::limit_exceeded,
            "Face storage exceeds the configured decoded-byte budget"));
    }
    for (const auto& face : document.faces) {
        if (std::ranges::any_of(face.vertices, [&](u32 vertex) {
                return static_cast<std::size_t>(vertex) >= document.vertex_count;
            })) {
            return std::unexpected(diagnostic(
                ErrorCode::index_out_of_range,
                "Face index is outside the vertex array"));
        }
    }

    if (document.edges) {
        if (limits && document.edges->size() > limits->max_edges) {
            return std::unexpected(diagnostic(
                ErrorCode::limit_exceeded,
                "Edge count exceeds the configured limit"));
        }
        if (!consume_decoded_bytes(document.edges->size(), sizeof(gfx::Edge))) {
            return std::unexpected(diagnostic(
                ErrorCode::limit_exceeded,
                "Edge storage exceeds the configured decoded-byte budget"));
        }
        for (const auto& edge : *document.edges) {
            if (std::ranges::any_of(edge.vertices, [&](u32 vertex) {
                    return static_cast<std::size_t>(vertex) >= document.vertex_count;
                })) {
                return std::unexpected(diagnostic(
                    ErrorCode::index_out_of_range,
                    "Edge index is outside the vertex array"));
            }
        }
    }
    return {};
}

class Output final {
public:
    explicit Output(std::size_t limit) noexcept
        : limit_(limit)
    {}

    void append(std::string_view text)
    {
        if (failed_) {
            return;
        }
        if (text.size() > limit_ - std::min(limit_, text_.size())) {
            failed_ = true;
            return;
        }
        text_.append(text);
    }

    void push(char character)
    {
        append(std::string_view(&character, 1));
    }

    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] std::string take() && { return std::move(text_); }

private:
    std::size_t limit_{};
    std::string text_;
    bool failed_{};
};

void append_escaped(Output& output, std::string_view value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    output.push('"');
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
        case '"': output.append("\\\""); break;
        case '\\': output.append("\\\\"); break;
        case '\b': output.append("\\b"); break;
        case '\f': output.append("\\f"); break;
        case '\n': output.append("\\n"); break;
        case '\r': output.append("\\r"); break;
        case '\t': output.append("\\t"); break;
        default:
            if (character < 0x20U) {
                std::array<char, 6> escaped{
                    '\\', 'u', '0', '0',
                    hex[(character >> 4U) & 0x0FU],
                    hex[character & 0x0FU],
                };
                output.append(std::string_view(escaped.data(), escaped.size()));
            } else {
                output.push(static_cast<char>(character));
            }
            break;
        }
    }
    output.push('"');
}

template<class Number>
void append_integer(Output& output, Number value)
{
    std::array<char, 32> buffer{};
    const auto conversion = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (conversion.ec == std::errc{}) {
        output.append(std::string_view(buffer.data(), conversion.ptr));
    }
}

void append_float(Output& output, f32 value)
{
    std::array<char, 64> buffer{};
    const auto conversion = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value,
        std::chars_format::general, std::numeric_limits<f32>::max_digits10);
    if (conversion.ec == std::errc{}) {
        output.append(std::string_view(buffer.data(), conversion.ptr));
    }
}

void append_scalar(Output& output, const VertexField& field, std::size_t index)
{
    switch (field.type.scalar) {
    case ScalarType::Float32:
        append_float(output, std::get<std::vector<f32>>(field.values)[index]);
        break;
    case ScalarType::Int32:
        append_integer(output, std::get<std::vector<i32>>(field.values)[index]);
        break;
    case ScalarType::UInt32:
        append_integer(output, std::get<std::vector<u32>>(field.values)[index]);
        break;
    }
}

} // namespace

bool is_valid_name(std::string_view name) noexcept
{
    return valid_stable_name(name);
}

Result<void> validate(const Document& document)
{
    return validate_document(document, nullptr);
}

Result<void> validate(const Document& document, const Limits& limits)
{
    return validate_document(document, &limits);
}

Result<Document> parse_vmesh(std::string_view source, const ReadOptions& options)
{
    if (source.size() > options.limits.max_source_bytes) {
        return std::unexpected(diagnostic(
            ErrorCode::input_too_large,
            ".vmesh source exceeds the configured byte limit",
            SourceLocation{}));
    }
    return Parser(source, options).parse();
}

Result<Document> read_vmesh(
    const std::filesystem::path& path,
    const ReadOptions& options)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        auto error = diagnostic(ErrorCode::io_error, "Could not open .vmesh file");
        error.path = path;
        return std::unexpected(std::move(error));
    }

    const std::streampos end = input.tellg();
    if (end < std::streampos{0}) {
        auto error = diagnostic(ErrorCode::io_error, "Could not determine .vmesh file size");
        error.path = path;
        return std::unexpected(std::move(error));
    }
    const auto size = static_cast<std::uintmax_t>(end);
    if (size > options.limits.max_source_bytes
        || size > std::numeric_limits<std::size_t>::max()
        || size > static_cast<std::uintmax_t>(
            std::numeric_limits<std::streamsize>::max())) {
        auto error = diagnostic(
            ErrorCode::input_too_large,
            ".vmesh file exceeds the configured byte limit");
        error.path = path;
        return std::unexpected(std::move(error));
    }

    std::string source(static_cast<std::size_t>(size), '\0');
    input.seekg(0, std::ios::beg);
    if (!source.empty()) {
        input.read(source.data(), static_cast<std::streamsize>(source.size()));
    }
    if (!input) {
        auto error = diagnostic(ErrorCode::io_error, "Could not read complete .vmesh file");
        error.path = path;
        return std::unexpected(std::move(error));
    }

    auto parsed = parse_vmesh(source, options);
    if (!parsed) {
        parsed.error().path = path;
    }
    return parsed;
}

Result<std::string> write_vmesh(
    const Document& document,
    const WriteOptions& options)
{
    if (auto valid = validate(document, options.limits); !valid) {
        return std::unexpected(std::move(valid.error()));
    }

    Output output(options.limits.max_source_bytes);
    output.append("vmesh 1.0\n");
    output.append("info {\n");
    for (const auto& [key, value] : document.metadata) {
        output.append("    ");
        output.append(key);
        output.append(" = ");
        append_escaped(output, value);
        output.append(";\n");
    }
    output.append("}\n");

    output.append("vertices ");
    append_integer(output, document.vertex_count);
    output.append(" {\n");
    output.append("    fields {\n");
    for (const auto& field : document.vertex_fields) {
        output.append("        ");
        output.append(field.name);
        output.append(" : ");
        output.append(field_type_name(field.type));
        output.append(";\n");
    }
    output.append("    }\n");
    output.append("    data {\n");
    for (std::size_t vertex = 0; vertex < document.vertex_count; ++vertex) {
        output.append("        ");
        for (std::size_t field_index = 0;
             field_index < document.vertex_fields.size(); ++field_index) {
            if (field_index != 0) {
                output.push(' ');
            }
            const auto& field = document.vertex_fields[field_index];
            output.push('[');
            for (u8 component = 0; component < field.type.components; ++component) {
                if (component != 0) {
                    output.append(", ");
                }
                const std::size_t value_index =
                    vertex * field.type.components + component;
                append_scalar(output, field, value_index);
            }
            output.push(']');
        }
        output.append(";\n");
    }
    output.append("    }\n");
    output.append("}\n");

    output.append("faces ");
    append_integer(output, document.faces.size());
    output.append(" {\n");
    for (const auto& face : document.faces) {
        output.append("    [");
        append_integer(output, face.vertices[0]);
        output.append(", ");
        append_integer(output, face.vertices[1]);
        output.append(", ");
        append_integer(output, face.vertices[2]);
        output.append("];\n");
    }
    output.append("}\n");

    if (document.edges) {
        output.append("edges ");
        append_integer(output, document.edges->size());
        output.append(" {\n");
        for (const auto& edge : *document.edges) {
            output.append("    [");
            append_integer(output, edge.vertices[0]);
            output.append(", ");
            append_integer(output, edge.vertices[1]);
            output.append("];\n");
        }
        output.append("}\n");
    }

    if (output.failed()) {
        return std::unexpected(diagnostic(
            ErrorCode::input_too_large,
            "Serialized .vmesh text exceeds the configured byte limit"));
    }
    return std::move(output).take();
}

Result<void> write_vmesh(
    const std::filesystem::path& path,
    const Document& document,
    const WriteOptions& options)
{
    auto serialized = write_vmesh(document, options);
    if (!serialized) {
        serialized.error().path = path;
        return std::unexpected(std::move(serialized.error()));
    }
    if (serialized->size() > static_cast<std::size_t>(
            std::numeric_limits<std::streamsize>::max())) {
        auto error = diagnostic(
            ErrorCode::input_too_large,
            ".vmesh output exceeds the platform stream-size limit");
        error.path = path;
        return std::unexpected(std::move(error));
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        auto error = diagnostic(ErrorCode::io_error, "Could not open .vmesh file for writing");
        error.path = path;
        return std::unexpected(std::move(error));
    }
    output.write(serialized->data(), static_cast<std::streamsize>(serialized->size()));
    output.flush();
    if (!output) {
        auto error = diagnostic(ErrorCode::io_error, "Could not write complete .vmesh file");
        error.path = path;
        return std::unexpected(std::move(error));
    }
    return {};
}

} // namespace vng::content::vmesh
