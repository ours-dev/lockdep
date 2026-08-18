/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_LOCK_CONTEXT_HPP
#define LOCKDEP_LOCK_CONTEXT_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/types.hpp>

#include <ustl/collections/intrusive/slist.hpp>
#include <ustl/util/noncopyable.hpp>

namespace lockdep {
namespace priv {
    /// One entry per lock currently held by an execution context.  The entry
    /// lives in the `Guard` on the stack, so the list never allocates.
    class AcquiredLockEntry {
        using Self = AcquiredLockEntry;

      public:
        AcquiredLockEntry() = default;

        explicit AcquiredLockEntry(LockClassId id)
            : m_id(id) {}

        auto id() const -> LockClassId {
            return m_id;
        }

      private:
        USTL_NOCOPYABLE(AcquiredLockEntry);

        LockClassId m_id{kInvalidLockClassId};

        ustl::collections::intrusive::SlistMemberHook<> m_hook;
        USTL_DECLARE_HOOK_OPTION(Self, m_hook, HookOption);

      public:
        USTL_DECLARE_SLIST(Self, List, HookOption);
    };

    /// The set of locks held by one execution context, innermost first.
    ///
    /// Every kernel `Thread` owns one.  It is only ever touched by its own
    /// context, so it needs no locking of its own.
    class LockContext {
        using Self = LockContext;

      public:
        LockContext() = default;

        /// Records `entry` as held.  Must be called before the lock is taken so
        /// that a report names the acquisition that is about to deadlock.
        auto acquire(AcquiredLockEntry &entry) -> void {
            validate_acquire(entry);
            m_acquired_locks.push_front(entry);
        }

        /// Drops `entry` again.  Releases are not required to nest, so this
        /// erases by identity rather than popping.
        auto release(AcquiredLockEntry &entry) -> void {
            static_cast<void>(m_acquired_locks.erase(m_acquired_locks.iterator_to(entry)));
        }

        auto empty() const -> bool {
            return m_acquired_locks.empty();
        }

      private:
        USTL_NOCOPYABLE(LockContext);

        /// TODO: walk the dependency graph reachable from `entry.id()` against
        /// the classes already in `m_acquired_locks` and report a cycle.  The
        /// graph and its detector live in `ValidatorLockClassState`.
        auto validate_acquire(AcquiredLockEntry const &entry) -> void {
            (void)entry;
        }

        AcquiredLockEntry::List m_acquired_locks;
    };

    /// Empty stand-ins used when validation is compiled out, so that a `Guard`
    /// on the stack and a member like `Thread::m_lock_context` both cost
    /// nothing.
    class NullAcquiredLockEntry {
      public:
        NullAcquiredLockEntry() = default;

        explicit NullAcquiredLockEntry(LockClassId) {}

        auto id() const -> LockClassId {
            return kInvalidLockClassId;
        }
    };

    class NullLockContext {
      public:
        auto acquire(NullAcquiredLockEntry &) -> void {}
        auto release(NullAcquiredLockEntry &) -> void {}
        auto empty() const -> bool { return true; }
    };

} // namespace lockdep::priv

    using AcquiredLockEntry =
        IfLockValidationEnabled<priv::AcquiredLockEntry, priv::NullAcquiredLockEntry>;

    using LockContext = IfLockValidationEnabled<priv::LockContext, priv::NullLockContext>;

    /// Returns the lock context of the currently running execution context, or
    /// `nullptr` when there is none yet (early boot, before the scheduler).
    ///
    /// lockdep cannot know how the environment tracks the current context, so
    /// the definition is supplied by the kernel.  It is only referenced when
    /// validation is enabled, which is why a build with `lockdep_enable=false`
    /// links without providing it.
    auto current_lock_context() -> LockContext *;

} // namespace lockdep

#endif // #ifndef LOCKDEP_LOCK_CONTEXT_HPP
