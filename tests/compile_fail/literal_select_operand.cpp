#include <vng/shader/dsl/dsl.hpp>
auto rejected(vng::dsl::Bool condition, float mutable_value)
{
    return vng::dsl::select(condition, 1.0F, mutable_value);
}
