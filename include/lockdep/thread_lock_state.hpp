/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_THREAD_LOCK_STATE_HPP
#define LOCKDEP_THREAD_LOCK_STATE_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/lock_class_state.hpp>
#include <lockdep/runtime_api.hpp>
#include <lockdep/types.hpp>

#include <ustl/collections/intrusive/list.hpp>
#include <ustl/util/noncopyable.hpp>

namespace lockdep {
/// The record of one lock held by one context.
///
/// It lives in the Guard that holds the lock, on that context's stack, so the
/// held-lock list never allocates. Zircon's AcquiredLockEntry
/// (lockdep/thread_lock_state.h).
class AcquiredLockEntry {
    using Self = AcquiredLockEntry;

  public:
    AcquiredLockEntry() = default;

    /// |address| identifies the lock instance, so that taking the very same
    /// lock twice can be told from taking two locks of one class. |order| is
    /// the rank among locks of one class held together -- zero unless the
    /// class is nested by an explicit order.
    explicit AcquiredLockEntry(void const *address, LockClassId const id, usize const order = 0)
        : m_address(address), m_id(id), m_order(order) {}

    [[nodiscard]]
    auto address() const -> void const * {
        return m_address;
    }

    [[nodiscard]]
    auto id() const -> LockClassId {
        return m_id;
    }

    [[nodiscard]]
    auto order() const -> usize {
        return m_order;
    }

    /// Whether the entry is on a held-lock list.
    [[nodiscard]]
    auto is_linked() const -> bool {
        return m_hook.is_linked();
    }

  private:
    USTL_NOCOPYABLE(AcquiredLockEntry);

    void const *m_address{nullptr};
    LockClassId m_id{INVALID_LOCK_CLASS_ID};
    usize m_order{0};

    /// Doubly linked, as Zircon's fbl::DoublyLinkedList is: a lock released
    /// out of order is unlinked in O(1), and an entry that moves to another
    /// frame can take its predecessor's place in O(1). The hook's default
    /// SafeLink mode also traps an entry destroyed while still on a list --
    /// a Guard's frame abandoned with the lock recorded as held -- which is
    /// Zircon's `ZX_DEBUG_ASSERT(!InContainer())` in ~AcquiredLockEntry.
    ustl::collections::intrusive::ListMemberHook<> m_hook;
    USTL_DECLARE_HOOK_OPTION(Self, m_hook, HookOption);

  public:
    USTL_DECLARE_LIST(Self, List, HookOption);
};

/// The locks held by one context, and the validation of each acquisition
/// against them. Zircon's ThreadLockState.
///
/// "Context" is the environment's to define: system_get_thread_lock_state()
/// hands out the running thread's state for most locks and the CPU's for
/// irq-safe ones. Either way the state is only ever changed by the context it
/// belongs to, so it needs no lock of its own. The one reach across is a read:
/// taking an irq-unsafe lock looks at the CPU's state for irq-safe locks held,
/// see check_irq_safety_across_states().
class ThreadLockState {
    using Self = ThreadLockState;

  public:
    ThreadLockState() = default;

    /// Validates the acquisition that |entry| records against every lock this
    /// context holds, records the new ordering edges, and adds |entry| to the
    /// held list. Called before the lock is taken, so that a report can name
    /// the acquisition that is about to deadlock.
    ///
    /// The checks and their order are Zircon's ThreadLockState::Acquire(), for
    /// each held entry in turn:
    ///
    ///   1. the held lock is a leaf -- nothing may follow it;
    ///   2. the held lock is of the same class -- the very same lock is
    ///      re-entrance; otherwise, unless the class is MultiAcquire, the new
    ///      order has to rise above the held one;
    ///   3. otherwise the pair is an ordering edge, held -> new. Only when the
    ///      edge is new is anything checked or triggered: an edge already in
    ///      the graph was checked when it went in, so an ordering that recurs
    ///      is reported once rather than at every acquisition.
    ///
    /// and, where the environment keeps irq-safe locks on a state of their
    /// own, one check Zircon leaves out: an irq-unsafe lock is also checked
    /// against the irq-safe locks held there.
    ///
    /// All of it is O(locks held), and the one check that needs the whole
    /// graph -- a cycle through classes this context does not hold -- is left
    /// to loop_detection_pass(), which the new edge asks for.
    auto acquire(AcquiredLockEntry &entry) -> void {
        if constexpr (!LOCK_VALIDATION_ENABLED) {
            return;
        }
        auto *const acquiring = LockClassState::from_id(entry.id());
        if (acquiring == nullptr || acquiring->tracking_disabled()) {
            return;
        }
        if (acquiring->has(ValidationOptions::ReportingDisabled)) {
            ++m_reporting_disabled_count;
        }
        // A reporter that takes locks acquires them while reporting is
        // suspended; leave the result it is reporting alone.
        if (!reporting_disabled()) {
            m_last_result = LockResult::Success;
        }
        acquiring->ensure_registered();

        for (auto &held_entry: m_acquired_locks) {
            auto *const held = LockClassState::from_id(held_entry.id());
            if (held->has(ValidationOptions::Leaf)) {
                report(entry, &held_entry, LockResult::AcquireAfterLeaf);
            } else if (held == acquiring) {
                check_same_class(entry, held_entry, *acquiring);
            } else {
                record_edge(entry, held_entry, *held, *acquiring);
            }
        }
        if (!acquiring->has(ValidationOptions::IrqSafe)) {
            check_irq_safety_across_states(entry);
        }

        // ActiveListDisabled: validated, but not kept -- for the locks that
        // guard context switching, whose release happens on another stack.
        if (!acquiring->has(ValidationOptions::ActiveListDisabled)) {
            m_acquired_locks.push_back(entry);
        }
    }

    /// Removes |entry| from the held list. Releases need not nest.
    auto release(AcquiredLockEntry &entry) -> void {
        if constexpr (!LOCK_VALIDATION_ENABLED) {
            return;
        }
        auto *const releasing = LockClassState::from_id(entry.id());
        if (releasing == nullptr || releasing->tracking_disabled()) {
            return;
        }
        if (releasing->has(ValidationOptions::ReportingDisabled)) {
            --m_reporting_disabled_count;
        }
        if (entry.is_linked()) {
            m_acquired_locks.erase(entry);
        }
    }

    /// Fatal if this context holds any tracked lock -- for code that must run
    /// with none held, such as a context switch out of a thread. Reports the
    /// most recent acquisition, as Zircon does.
    auto assert_no_locks_held() -> void {
        if constexpr (!LOCK_VALIDATION_ENABLED) {
            return;
        }
        if (auto held = m_acquired_locks.back()) {
            system_lock_validation_fatal(*held, *this, LockResult::ShouldNotHold);
        }
    }

    [[nodiscard]]
    auto empty() const -> bool {
        return m_acquired_locks.empty();
    }

    /// The first violation the last acquisition ran into, for tests.
    [[nodiscard]]
    auto last_result() const -> LockResult {
        return m_last_result;
    }

    /// Whether reports are suspended: a ReportingDisabled lock is held, or a
    /// report is being delivered right now.
    [[nodiscard]]
    auto reporting_disabled() const -> bool {
        return m_reporting_disabled_count > 0;
    }

  private:
    USTL_NOCOPYABLE(ThreadLockState);

    /// A second lock of the class of one already held.
    ///
    /// The very same lock is re-entrance, always an error. Otherwise it is
    /// legitimate only for a MultiAcquire class, or when the new lock's order
    /// rises above the held one's: a Nestable class is nested by an order the
    /// caller supplies, and a group of one class taken together is ordered by
    /// address. Order zero on a class that is not Nestable is the plain
    /// mistake of taking a second lock of it.
    auto check_same_class(AcquiredLockEntry const &entry, AcquiredLockEntry const &held_entry,
                          LockClassState const &cls) -> void {
        if (entry.address() == held_entry.address()) {
            report(entry, &held_entry, LockResult::Reentrance);
            return;
        }
        if (cls.has(ValidationOptions::MultiAcquire) || entry.order() > held_entry.order()) {
            return;
        }
        if (!cls.has(ValidationOptions::Nestable) && entry.order() == 0) {
            report(entry, &held_entry, LockResult::AlreadyAcquired);
        } else {
            report(entry, &held_entry, LockResult::InvalidNesting);
        }
    }

    /// Records held -> acquiring, and checks it if it is new.
    ///
    /// Two checks, both about the pair alone. An irq-safe lock held while an
    /// irq-unsafe one is taken is a hazard in its own right. And if the
    /// reverse edge is already in the graph, two contexts running the two
    /// orders can deadlock head on -- worth reporting here, with the offending
    /// context still on the stack, rather than waiting for a detector pass.
    auto record_edge(AcquiredLockEntry const &entry, AcquiredLockEntry const &held_entry,
                     LockClassState &held, LockClassState &acquiring) -> void {
        held.ensure_registered();
        switch (held.add_dependency(&acquiring)) {
            case LockClassState::AddResult::Added:
                trigger_loop_detection();
                if (held.has(ValidationOptions::IrqSafe) &&
                    !acquiring.has(ValidationOptions::IrqSafe)) {
                    report(entry, &held_entry, LockResult::InvalidIrqSafety);
                }
                if (acquiring.has_dependency(&held)) {
                    report(entry, &held_entry, LockResult::OutOfOrder);
                }
                return;
            case LockClassState::AddResult::Full:
                report(entry, &held_entry, LockResult::MaxLockDependencies);
                return;
            case LockClassState::AddResult::Present:
                return;
        }
    }

    /// An irq-unsafe lock taken while irq-safe locks are held: the rule
    /// record_edge() applies to a pair on one list, applied to the pair that
    /// a split environment never lets meet on one.
    ///
    /// The kernel keeps irq-safe locks on the CPU's state and the rest on the
    /// thread's, as Zircon's SystemGetThreadLockState() does, so that an
    /// interrupt handler never changes the list of the thread it interrupted.
    /// That split is right for recording and wrong for this question: the
    /// held irq-safe locks sit on the other list, where the loop in acquire()
    /// never looks, and the check it runs could not fire. Zircon has the same
    /// gap. So the CPU's state is asked directly.
    ///
    /// Only the rule is checked; no edge is recorded across the states, and
    /// none needs to be. A cycle through classes of both kinds has to step
    /// somewhere from an irq-safe class to an irq-unsafe one, and that step is
    /// reported here as it is taken; a cycle through one kind stays on one
    /// state, whose edges are recorded and searched by loop_detection_pass().
    ///
    /// A state that is delivering a report, or holds a ReportingDisabled lock,
    /// is not reported against, since its holder may be the reporter itself.
    /// The read is only sound while the running thread cannot move to another
    /// CPU: validating an irq-unsafe lock has to happen with interrupts off,
    /// which is what the policy's ValidationGuard is for -- Zircon's Mutex and
    /// BrwLock policies use LockValidationGuard to the same end.
    auto check_irq_safety_across_states(AcquiredLockEntry const &entry) -> void {
        // Not a pointer to const, though nothing is changed through it: ustl's
        // List::back() const does not compile, returning Option<T &> where it
        // promises Option<T const &>.
        auto *const irq_state = system_get_thread_lock_state(ValidationOptions::IrqSafe);
        if (irq_state == nullptr || irq_state == this || irq_state->reporting_disabled()) {
            return;
        }
        if (auto const held = irq_state->m_acquired_locks.back()) {
            report(entry, &*held, LockResult::InvalidIrqSafety);
        }
    }

    /// Delivers one violation, unless reports are suspended.
    ///
    /// Reporting is suspended for the duration of the report itself: a
    /// reporter that takes a lock -- to print, say -- would otherwise be
    /// validated in the middle of a report, and report again. Zircon's
    /// ThreadLockState::Report().
    auto report(AcquiredLockEntry const &entry, AcquiredLockEntry const *conflicting,
                LockResult const result) -> void {
        if (reporting_disabled()) {
            return;
        }
        ++m_reporting_disabled_count;

        auto const *const cls = LockClassState::from_id(entry.id());
        auto const repeat =
            result == LockResult::AlreadyAcquired || result == LockResult::InvalidNesting;
        if (repeat && cls->has(ValidationOptions::ReAcquireFatal)) {
            system_lock_validation_fatal(entry, *this, LockResult::AlreadyAcquired);
        }
        if (result == LockResult::AcquireAfterLeaf) {
            system_lock_validation_fatal(entry, *this, LockResult::AcquireAfterLeaf);
        }
        system_lock_validation_error(entry, conflicting, *this, result);

        --m_reporting_disabled_count;
        if (m_last_result == LockResult::Success) {
            m_last_result = result;
        }
    }

    /// Asks the environment for a loop-detection pass, unless reports are
    /// suspended -- the request could itself take a lock.
    auto trigger_loop_detection() -> void {
        if (reporting_disabled()) {
            return;
        }
        ++m_reporting_disabled_count;
        system_trigger_loop_detection();
        --m_reporting_disabled_count;
    }

    AcquiredLockEntry::List m_acquired_locks;

    /// Held ReportingDisabled locks, plus one while a report or a trigger is
    /// in flight.
    u16 m_reporting_disabled_count{0};

    LockResult m_last_result{LockResult::Success};
};

namespace priv {
    /// Empty stand-ins, so that a Guard on the stack and a member such as
    /// Thread's lock state cost nothing where validation is compiled out.
    class NullAcquiredLockEntry {
      public:
        NullAcquiredLockEntry() = default;

        explicit NullAcquiredLockEntry(void const *, LockClassId, usize = 0) {}

        [[nodiscard]]
        auto id() const -> LockClassId {
            return INVALID_LOCK_CLASS_ID;
        }
    };

    class NullThreadLockState {
      public:
        auto acquire(NullAcquiredLockEntry &) -> void {}
        auto release(NullAcquiredLockEntry &) -> void {}
        auto assert_no_locks_held() -> void {}

        [[nodiscard]]
        auto empty() const -> bool {
            return true;
        }
    };

} // namespace priv

/// Fatal if the running context holds any tracked lock. Zircon's
/// lockdep::AssertNoLocksHeld().
inline auto assert_no_locks_held() -> void {
    if constexpr (LOCK_VALIDATION_ENABLED) {
        if (auto *const state = system_get_thread_lock_state(ValidationOptions::None)) {
            state->assert_no_locks_held();
        }
    }
}

} // namespace lockdep

#endif // #ifndef LOCKDEP_THREAD_LOCK_STATE_HPP
