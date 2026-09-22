#include <vng/shader/dsl/dsl.hpp>
auto rejected(vng::dsl::Float4 expression, vng::Mat4 matrix)
{
    return matrix * expression;
}
