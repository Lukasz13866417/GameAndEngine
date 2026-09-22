#include <vng/shader/dsl/dsl.hpp>
auto rejected(vng::dsl::Float3 expression, float mutable_bound)
{
    return vng::dsl::clamp(expression, mutable_bound, 1.0F);
}
