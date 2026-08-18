/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_GUARD_HPP
#define LOCKDEP_GUARD_HPP 1

#include <utility>

namespace lockdep {
    /// The RAII object to help us easier to use custom lock features.
    template <typename LockType, typename... Options>
    class Guard {
        using Self = Guard;
      public:
        using Lock = LockType;

        Guard(Lock &lock)
            : lock_(lock)
            , states_() { 
            this->guard();
        }

        ~Guard() { 
            unlock();
        }

        auto unlock() -> void {
            if (!std::get<0>(states_)) {
                lock_.release();
                std::get<0>(states_) = true;
            }
        }

        auto unguard() -> void {
            std::get<0>(states_) = true;
        }
      private:
        USTL_NO_MOVEABLE_AND_COPYABLE(Guard);

        auto guard() -> void {
            std::get<0>(states_) = false;
            lock_.acquire();
        }

        Lock &lock_;
        std::tuple<bool, Options...> states_; // unlocked, options...
    };

    template <>
    class Guard<void> {
      public:
        Guard(...) {}
        ~Guard() {}

        auto unlock() -> void {}
        auto unguard() -> void {}
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