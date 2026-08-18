/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_LOCK_CLASS_HPP
#define LOCKDEP_LOCK_CLASS_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/lock-class-state.hpp>
#include <lockdep/lock-traits.hpp>
#include <lockdep/types.hpp>

namespace lockdep {
namespace priv {
    template <bool Enabled, typename Class, typename Lock, Size Index, ValidationControl Control>
    class LockClassStateStorage;

    template <typename Class, typename Lock, Size Index, ValidationControl Control>
    class LockClassStateStorage<true, Class, Lock, Index, Control> {
        using State = IfLockValidationEnabled<ValidatorLockClassState, MetadataLockClassState>;

      public:
        static auto id() -> LockClassId {
            return s_lock_class_state.id();
        }

        static auto state() -> State const & {
            return s_lock_class_state;
        }

      private:
        /// One object per (Class, Lock, Index, Control) instantiation.  Its
        /// address is the class id, which is why it is `inline` rather than
        /// defined out of line: every translation unit must agree on it.
        static inline State s_lock_class_state{Control};
    };

    template <typename Class, typename Lock, Size Index, ValidationControl Control>
    class LockClassStateStorage<false, Class, Lock, Index, Control> {
      public:
        static auto id() -> LockClassId {
            return kInvalidLockClassId;
        }
    };

} // namespace lockdep::priv

    /// Names a lock class.  `Class` is the type that owns the lock, `Index`
    /// separates several locks of the same type within one owner.
    template <typename Class, typename Lock, Size Index = 0,
              ValidationControl Control = kLockControlOf<Lock>>
    class LockClass
        : public priv::LockClassStateStorage<kLockMetadataAvailable, Class, Lock, Index, Control> {};

    /// The class a bare lock falls back to when the acquisition site names no
    /// owner.  Every lock of the same type shares it.
    ///
    /// TODO: this is deliberately coarse.  Once locks are declared through an
    /// instrumenting wrapper, each declaration site gets its own class and the
    /// validator can tell two spinlocks apart.  Keeping the lookup behind this
    /// one alias is what makes that change local.
    template <typename Lock>
    using DefaultLockClass = LockClass<void, Lock, 0, kLockControlOf<Lock>>;

} // namespace lockdep

#endif // #ifndef LOCKDEP_LOCK_CLASS_HPP
