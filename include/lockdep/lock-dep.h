#pragma once

#include <lockdep/lock-traits.hpp>
#include <lockdep/lock-class.hpp>

namespace lockdep {
namespace priv {
    template <LockFlags Flags>
    struct AdditionalLockState;

    template <>
    struct AdditionalLockState<LockFlags::Nestable> {
        AdditionalLockState(usize order) : order(order) {}
        usize order;
    };

    template <bool Enabled, typename Class, typename Lock, usize Index, LockFlags Flags>
    class LockClassStateStorage;

    template <typename Class, typename Lock, usize Index, LockFlags Flags>
    class LockClassStateStorage<true, Class, Lock, Index, Flags> {
        using Self = LockClassStateStorage;
      public:
        static auto id() -> LockClassId { &LOCK_CLASS; }
      private:
        inline static LockClass LOCK_CLASS{Flags};
    };

    template <typename Class, typename Lock, usize Index, LockFlags Flags>
    class LockClassStateStorage<false, Class, Lock, Index, Flags> {
        using Self = LockClassStateStorage;
      public:
        static auto id() -> LockClassId { return nullptr; }
    };
} // namespace priv 

    template <typename Class,typename Lock, typename Tag, LockFlags Flags>
    class LockDep {};
} // namespace lockdep