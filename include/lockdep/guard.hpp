/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_GUARD_HPP
#define LOCKDEP_GUARD_HPP 1

#include <tuple>

#include <bits/bitmask.hpp>

namespace lockdep {
    /// The RAII object to help us easier to use custom lock features.
    template <typename LockType, typename... Options>
    class Guard {
        using Self = Guard;
      public:
        using Lock = LockType;

        Guard(Lock &lock)
            : m_lock(lock)
            , m_states() { 
            this->guard();
        }

        ~Guard() { 
            unlock();
        }

        auto unlock() -> void {
            if (!std::get<0>(m_states)) {
                m_lock.release();
                std::get<0>(m_states) = true;
            }
        }

        auto unguard() -> void {
            std::get<0>(m_states) = true;
        }
      private:
        Guard(Self &&) = delete;
        Guard(Self const &) = delete;
        auto operator=(Self &&) -> Self & = delete;
        auto operator=(Self const &) -> Self & = delete;

        auto guard() -> void {
            std::get<0>(m_states) = false;
            m_lock.acquire();
        }

        Lock &m_lock;
        std::tuple<bool, Options...> m_states; // unlocked, options...
    };

    template <>
    class Guard<void> {
        using Self = Guard;
      public:
        Guard(...) {}

        auto unlock() -> void {}
        auto unguard() -> void {}
      private:
        Guard(Self &&) = delete;
        Guard(Self const &) = delete;
        auto operator=(Self &&) -> Self & = delete;
        auto operator=(Self const &) -> Self & = delete;
    };

    template <typename LockType, typename... Option>
    Guard(LockType &, Option...) -> Guard<LockType, Option...>;

    template <typename... LockTypes>
    class ScopedGuard {
      public:
        ScopedGuard(LockTypes &&...);
        ~ScopedGuard();
      private:
    };

    template <typename... LockType>
    ScopedGuard(LockType &&...) -> ScopedGuard<LockType...>;

} // namespace lockdep

#endif // #ifndef LOCKDEP_GUARD_HPP