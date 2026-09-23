#pragma once

// A basic fixed-size ring buffer.
// This class is not thread safe

#include <cstddef>

#include <array>
#include <optional>

namespace gct {

template <typename T, std::size_t Size>
class RingBuffer {

    static_assert(Size > 0);

    std::array<T, Size> m_buffer{};
    size_t m_head{0};
    size_t m_tail{0};

public:
    RingBuffer() {}

    ~RingBuffer() {}

    bool pushBack(T item)
    {
        const size_t next = (m_head + 1) % m_buffer.size();
        if (next != m_tail) {
            m_buffer[m_head] = std::move(item);
            m_head = next;
            return true;
        }
        else {
            return false;
        }
    }

    std::optional<T> popFront()
    {
        if (m_tail != m_head) {
            T item = m_buffer[m_tail];
            m_tail = (m_tail + 1) % m_buffer.size();
            return item;
        }
        else {
            return {};
        }
    }
};

} // namespace gct
