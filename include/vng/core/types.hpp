#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace vng {

using i8 = std::int8_t;
using u8 = std::uint8_t;
using i16 = std::int16_t;
using u16 = std::uint16_t;
using i32 = std::int32_t;
using u32 = std::uint32_t;
using i64 = std::int64_t;
using u64 = std::uint64_t;
using f32 = float;
using f64 = double;

struct Extent2D final {
    u32 width{};
    u32 height{};

    [[nodiscard]] constexpr bool empty() const noexcept
    {
        return width == 0 || height == 0;
    }

    friend constexpr bool operator==(const Extent2D&, const Extent2D&) = default;
};

template<class T, std::size_t N>
struct Vector;

template<class T>
struct Vector<T, 2> {
    using value_type = T;
    static constexpr std::size_t component_count = 2;

    T x{};
    T y{};

    [[nodiscard]] constexpr T& operator[](std::size_t index) noexcept
    {
        return index == 0 ? x : y;
    }

    [[nodiscard]] constexpr const T& operator[](std::size_t index) const noexcept
    {
        return index == 0 ? x : y;
    }

    friend constexpr bool operator==(const Vector&, const Vector&) = default;
};

template<class T>
struct Vector<T, 3> {
    using value_type = T;
    static constexpr std::size_t component_count = 3;

    T x{};
    T y{};
    T z{};

    [[nodiscard]] constexpr T& operator[](std::size_t index) noexcept
    {
        if (index == 0) {
            return x;
        }
        return index == 1 ? y : z;
    }

    [[nodiscard]] constexpr const T& operator[](std::size_t index) const noexcept
    {
        if (index == 0) {
            return x;
        }
        return index == 1 ? y : z;
    }

    friend constexpr bool operator==(const Vector&, const Vector&) = default;
};

template<class T>
struct Vector<T, 4> {
    using value_type = T;
    static constexpr std::size_t component_count = 4;

    T x{};
    T y{};
    T z{};
    T w{};

    [[nodiscard]] constexpr T& operator[](std::size_t index) noexcept
    {
        if (index == 0) {
            return x;
        }
        if (index == 1) {
            return y;
        }
        return index == 2 ? z : w;
    }

    [[nodiscard]] constexpr const T& operator[](std::size_t index) const noexcept
    {
        if (index == 0) {
            return x;
        }
        if (index == 1) {
            return y;
        }
        return index == 2 ? z : w;
    }

    friend constexpr bool operator==(const Vector&, const Vector&) = default;
};

template<class T>
struct is_vector : std::false_type {};

template<class T, std::size_t N>
struct is_vector<Vector<T, N>> : std::true_type {};

template<class T>
inline constexpr bool is_vector_v = is_vector<std::remove_cv_t<T>>::value;

template<class T>
struct vector_traits;

template<class T, std::size_t N>
struct vector_traits<Vector<T, N>> {
    using value_type = T;
    static constexpr std::size_t component_count = N;
};

using Vec2 = Vector<f32, 2>;
using Vec3 = Vector<f32, 3>;
using Vec4 = Vector<f32, 4>;

using IVec2 = Vector<i32, 2>;
using IVec3 = Vector<i32, 3>;
using IVec4 = Vector<i32, 4>;

using UVec2 = Vector<u32, 2>;
using UVec3 = Vector<u32, 3>;
using UVec4 = Vector<u32, 4>;

template<std::size_t N>
struct Matrix;

template<>
struct Matrix<3> {
    using value_type = f32;
    using column_type = Vec3;
    static constexpr std::size_t column_count = 3;
    static constexpr std::size_t row_count = 3;

    std::array<Vec3, 3> columns{};

    [[nodiscard]] constexpr Vec3& operator[](std::size_t column) noexcept
    {
        return columns[column];
    }

    [[nodiscard]] constexpr const Vec3& operator[](std::size_t column) const noexcept
    {
        return columns[column];
    }

    [[nodiscard]] static constexpr Matrix identity() noexcept
    {
        Matrix result{};
        result.columns = {
            Vec3{1.0F, 0.0F, 0.0F},
            Vec3{0.0F, 1.0F, 0.0F},
            Vec3{0.0F, 0.0F, 1.0F},
        };
        return result;
    }

    friend constexpr bool operator==(const Matrix&, const Matrix&) = default;
};

template<>
struct Matrix<4> {
    using value_type = f32;
    using column_type = Vec4;
    static constexpr std::size_t column_count = 4;
    static constexpr std::size_t row_count = 4;

    std::array<Vec4, 4> columns{};

    [[nodiscard]] constexpr Vec4& operator[](std::size_t column) noexcept
    {
        return columns[column];
    }

    [[nodiscard]] constexpr const Vec4& operator[](std::size_t column) const noexcept
    {
        return columns[column];
    }

    [[nodiscard]] static constexpr Matrix identity() noexcept
    {
        Matrix result{};
        result.columns = {
            Vec4{1.0F, 0.0F, 0.0F, 0.0F},
            Vec4{0.0F, 1.0F, 0.0F, 0.0F},
            Vec4{0.0F, 0.0F, 1.0F, 0.0F},
            Vec4{0.0F, 0.0F, 0.0F, 1.0F},
        };
        return result;
    }

    friend constexpr bool operator==(const Matrix&, const Matrix&) = default;
};

using Mat3 = Matrix<3>;
using Mat4 = Matrix<4>;

static_assert(sizeof(i32) == 4);
static_assert(sizeof(u32) == 4);
static_assert(sizeof(f32) == 4);
static_assert(std::is_standard_layout_v<Vec2>);
static_assert(std::is_standard_layout_v<Vec3>);
static_assert(std::is_standard_layout_v<Vec4>);

} // namespace vng

namespace vng::core {

using ::vng::f32;
using ::vng::f64;
using ::vng::Extent2D;
using ::vng::i8;
using ::vng::i16;
using ::vng::i32;
using ::vng::i64;
using ::vng::u8;
using ::vng::u16;
using ::vng::u32;
using ::vng::u64;
using ::vng::IVec2;
using ::vng::IVec3;
using ::vng::IVec4;
using ::vng::Mat3;
using ::vng::Mat4;
using ::vng::UVec2;
using ::vng::UVec3;
using ::vng::UVec4;
using ::vng::Vec2;
using ::vng::Vec3;
using ::vng::Vec4;

} // namespace vng::core
