#pragma once

#include "types.hpp"
#include "config.hpp"
#include "lock-traits.hpp"
#include "lock-class-state.hpp"

namespace lockdep {
    template <typename Class, typename Lock, usize Index, ValidationControl Control>
    class LockClass;

namespace priv {
    template <bool Enabled, typename Class, typename Lock, usize Index, ValidationControl Control>
    class LockClassStateStorage;

    template <typename Class, typename Lock, usize Index, ValidationControl Control>
    class LockClassStateStorage<true, Class, Lock, Index, Control> {
        using Self = LockClassStateStorage;
      public:
        static auto id() -> LockClassId;
      private:
        using State = IfLockValidationEnabled<ValidatorLockClassState, MetadataLockClassState> ;
        static inline State g_lock_class_state;
    };

    template <typename Class, typename Lock, usize Index, ValidationControl Control>
    class LockClassStateStorage<false, Class, Lock, Index, Control> {
      public:
        static auto id() -> LockClassId { return nullptr; }
    };

} // namespace lockdep::priv

    template <typename Class, typename Lock, usize Index, ValidationControl Control>
    class LockClass
        : public priv::LockClassStateStorage<kLockMetadataAvailable, Class, Lock, Index, Control> 
    {};

    template <typename Lock_>
    class Lock {
        using Self = Lock;
      public:
      private:
    };

} // namespace lockdep