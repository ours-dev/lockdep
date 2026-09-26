/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_GUARD_HPP
#define LOCKDEP_GUARD_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/lock_class.hpp>
#include <lockdep/lock_context.hpp>
#include <lockdep/lock_policy.hpp>

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
        [[nodiscard]]
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
        [[nodiscard]]
        auto get() const -> ::lockdep::LockContext * {
            return nullptr;
        }

        auto set(::lockdep::LockContext *) -> void {}
    };

    /// The policy a Guard holds its lock with, or DefaultLockPolicy standing in
    /// once the Guard's own assertion has already reported that there is none
    /// -- so a missing policy is reported once, rather than again by every
    /// member that names it.
    template <typename Lock, typename Option>
    struct PolicyOrStandIn {
        using Type = DefaultLockPolicy;
    };

    template <typename Lock, typename Option>
        requires HasPolicy<Lock, Option>
    struct PolicyOrStandIn<Lock, Option> {
        using Type = PolicyFor<Lock, Option>;
    };

} // namespace priv

/// Holds one lock for one scope, the way the lock's policy says to hold it.
///
/// |Option| selects the policy: `Guard<SpinLock, IrqSave>` holds the lock
/// with interrupts saved and disabled, `Guard<Mutex>` with nothing around it.
/// See lock_policy.hpp for how the pair is resolved, and for the order in
/// which the policy and the validator take turns.
///
/// A scoped capability, so that clang's thread-safety analysis sees the lock
/// taken here: without TA_SCOPED_CAPABILITY on this class every TA_REQUIRES
/// in the tree reads as unsatisfied and every TA_RELEASE as "releasing a lock
/// that was never held" -- the annotations were pure documentation until this
/// was added. Mirrors Zircon's lockdep::Guard, which is __TA_SCOPED_CAPABILITY
/// with __TA_ACQUIRE on the constructor and __TA_RELEASE on Release().
template <typename LockType, typename Option = NoOption>
class TA_SCOPED_CAPABILITY Guard {
    using Self = Guard;

    /// `Guard guard{lock}` deduces Guard<Lock>, which for a spin lock is a lock
    /// held with interrupts and preemption both enabled -- and the spelling
    /// looks exactly like a correct one. It has to fail here rather than at run
    /// time, where it surfaced as a thread preempted inside PmZone's critical
    /// section and the next thread on the same CPU spinning on the lock it
    /// still held. Zircon's Guard carries the same assertion against
    /// AmbiguousOption. Checked before the general one below, so that this
    /// case gets the message that says what to write instead.
    static_assert(!std::is_same_v<Option, NoOption> || !OptionRequired<LockType>,
                  "this lock must be guarded with an option: Guard<Lock, lockdep::IrqSave> "
                  "(or IrqSaveAndPreemptDisable where the scope may reschedule); without one "
                  "the Guard holds it with interrupts and preemption both enabled");

    /// An option the lock has no policy for. This is also how a look-alike tag
    /// is caught -- an empty struct named IrqSave declared outside lockdep,
    /// which is what ours::IrqSave used to be -- since no lock declares a
    /// policy for it.
    static_assert(HasPolicy<LockType, Option> ||
                      (std::is_same_v<Option, NoOption> && OptionRequired<LockType>),
                  "no lock policy for this lock and option: the lock type must declare "
                  "lockdep_policy(Lock *, Option *) -- or a state saver -- for it");

    using Policy = typename priv::PolicyOrStandIn<LockType, Option>::Type;

    static_assert(LockPolicy<Policy, LockType>,
                  "a lock policy must provide State, ValidationGuard, pre_validate(), "
                  "acquire() returning bool, and release()");

  public:
    /// Takes a lock declared as lockdep::Lock<LockType>, recording it under
    /// the class its declaration named.
    ///
    /// The capability acquired is the Lock itself, which is what members are
    /// guarded by: TA_GUARDED(m_pi_lock) with m_pi_lock the Lock.
    explicit Guard(::lockdep::Lock<LockType> &lock) TA_ACQUIRE(lock)
        : m_lock(lock.lock()), m_entry(lock.id()) {
        this->guard();
    }

    /// Takes a bare lock, which has no class of its own: it is recorded under
    /// DefaultLockClass<LockType>, shared by every lock of its type.
    ///
    /// Transitional. It is what every Guard in the kernel still uses, and it
    /// is also why nesting two different spin locks reads to the validator as
    /// taking one lock twice; it goes once the kernel's locks are declared as
    /// lockdep::Lock.
    Guard(LockType &lock) TA_ACQUIRE(lock)
        : m_lock(lock), m_entry(DefaultLockClass<LockType>::id()) {
        this->guard();
    }

    /// Tag-object form, `Guard guard{lock, IRQ_SAVE}`, which the deduction
    /// guide below turns into Guard<Lock, IrqSave>. The tag carries no data;
    /// it only lets the option be written at the call site.
    ///
    /// A constrained template rather than a plain `Guard(Lock &, Option)`,
    /// because with no option that plain form would take a NoOption the
    /// caller never wrote; and a trailing requires-clause, the obvious way to
    /// rule that out, cannot follow the GNU attribute TA_ACQUIRE expands to,
    /// while the attribute cannot move in front of the declarator because it
    /// names the parameter.
    template <typename Tag>
        requires(!std::is_same_v<Option, NoOption> && std::is_same_v<Tag, Option>)
    Guard(::lockdep::Lock<LockType> &lock, Tag) TA_ACQUIRE(lock) : Guard(lock) {}

    template <typename Tag>
        requires(!std::is_same_v<Option, NoOption> && std::is_same_v<Tag, Option>)
    Guard(LockType &lock, Tag) TA_ACQUIRE(lock) : Guard(lock) {}

    ~Guard() TA_RELEASE() {
        unlock();
    }

    /// Releases the lock before the end of the scope.
    ///
    /// The policy restores what it saved at this point, not when the guard is
    /// destroyed: after `guard.unlock()` the CPU is back in the state it was
    /// in before the guard, as with Zircon's Guard::Release(). Keeping
    /// interrupts disabled until the end of the scope instead would leave a
    /// stretch of code that holds no lock yet still runs with interrupts off,
    /// which nothing in its text says.
    TA_RELEASE() auto unlock() -> void {
        if (m_held) {
            release_lock();
            m_held = false;
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
    ///
    /// The policy is not asked to release, so what it saved stays saved until
    /// the guard itself is destroyed -- the new owner releases the lock, and
    /// the state belongs to the frame that saved it.
    TA_RELEASE() auto unguard() -> void {
        if (m_held) {
            unregister();
            m_held = false;
        }
    }

    /// Whether the guard holds its lock: false after unlock() or unguard(),
    /// and after a try-lock policy failed to acquire.
    [[nodiscard]]
    auto held() const -> bool {
        return m_held;
    }

    explicit operator bool() const {
        return m_held;
    }

    /// Releases the lock around |op| and takes it back afterwards, so that one
    /// guard can cover a larger scope with an unlocked stretch in the middle
    /// -- typically a loop that has to drop the lock to do something that may
    /// block. Zircon's Guard::CallUnlocked().
    ///
    /// The release goes through the policy, so |op| runs with the state the
    /// guard saved restored -- interrupts back on, for IrqSave -- and the
    /// retake repeats the whole acquisition, validation included. A guard
    /// that does not hold its lock runs |op| and leaves the lock alone.
    template <typename Op>
    TA_NO_THREAD_SAFETY_ANALYSIS auto call_unlocked(Op &&op) -> void {
        if (!m_held) {
            std::forward<Op>(op)();
            return;
        }
        release_lock();
        m_held = false;
        std::forward<Op>(op)();
        guard();
    }

    /// Withdraws the record of the acquisition around |op| without releasing
    /// the lock. For the narrow case where validation is more pessimistic than
    /// the code: |op| asserts that no locks are held, say, and this lock is in
    /// fact safe to hold across it. A scoped form of TrackingDisabled that is
    /// meant to be temporary. Zircon's Guard::CallUntracked().
    template <typename Op>
    auto call_untracked(Op &&op) -> void {
        if (!m_held) {
            std::forward<Op>(op)();
            return;
        }
        unregister();
        std::forward<Op>(op)();
        record();
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
    /// The order is Zircon's ValidateAndAcquire(): the policy prepares, the
    /// acquisition is recorded, then the lock is taken -- recorded before it
    /// is taken so that a report about an acquisition that is about to
    /// deadlock can name it. When a try-lock fails, the record is withdrawn;
    /// undoing whatever pre_validate() did is the policy's own business, as
    /// in Zircon.
    TA_NO_THREAD_SAFETY_ANALYSIS auto guard() -> void {
        Policy::pre_validate(m_lock, m_state);
        record();
        if (Policy::acquire(m_lock, m_state)) {
            m_held = true;
            return;
        }
        unregister();
    }

    /// The record goes before the lock does, in the order Zircon's
    /// Guard::Release() documents: releasing may re-enable interrupts, and a
    /// handler that then takes the same lock must not find it still recorded
    /// as held here.
    TA_NO_THREAD_SAFETY_ANALYSIS auto release_lock() -> void {
        unregister();
        Policy::release(m_lock, m_state);
    }

    auto record() -> void {
        if constexpr (LOCK_VALIDATION_ENABLED) {
            typename Policy::ValidationGuard const validation_guard{};
            m_context.set(current_lock_context());
            if (auto *context = m_context.get()) {
                context->acquire(m_entry);
            }
        }
    }

    auto unregister() -> void {
        if constexpr (LOCK_VALIDATION_ENABLED) {
            if (auto *context = m_context.get()) {
                typename Policy::ValidationGuard const validation_guard{};
                context->release(m_entry);
                m_context.set(nullptr);
            }
        }
    }

    LockType &m_lock;

    /// Whatever the policy keeps while the lock is held -- the saved interrupt
    /// state, for IrqSave.
    [[no_unique_address]]
    typename Policy::State m_state{};

    /// This acquisition's entry on the lock context's held list, and the
    /// context it went onto. Both are empty types when validation is compiled
    /// out, so a guard then costs no more than it did before lockdep had
    /// anything to say.
    [[no_unique_address]]
    AcquiredLockEntry m_entry;
    [[no_unique_address]]
    priv::ContextHolder<LOCK_VALIDATION_ENABLED> m_context;

    bool m_held{false};
};

template <>
class Guard<void> {
  public:
    Guard(...) {}
    ~Guard() {}

    auto unlock() -> void {}
    auto unguard() -> void {}
    [[nodiscard]]
    auto held() const -> bool {
        return false;
    }
};

template <typename LockType>
Guard(Lock<LockType> &) -> Guard<LockType>;

template <typename LockType, typename Option>
Guard(Lock<LockType> &, Option) -> Guard<LockType, Option>;

template <typename LockType>
Guard(LockType &) -> Guard<LockType>;

template <typename LockType, typename Option>
Guard(LockType &, Option) -> Guard<LockType, Option>;

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
