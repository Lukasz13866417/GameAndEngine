#include <vng/shader/dsl/dsl.hpp>
auto rejected(vng::dsl::Float3 expression, float mutable_value)
{
    return expression + vng::Vec3{mutable_value, 0.0F, 1.0F};
}
