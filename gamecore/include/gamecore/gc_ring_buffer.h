#pragma once

// A basic fixed-size ring buffer.
// This class is not thread safe

#include <cstddef>

#include <array>
#include <optional>

namespace gc {

template <typename T, std::size_t sz>
class RingBuffer {
    std::array<T, sz> m_buffer{};
    std::size_t m_head{0};
    std::size_t m_tail{0};

public:
    inline RingBuffer() {}

    inline ~RingBuffer() {}

    inline bool pushBack(T item)
    {
        bool result = false;
        const std::size_t next = (m_head + 1) % m_buffer.size();
        if (next != m_tail) {
            m_buffer[m_head] = item;
            m_head = next;
            result = true;
        }
        return result;
    }

    inline std::optional<T> popFront()
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

} // namespace gc
