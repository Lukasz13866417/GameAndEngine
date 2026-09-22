#include <vng/shader/dsl/dsl.hpp>
auto rejected(vng::dsl::Float expression, float mutable_value)
{
    return vng::dsl::mix(expression, 1.0F, mutable_value);
}
