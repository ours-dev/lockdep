/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_GUARD_HPP
#define LOCKDEP_GUARD_HPP 1

#include <lockdep/state_saver.hpp>
#include <ours/compiler.hpp>
#include <ustl/util/noncopyable.hpp>

#include <tuple>
#include <utility>

namespace lockdep {
/// The RAII object to help us easier to use custom lock features.
///
/// A scoped capability, so that clang's thread-safety analysis sees the lock
/// taken here: without TA_SCOPED_CAPABILITY on this class every TA_REQUIRES
/// in the tree reads as unsatisfied and every TA_RELEASE as "releasing a lock
/// that was never held" -- the annotations were pure documentation until this
/// was added. Mirrors Zircon's lockdep::Guard, which is __TA_SCOPED_CAPABILITY
/// with __TA_ACQUIRE on the constructor and __TA_RELEASE on Release().
template <typename LockType, typename... Options>
class TA_SCOPED_CAPABILITY Guard {
    using Self = Guard;

  public:
    using Lock = LockType;

    /// |Options| are the SavedStateContainer tags from state_saver.hpp --
    /// IrqSave, PreemptDisable and so on. Each one names a state-saving RAII
    /// object through StateSaver<Lock, Option>, and this constructor's job is
    /// to have all of them alive before the lock is taken.
    ///
    /// The sequencing falls out of the language and is worth spelling out,
    /// because getting it backwards is a self-deadlock rather than a
    /// slowdown. Members are initialised before the constructor body runs, so
    /// every saver is constructed -- interrupts off, preemption held --
    /// *before* m_lock.acquire(). On the way out the destructor body runs
    /// first, releasing the lock, and only then are the members destroyed, so
    /// interrupts come back *after* the lock is dropped. SpinLock's own
    /// acquire_irq_save()/release_irq_restore() pair documents the same
    /// ordering constraint: restoring first leaves a window where an
    /// interrupt on this CPU can reach for the lock this CPU still holds.
    Guard(Lock &lock) TA_ACQUIRE(lock) : m_savers(), m_lock(lock), m_unlocked(false) {
        this->guard();
    }

    ~Guard() TA_RELEASE() {
        unlock();
    }

    TA_RELEASE() auto unlock() -> void {
        if (!m_unlocked) {
            release_lock();
            m_unlocked = true;
        }
    }

    /// Stop tracking the lock without releasing it: some other party -- the
    /// thread we are about to switch to, in Scheduler::reschedule_common --
    /// owns the release from here on. For the analysis that is exactly a
    /// release, which is why it carries the same annotation as unlock().
    TA_RELEASE() auto unguard() -> void {
        m_unlocked = true;
    }

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(Guard);

    /// The two helpers that actually touch the lock are hidden from the
    /// analysis. The Guard is the capability the caller sees; if the analysis
    /// also followed the underlying acquire()/release() it would count the
    /// same lock twice -- once as `*this`, once as `m_lock` -- and report the
    /// constructor as leaving `m_lock` held and unlock() as releasing a lock
    /// that was never acquired.
    TA_NO_THREAD_SAFETY_ANALYSIS auto guard() -> void {
        m_lock.acquire();
    }

    TA_NO_THREAD_SAFETY_ANALYSIS auto release_lock() -> void {
        m_lock.release();
    }

    /// One saver per option, in the order the options were written.
    ///
    /// std::tuple destroys its elements in reverse, which is the order that
    /// matters for `Guard<SpinLock, IrqSave, PreemptDisable>`: preemption is
    /// re-enabled first, so any reschedule that flush releases still runs
    /// with interrupts disabled -- the state Scheduler::reschedule_common
    /// asserts on entry -- and interrupts are restored last.
    ///
    /// This used to be `std::tuple<bool, Options...>`, storing the option
    /// *tags* themselves. The tags are empty structs and nothing ever read
    /// them, so the options were inert: every `Guard<SpinLock, IrqSave>` in
    /// the tree took the lock and left interrupts enabled. It surfaced as the
    /// debug kernel stopping in Scheduler::reschedule_common on
    /// `DEBUG_ASSERT(arch::irqs_disabled())`, reached from
    /// Thread::Current::preempt(), whose guard was supposed to have disabled
    /// them.
    std::tuple<StateSaver<LockType, Options>...> m_savers;

    Lock &m_lock;

    /// Whether the lock has already been handed off -- by unlock() or by
    /// unguard() -- so the destructor knows to leave it alone.
    bool m_unlocked;
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

/// Holds several locks for one scope.
///
/// Acquires them left to right and releases them right to left, so a
/// caller only has to state the order once and nesting stays consistent
/// with what a stack of single Guards would have done.
///
/// The bodies used to be declared and never defined, which made every use
/// an undefined symbol at link time.
///
/// Invisible to the thread-safety analysis: a capability attribute cannot
/// name a parameter pack, so there is no way to write TA_ACQUIRE(locks...).
/// The bodies are excluded from analysis so that the raw acquire()/release()
/// calls inside them do not trip it; callers who need the analysis to see
/// the locks should nest single Guards instead.
template <typename... LockTypes>
class ScopedGuard {
  public:
    TA_NO_THREAD_SAFETY_ANALYSIS explicit ScopedGuard(LockTypes &&...locks) : m_locks(locks...) {
        // The lambda is its own function to the analysis; the attribute on
        // the constructor does not reach into it.
        std::apply([](auto &...lock) TA_NO_THREAD_SAFETY_ANALYSIS { (lock.acquire(), ...); },
                   m_locks);
    }

    TA_NO_THREAD_SAFETY_ANALYSIS ~ScopedGuard() {
        release(std::index_sequence_for<LockTypes...>{});
    }

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(ScopedGuard);

    /// Reverse of the acquisition order: index i releases element N-1-i.
    template <std::size_t... Is>
    TA_NO_THREAD_SAFETY_ANALYSIS auto release(std::index_sequence<Is...>) -> void {
        constexpr auto COUNT = sizeof...(Is);
        (std::get<COUNT - 1 - Is>(m_locks).release(), ...);
    }

    std::tuple<LockTypes...> m_locks;
};

template <typename... LockType>
ScopedGuard(LockType &&...) -> ScopedGuard<LockType...>;

} // namespace lockdep

#endif // #ifndef LOCKDEP_GUARD_HPP