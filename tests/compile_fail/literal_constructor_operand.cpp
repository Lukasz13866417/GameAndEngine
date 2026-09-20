#include <vng/dsl/dsl.hpp>
auto rejected(vng::dsl::Float expression, float mutable_value)
{
    return vng::dsl::vec4(expression, 0.0F, mutable_value, 1.0F);
}
