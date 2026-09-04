#include <vng/shader/type.hpp>

#include <stdexcept>

namespace vng::shader {

TypeTable::TypeTable()
{
    TypeDescription poison;
    poison.kind = TypeKind::poison;
    poison.name = "poison";
    poison.canonical_key = "poison";
    types_.push_back(std::move(poison));
}

TypeId TypeTable::intern(TypeDescription description)
{
    for (u32 index = 0; index < types_.size(); ++index) {
        if (types_[index].canonical_key == description.canonical_key) {
            return TypeId{index};
        }
    }

    const auto id = TypeId{static_cast<u32>(types_.size())};
    types_.push_back(std::move(description));
    return id;
}

const TypeDescription& TypeTable::operator[](TypeId id) const
{
    if (!id.valid() || id.value >= types_.size()) {
        throw std::out_of_range("invalid shader TypeId");
    }
    return types_[id.value];
}

} // namespace vng::shader
