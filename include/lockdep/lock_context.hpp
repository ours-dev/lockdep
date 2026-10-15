/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_LOCK_CONTEXT_HPP
#define LOCKDEP_LOCK_CONTEXT_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/lock_class_state.hpp>
#include <lockdep/report.hpp>
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

        explicit AcquiredLockEntry(LockClassId id) : m_id(id) {}

        [[nodiscard]]
        auto id() const -> LockClassId {
            return m_id;
        }

      private:
        USTL_NOCOPYABLE(AcquiredLockEntry);

        LockClassId m_id{INVALID_LOCK_CLASS_ID};

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

        [[nodiscard]]
        auto empty() const -> bool {
            return m_acquired_locks.empty();
        }

      private:
        USTL_NOCOPYABLE(LockContext);

        /// Records what this acquisition says about lock ordering, and reports
        /// the violations that are cheap to see from here.
        ///
        /// Runs with the lock not yet taken and the entry not yet in the list,
        /// so `m_acquired_locks` is exactly the set of locks already held.
        ///
        /// Deliberately O(held locks): it records edges and checks for a direct
        /// back edge, and leaves the full graph walk to
        /// `detect_lock_order_cycles()`.  A transitive cycle costs nothing here
        /// and is found by the next detector pass.
        auto validate_acquire(AcquiredLockEntry const &entry) -> void {
            auto *acquiring = ValidatorLockClassState::from_id(entry.id());
            if (acquiring == nullptr || acquiring->tracking_disabled()) {
                return;
            }
            acquiring->ensure_registered();

            for (auto const &held_entry: m_acquired_locks) {
                auto *held = ValidatorLockClassState::from_id(held_entry.id());
                if (held == nullptr || held->tracking_disabled()) {
                    continue;
                }

                if (held == acquiring) {
                    if (!acquiring->nestable()) {
                        report(Violation::ReAcquire, acquiring, acquiring);
                    }
                    continue;
                }

                // A leaf lock is the last one a context may take.
                if (held->has(ValidationOptions::Leaf)) {
                    report(Violation::LeafAcquire, acquiring, held);
                }

                // The reverse order has been seen before, so the two orders can
                // meet head on.  Worth reporting here rather than waiting for a
                // detector pass, because the acquiring context is still on the
                // stack that produced it.
                if (acquiring->has_dependency(held)) {
                    report(Violation::CircularDependency, acquiring, held);
                }

                held->ensure_registered();
                if (!held->add_dependency(acquiring)) {
                    report(Violation::DependencySetFull, acquiring, held);
                }
            }
        }

        static auto report(Violation violation, ValidatorLockClassState *acquiring,
                           ValidatorLockClassState *held) -> void {
            if (acquiring->has(ValidationOptions::ReportingDisabled) ||
                held->has(ValidationOptions::ReportingDisabled)) {
                return;
            }
            report_violation(ViolationReport{
                violation,
                acquiring->id(),
                held->id(),
                acquiring->has(ValidationOptions::ReAcquireFatal),
            });
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

        [[nodiscard]]
        auto id() const -> LockClassId {
            return INVALID_LOCK_CLASS_ID;
        }
    };

    class NullLockContext {
      public:
        auto acquire(NullAcquiredLockEntry &) -> void {}
        auto release(NullAcquiredLockEntry &) -> void {}
        [[nodiscard]]
        auto empty() const -> bool {
            return true;
        }
    };

} // namespace priv

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
