#pragma once

#include <cassert>
#include <cstdint>

#include <initializer_list>
#include <memory>
#include <utility>

namespace gct {

// A stack-allocated vector constrained by a constant value capacity
// Attempts to push more objects into the container or construct more objects than Capacity results in undefined behaviour.
// This is useful for when you want stack allocation but can't or don't want to default-construct objects like with std::array.
template <typename T, uint32_t Capacity>
class static_vector {

    static_assert(Capacity > 0);

    alignas(T) uint8_t m_buffer[sizeof(T) * Capacity];
    uint32_t m_size{};

public:
    static_vector() {}
    static_vector(uint32_t count, const T& value)
    {
        assert(count <= Capacity);
        for (uint32_t i = 0; i < count; ++i) {
            push_back(value);
        }
    }
    static_vector(std::initializer_list<T> init)
    {
        assert(init.size() <= Capacity);
        for (const T& value : init) {
            push_back(value);
        }
    }
    static_vector(const static_vector& other) : m_size(other.size())
    {
        const T* const other_typed_ptr = reinterpret_cast<const T*>(other.m_buffer);
        T* const typed_ptr = reinterpret_cast<T*>(m_buffer);
        std::uninitialized_copy(other_typed_ptr, other_typed_ptr + other.size(), typed_ptr);
    }
    static_vector(static_vector&& other) noexcept : m_size(other.size())
    {
        T* const other_typed_ptr = reinterpret_cast<T*>(other.m_buffer);
        T* const typed_ptr = reinterpret_cast<T*>(m_buffer);
        std::uninitialized_move(other_typed_ptr, other_typed_ptr + other.size(), typed_ptr);
    }

    ~static_vector() { std::destroy_n(begin(), m_size); }

    static_vector& operator=(const static_vector& other)
    {
        clear();
        m_size = other.size();
        const T* const other_typed_ptr = reinterpret_cast<const T*>(other.m_buffer);
        T* const typed_ptr = reinterpret_cast<T*>(m_buffer);
        std::uninitialized_copy(other_typed_ptr, other_typed_ptr + other.size(), typed_ptr);
        return *this;
    }
    static_vector& operator=(static_vector&& other) noexcept
    {
        clear();
        m_size = other.size();
        T* const other_typed_ptr = reinterpret_cast<T*>(other.m_buffer);
        T* const typed_ptr = reinterpret_cast<T*>(m_buffer);
        std::uninitialized_move(other_typed_ptr, other_typed_ptr + other.size(), typed_ptr);
        return *this;
    }
    static_vector& operator=(std::initializer_list<T> ilist)
    {
        assert(ilist.size() <= Capacity);
        clear();
        for (const T& value : ilist) {
            push_back(value);
        }
        return *this;
    }

    // ELEMENT ACCESS

    T& operator[](uint32_t pos)
    {
        assert(pos < m_size);
        return *(reinterpret_cast<T*>(m_buffer) + pos);
    }
    const T& operator[](uint32_t pos) const
    {
        assert(pos < m_size);
        return *(reinterpret_cast<const T*>(m_buffer) + pos);
    }

    T& front()
    {
        assert(m_size != 0);
        return *(reinterpret_cast<T*>(m_buffer));
    }
    const T& front() const
    {
        assert(m_size != 0);
        return *(reinterpret_cast<const T*>(m_buffer));
    }

    T& back()
    {
        assert(m_size != 0);
        return *(reinterpret_cast<T*>(m_buffer) + m_size - 1);
    }
    const T& back() const
    {
        assert(m_size != 0);
        return *(reinterpret_cast<const T*>(m_buffer) + m_size - 1);
    }

    // Unlike std::vector, data() is never invalidated
    T* data() { return reinterpret_cast<T*>(m_buffer); }
    const T* data() const { return reinterpret_cast<const T*>(m_buffer); }

    // ITERATORS

    // Unlike std::vector, begin() is never invalidated
    T* begin() { return data(); }
    const T* begin() const { return data(); }
    const T* cbegin() const { return begin(); }

    T* end() { return data() + m_size; }
    const T* end() const { return data() + m_size; }
    const T* cend() const { return end(); }

    // CAPACITY

    bool empty() const { return m_size == 0; }

    bool full() const { return m_size == Capacity; }

    uint32_t size() const { return m_size; }

    constexpr uint32_t capacity() const { return Capacity; }

    // MODIFIERS

    void clear()
    {
        while (!empty()) {
            pop_back();
        }
    }

    void push_back(const T& value) { emplace_back(value); }

    void push_back(T&& value) { emplace_back(std::move(value)); }

    template <class... Args>
    T& emplace_back(Args&&... args)
    {
        assert(m_size < Capacity);
        T* const typed_buffer = reinterpret_cast<T*>(m_buffer);
        std::construct_at(typed_buffer + m_size, std::forward<Args>(args)...);
        ++m_size;
        return back();
    }

    // UB if empty
    void pop_back()
    {
        assert(m_size != 0);
        --m_size;
        std::destroy_at(&reinterpret_cast<T*>(m_buffer)[m_size]);
    }

    void resize(uint32_t count)
    {
        assert(count <= Capacity);
        if (count < m_size) {
            while (m_size > count) {
                pop_back();
            }
        }
        else if (count > m_size) {
            while (m_size < count) {
                push_back(T{});
            }
        }
    }
};

} // namespace gct