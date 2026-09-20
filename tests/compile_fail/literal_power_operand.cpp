#include <vng/dsl/dsl.hpp>

auto rejected(vng::dsl::Float expression, float exponent)
{
    return vng::dsl::pow(expression, exponent);
}
