#include <vng/dsl/dsl.hpp>
auto rejected(vng::dsl::Int expression)
{
    int mutable_value = 12;
    return expression * mutable_value;
}
