#include <vng/shader/dsl/dsl.hpp>
int runtime_value();
auto rejected(vng::dsl::Int expression)
{
    return expression * (runtime_value() + 0);
}
