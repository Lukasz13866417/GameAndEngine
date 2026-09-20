#include <vng/dsl/dsl.hpp>
auto rejected(vng::dsl::Float expression, float mutable_value)
{
    return vng::dsl::make<vng::Vec2>(expression, mutable_value);
}
