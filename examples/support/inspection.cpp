#include "inspection.hpp"

#include <iostream>

namespace example {

void print_section(std::string_view title, std::string_view contents)
{
    std::cout << "\n=== " << title << " ===\n" << contents;
    if (contents.empty() || contents.back() != '\n') {
        std::cout << '\n';
    }
}

} // namespace example
