#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace editor_example {
// Constructed on demand. Values are owned: a report may outlive the dispatch
// which supplied its context. Facts are supplied explicitly by their owner;
// collecting diagnostics cannot query another component or trigger work.
struct DebugReport {
    struct Fact { std::string name, value; };
    std::string name{}, role{}, situation{};
    std::vector<Fact> received{}, owned{}, observations{};
    std::vector<DebugReport> children{};

    [[nodiscard]] std::string string() const {
        std::string result;
        append(result, {}, 0);
        return result;
    }
private:
    void append(std::string& result, std::string_view parent, unsigned depth) const {
        const std::string path = parent.empty() ? name : std::string(parent) + "." + name;
        const std::string indent(depth * 2, ' ');
        result += indent + path + "\n";
        result += indent + "  role: " + role + "\n";
        result += indent + "  situation: " + situation + "\n";
        const auto section = [&](std::string_view title, const std::vector<Fact>& facts) {
            if (facts.empty()) return;
            result += indent + "  " + std::string(title) + ":\n";
            for (const auto& fact : facts)
                result += indent + "    " + fact.name + ": " + fact.value + "\n";
        };
        section("received", received);
        section("owned", owned);
        section("observations", observations);
        for (const auto& child : children) child.append(result, path, depth + 1);
    }
};
inline std::string debug_bool(bool value) { return value ? "yes" : "no"; }
} // namespace editor_example
