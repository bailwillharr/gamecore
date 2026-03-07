/*
 * MaybeOwning<T>
 * a container that may or may not own the data at runtime
 *
 * If the object is in non-owning mode, then you cannot modify the data at all.
 * To do that, make a copy.
 * 
 * MaybeOwning is Owning by default
 */

#pragma once

#include <vector>
#include <variant>
#include <span>
#include <optional>

namespace gct {

template <typename T>
class MaybeOwning {
    using OwningContainer = std::vector<T>;
    using NonOwningContainer = std::span<const T>;
    std::variant<OwningContainer, NonOwningContainer> m_container{};

public:
    MaybeOwning() = default;

    MaybeOwning(std::vector<T> data) : m_container(std::move(data)) {}
    MaybeOwning(std::span<const T> data) : m_container(data) {}

    MaybeOwning(const MaybeOwning& other)
    {
        if (std::holds_alternative<NonOwningContainer>(other.m_container)) {
            const auto& other_container = std::get<NonOwningContainer>(other.m_container);
            m_container.emplace<OwningContainer>(other_container.begin(), other_container.end());
        }
        else {
            m_container = other.m_container;
        }
    }

    MaybeOwning(MaybeOwning&&) = default;

    // always copies data and makes owning (safe default behaviour)
    MaybeOwning& operator=(const MaybeOwning& other)
    {
        if (this != &other) {
            if (std::holds_alternative<NonOwningContainer>(other.m_container)) {
                const auto& other_container = std::get<NonOwningContainer>(other.m_container);
                m_container.emplace<OwningContainer>(other_container.begin(), other_container.end());
            }
            else {
                m_container = other.m_container;
            }
        }
        return *this;
    }

    MaybeOwning& operator=(MaybeOwning&&) = default;

    std::span<const T> get() const
    {
        return std::visit([](const auto& data) -> std::span<const T> { return data; }, m_container);
    }

    std::vector<T>* getMutable()
    {
        if (std::holds_alternative<OwningContainer>(m_container)) {
            return &std::get<OwningContainer>(m_container);
        }
        else {
            return nullptr;
        }
    }
};

} // namespace gct