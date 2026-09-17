/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_LOCK_CLASS_HPP
#define LOCKDEP_LOCK_CLASS_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/lock_class_state.hpp>
#include <lockdep/lock_traits.hpp>
#include <lockdep/types.hpp>

namespace lockdep {
namespace priv {
    template <bool Enabled, typename Class, typename Lock, usize Index, ValidationControl Control>
    class LockClassStateStorage;

    template <typename Class, typename Lock, usize Index, ValidationControl Control>
    class LockClassStateStorage<true, Class, Lock, Index, Control> {
        using State = IfLockValidationEnabled<ValidatorLockClassState, MetadataLockClassState>;

      public:
        /// Used to be declared and never defined, so the first caller would
        /// have failed to link.
        static auto id() -> LockClassId {
            return m_lock_class_state.id();
        }

        static auto state() -> State const & {
            return m_lock_class_state;
        }

      private:
        /// One object per (Class, Lock, Index, Control) instantiation. Its
        /// address is the class id, which is why it is an inline variable
        /// rather than defined out of line: every translation unit must agree
        /// on it. Zircon's LockClassStateStorage::lock_class_state_ in
        /// lockdep/lock_class.h plays the same dual role.
        static inline State m_lock_class_state{Control};
    };

    template <typename Class, typename Lock, usize Index, ValidationControl Control>
    class LockClassStateStorage<false, Class, Lock, Index, Control> {
      public:
        static auto id() -> LockClassId {
            return INVALID_LOCK_CLASS_ID;
        }
    };

} // namespace priv

/// Names a lock class. |Class| is the type that owns the lock, |Index|
/// separates several locks of the same type within one owner.
template <typename Class, typename Lock, usize Index = 0,
          ValidationControl Control = LockControlV<Lock>>
class LockClass
    : public priv::LockClassStateStorage<LOCK_METADATA_AVAILABLE, Class, Lock, Index, Control> {};

/// The class a bare lock falls back to when the acquisition site names no
/// owner. Every lock of the same type shares it.
///
/// Deliberately coarse, and the reason docs/interface.md replaces it: until
/// each lock declaration names its own class, two different spinlocks are one
/// class to the validator and ordinary nesting reads as re-acquisition.
/// Keeping the lookup behind this one alias is what keeps that change local.
template <typename Lock>
using DefaultLockClass = LockClass<void, Lock, 0, LockControlV<Lock>>;

/// Scaffolding for the instrumented lock wrapper that docs/interface.md
/// describes; intentionally empty until that lands.
template <typename Lock_>
class Lock {
    using Self = Lock;

  public:
  private:
};

} // namespace lockdep

#endif // #ifndef LOCKDEP_LOCK_CLASS_HPP
