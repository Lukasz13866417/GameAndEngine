#pragma once

#include <cstddef>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <vng/gfx/record.hpp>

namespace vng::gfx {

template<class RecordType>
    requires is_record_v<RecordType>
class VertexStream {
    static_assert(std::is_trivially_copyable_v<RecordType>);
    static_assert(sizeof(RecordType) == RecordType::stride,
                  "Record must contain only its explicit encoded byte storage");

public:
    using record_type = RecordType;
    using value_type = RecordType;
    using size_type = std::size_t;
    using iterator = typename std::vector<RecordType>::iterator;
    using const_iterator = typename std::vector<RecordType>::const_iterator;

    VertexStream() = default;

    explicit VertexStream(size_type count)
        : records_(count)
    {}

    VertexStream(std::initializer_list<RecordType> records)
        : records_(records)
    {}

    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }
    [[nodiscard]] size_type size() const noexcept { return records_.size(); }
    [[nodiscard]] size_type byte_size() const noexcept { return records_.size() * RecordType::stride; }

    void resize(size_type count) { records_.resize(count); }
    void reserve(size_type count) { records_.reserve(count); }
    void clear() noexcept { records_.clear(); }

    template<class... Args>
    RecordType& emplace_back(Args&&... args)
    {
        return records_.emplace_back(std::forward<Args>(args)...);
    }

    void push_back(const RecordType& record) { records_.push_back(record); }
    void push_back(RecordType&& record) { records_.push_back(std::move(record)); }

    [[nodiscard]] RecordType& operator[](size_type index) noexcept { return records_[index]; }
    [[nodiscard]] const RecordType& operator[](size_type index) const noexcept { return records_[index]; }

    [[nodiscard]] RecordType* data() noexcept { return records_.data(); }
    [[nodiscard]] const RecordType* data() const noexcept { return records_.data(); }

    [[nodiscard]] iterator begin() noexcept { return records_.begin(); }
    [[nodiscard]] iterator end() noexcept { return records_.end(); }
    [[nodiscard]] const_iterator begin() const noexcept { return records_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return records_.end(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return records_.cbegin(); }
    [[nodiscard]] const_iterator cend() const noexcept { return records_.cend(); }

    [[nodiscard]] std::span<std::byte> bytes() noexcept
    {
        return std::as_writable_bytes(std::span{records_});
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept
    {
        return std::as_bytes(std::span{records_});
    }

private:
    std::vector<RecordType> records_;
};

} // namespace vng::gfx
