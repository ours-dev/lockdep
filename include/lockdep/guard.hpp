/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_GUARD_HPP
#define LOCKDEP_GUARD_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/lock_class.hpp>
#include <lockdep/lock_context.hpp>
#include <lockdep/state_saver.hpp>

#include <tuple>
#include <utility>

#include <ours/compiler.hpp>
#include <ustl/util/noncopyable.hpp>

namespace lockdep {
namespace priv {
    /// Remembers which lock context a Guard registered with, so the release
    /// unregisters from that same context. Empty when validation is off.
    ///
    /// Access goes through get()/set() rather than the member directly: the
    /// holder's type does not depend on the guard's template parameters, so
    /// the discarded branch of an `if constexpr` inside the guard is still name
    /// checked and has to compile against the empty specialisation too.
    ///
    /// LockContext is spelled fully qualified on purpose. Inside namespace
    /// priv the bare name finds priv::LockContext, the validating class,
    /// before the lockdep::LockContext alias that picks between it and
    /// priv::NullLockContext by feature level. With validation off the two
    /// differ, and the discarded branch then fails to compile -- which clang
    /// 23 reports and older compilers silently let through.
    template <bool Enabled>
    struct ContextHolder {
        auto get() const -> ::lockdep::LockContext * {
            return m_context;
        }

        auto set(::lockdep::LockContext *context) -> void {
            m_context = context;
        }

        ::lockdep::LockContext *m_context{nullptr};
    };

    template <>
    struct ContextHolder<false> {
        auto get() const -> ::lockdep::LockContext * {
            return nullptr;
        }

        auto set(::lockdep::LockContext *) -> void {}
    };

} // namespace priv

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

    static_assert((IsSavedStateContainerV<Options> && ...),
                  "Guard options must be lockdep state tags (lockdep::IrqSave, "
                  "lockdep::PreemptDisable, lockdep::IrqSaveAndPreemptDisable); an "
                  "unrelated empty struct of the same name saves no state at all");

    /// The deduction guide turns `Guard guard{lock}` into Guard<Lock> with an
    /// empty saver tuple, which for a spin lock is a lock that disables
    /// nothing. That spelling looks exactly like a correct one, so it has to
    /// fail here rather than at run time, where it surfaced as a thread
    /// preempted inside PmZone's critical section and the next thread on the
    /// same CPU spinning on the lock it still held. Zircon's Guard carries the
    /// same assertion against AmbiguousOption.
    static_assert(sizeof...(Options) > 0 || !OptionRequired<LockType>,
                  "this lock must be guarded with an option: Guard<Lock, lockdep::IrqSave> "
                  "(or IrqSaveAndPreemptDisable where the scope may reschedule); without one "
                  "the Guard holds it with interrupts and preemption both enabled");

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
    Guard(Lock &lock) TA_ACQUIRE(lock)
        : m_savers(), m_lock(lock), m_entry(DefaultLockClass<Lock>::id()), m_unlocked(false) {
        this->guard();
    }

    /// Tag-object form, `Guard guard{lock, IRQ_SAVE}`. The deduction guide
    /// below has always turned that spelling into Guard<Lock, IrqSave>, but
    /// no constructor accepted the tag, so it never compiled. The tags carry
    /// no data; they only let the option be written at the call site.
    ///
    /// A constrained template rather than a plain `Guard(Lock &, Options...)`
    /// for two reasons. With no options that plain form is the constructor
    /// above a second time; and a trailing requires-clause, the obvious way to
    /// rule that out, cannot follow the GNU attribute TA_ACQUIRE expands to,
    /// while the attribute cannot move in front of the declarator because it
    /// names the parameter.
    template <typename... Tags>
        requires(sizeof...(Tags) > 0 && std::is_same_v<std::tuple<Tags...>, std::tuple<Options...>>)
    Guard(Lock &lock, Tags...) TA_ACQUIRE(lock) : Guard(lock) {}

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
    ///
    /// For lockdep it is a release too. The entry that records this lock as
    /// held lives in this guard, on this stack frame; leaving it on the
    /// context's list would leave the list pointing into a frame that is
    /// about to be abandoned.
    TA_RELEASE() auto unguard() -> void {
        if (!m_unlocked) {
            unregister();
            m_unlocked = true;
        }
    }

    auto held() const -> bool {
        return !m_unlocked;
    }

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(Guard);

    /// The two helpers that actually touch the lock are hidden from the
    /// analysis. The Guard is the capability the caller sees; if the analysis
    /// also followed the underlying acquire()/release() it would count the
    /// same lock twice -- once as `*this`, once as `m_lock` -- and report the
    /// constructor as leaving `m_lock` held and unlock() as releasing a lock
    /// that was never acquired.
    ///
    /// The acquisition is registered before the lock is taken, so that a
    /// report about an acquisition that is about to deadlock can name it.
    TA_NO_THREAD_SAFETY_ANALYSIS auto guard() -> void {
        if constexpr (LOCK_VALIDATION_ENABLED) {
            m_context.set(current_lock_context());
            if (auto *context = m_context.get()) {
                context->acquire(m_entry);
            }
        }
        m_lock.acquire();
    }

    /// Unregistered before the lock is released, in the order Zircon's
    /// Guard::Release documents: once the lock is dropped, anything that runs
    /// on this CPU may take it again, and must not find it still recorded as
    /// held here.
    TA_NO_THREAD_SAFETY_ANALYSIS auto release_lock() -> void {
        unregister();
        m_lock.release();
    }

    auto unregister() -> void {
        if constexpr (LOCK_VALIDATION_ENABLED) {
            if (auto *context = m_context.get()) {
                context->release(m_entry);
                m_context.set(nullptr);
            }
        }
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
    [[no_unique_address]]
    std::tuple<StateSaver<LockType, Options>...> m_savers;

    Lock &m_lock;

    /// This acquisition's entry on the lock context's held list, and the
    /// context it went onto. Both are empty types when validation is compiled
    /// out, so a guard then costs no more than it did before lockdep had
    /// anything to say.
    [[no_unique_address]]
    AcquiredLockEntry m_entry;
    [[no_unique_address]]
    priv::ContextHolder<LOCK_VALIDATION_ENABLED> m_context;

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
    auto held() const -> bool {
        return false;
    }
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
///
/// Invisible to lockdep's validation for the same practical reason, and on
/// its way out: docs/interface.md replaces it with nested Guards for locks of
/// different classes and GuardMultiple for locks of one class.
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
    template <usize... Is>
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
