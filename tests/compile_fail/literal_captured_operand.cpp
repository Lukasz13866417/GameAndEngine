#include <vng/dsl/dsl.hpp>
auto rejected(vng::dsl::Float expression, float brightness)
{
    auto body = [brightness](vng::dsl::Float value) {
        return value * brightness;
    };
    return body(expression);
}
