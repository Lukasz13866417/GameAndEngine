#include <vng/shader/shader.hpp>
int main()
{
    using In = vng::shader::FragmentInputs<>;
    using Out = vng::shader::FragmentOutputs<vng::shader::Color<0>>;
    auto stage = vng::shader::fragment<In, Out>([](auto& s, float value) {
        return s.output(vng::dsl::field<vng::shader::Color<0>>(s.constant(vng::Vec4{value, 0, 0, 1})));
    });
}
