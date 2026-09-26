/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_GUARD_HPP
#define LOCKDEP_GUARD_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/lock_class.hpp>
#include <lockdep/lock_context.hpp>
#include <lockdep/lock_policy.hpp>
#include <lockdep/runtime_api.hpp>
#include <lockdep/thread_lock_state.hpp>

#include <optional>
#include <utility>

#include <ours/compiler.hpp>
#include <ustl/util/noncopyable.hpp>

namespace lockdep {
namespace priv {
    /// Remembers which held-lock state a Guard recorded its acquisition on, so
    /// that the release withdraws it from that same state. Empty when
    /// validation is off.
    ///
    /// The two specialisations deliberately give the same expressions
    /// different but matching types. The holder's type does not depend on the
    /// guard's template parameters, so the discarded branch of an
    /// `if constexpr (LOCK_VALIDATION_ENABLED)` inside the guard is still type
    /// checked against it -- clang 23 checks it, older compilers let it
    /// through. With validation on, get() yields the ThreadLockState that
    /// takes the guard's AcquiredLockEntry; with it off, the empty stand-in
    /// that takes the empty stand-in entry. set() takes what
    /// system_get_thread_lock_state() returns in both.
    template <bool Enabled>
    struct ContextHolder {
        [[nodiscard]]
        auto get() const -> ThreadLockState * {
            return m_state;
        }

        auto set(ThreadLockState *const state) -> void {
            m_state = state;
        }

        ThreadLockState *m_state{nullptr};
    };

    template <>
    struct ContextHolder<false> {
        [[nodiscard]]
        auto get() const -> NullThreadLockState * {
            return nullptr;
        }

        auto set(ThreadLockState *) -> void {}
    };

    /// The record a Guard keeps of its acquisition: a real entry where
    /// validation is compiled in, an empty stand-in elsewhere.
    using GuardEntry = IfLockValidationEnabled<AcquiredLockEntry, NullAcquiredLockEntry>;

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

/// The rank of a lock among the locks of its class held together.
///
/// Two locks of one class held at once read to the validator as a mistake
/// unless the second one's order rises above the first's. A Nestable class
/// supplies the order itself -- a depth in a tree, say -- and ScopedGuard
/// uses the locks' addresses. A strong type rather than a bare integer, so
/// that `Guard guard{lock, LockOrder{2}}` cannot be confused with the
/// option-tag form `Guard guard{lock, IRQ_SAVE}`.
struct LockOrder {
    usize value;
};

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
        : m_lock(lock.lock()), m_entry(&lock.lock(), lock.id()) {
        this->guard();
    }

    /// Takes a lock of a class that may be held more than once, at |order|
    /// among the locks of that class this context holds: the orders have to
    /// rise, or the validator reports InvalidNesting. Zircon's ordered Guard
    /// constructor.
    Guard(::lockdep::Lock<LockType> &lock, LockOrder const order) TA_ACQUIRE(lock)
        : m_lock(lock.lock()), m_entry(&lock.lock(), lock.id(), order.value) {
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
        : m_lock(lock), m_entry(&lock, DefaultLockClass<LockType>::id()) {
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

    /// Recorded on the state the environment hands out for this lock's
    /// options -- the CPU's for an irq-safe lock, the thread's otherwise --
    /// under the policy's validation guard.
    auto record() -> void {
        if constexpr (LOCK_VALIDATION_ENABLED) {
            typename Policy::ValidationGuard const validation_guard{};
            m_context.set(system_get_thread_lock_state(options_of(m_entry.id())));
            if (auto *const state = m_context.get()) {
                state->acquire(m_entry);
            }
        }
    }

    auto unregister() -> void {
        if constexpr (LOCK_VALIDATION_ENABLED) {
            if (auto *const state = m_context.get()) {
                typename Policy::ValidationGuard const validation_guard{};
                state->release(m_entry);
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
    priv::GuardEntry m_entry;
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

template <typename LockType>
Guard(Lock<LockType> &, LockOrder) -> Guard<LockType>;

template <typename LockType, typename Option>
Guard(Lock<LockType> &, Option) -> Guard<LockType, Option>;

template <typename LockType>
Guard(LockType &) -> Guard<LockType>;

template <typename LockType, typename Option>
Guard(LockType &, Option) -> Guard<LockType, Option>;

/// Holds |Count| locks of one class at once, taking them in address order.
///
/// Two locks of one class held together are ordered by address, so any two
/// sites that lock the same set agree on the order however they list it --
/// which is what makes holding several of one class safe at all. Each lock is
/// held by a Guard whose LockOrder is its address, so the validator checks
/// the order rises. Zircon's GuardMultiple (lockdep/guard_multiple.h).
///
/// Named after std::scoped_lock, the standard's guard over several locks, but
/// not built on its algorithm. std::scoped_lock avoids deadlock through
/// std::lock -- take one lock, try the others, and on a failure drop them all
/// and start over -- so one set of locks may be taken in a different order on
/// every attempt. Lockdep would see the same locks taken in both orders and
/// report them. Linux's lockdep could live with that, as it records no
/// dependency for a try-lock (validate_chain() in kernel/locking/lockdep.c),
/// but only by leaving every lock but the one it blocks on unchecked. A fixed
/// order keeps them all checked.
///
/// Meant for locks of one class. Locks of different classes are nested Guards,
/// whose order is exactly what the validator is there to check; given here,
/// they are taken in address order and validated as such.
template <usize Count, typename LockType, typename Option = NoOption>
class ScopedGuard {
    using Self = ScopedGuard;

    static_assert(Count > 0, "ScopedGuard needs at least one lock");

    /// The same restriction as Zircon's: a Nestable class orders its locks
    /// itself, and mixing that order with the address order would compare two
    /// unrelated sequences.
    static_assert(!priv::has_option(LockValidationOptionsV<LockType>, ValidationOptions::Nestable),
                  "a Nestable lock type is ordered by its own orders, not by ScopedGuard");

  public:
    /// Excluded from the thread-safety analysis, as Zircon's GuardMultiple is:
    /// the locks are held by Guards built inside std::optional, where the
    /// analysis cannot follow them.
    template <typename... Locks>
        requires(sizeof...(Locks) == Count && (std::is_same_v<Locks, Lock<LockType>> && ...))
    TA_NO_THREAD_SAFETY_ANALYSIS explicit ScopedGuard(Locks &...locks) {
        Lock<LockType> *sorted[Count] = {&locks...};
        sort_by_address(sorted);
        for (usize i = 0; i < Count; ++i) {
            m_guards[i].emplace(*sorted[i], LockOrder{reinterpret_cast<usize>(sorted[i])});
        }
    }

    /// Released in the reverse of the acquisition order, highest address
    /// first. The array's own destruction runs in that order too.
    TA_NO_THREAD_SAFETY_ANALYSIS ~ScopedGuard() {
        release();
    }

    TA_NO_THREAD_SAFETY_ANALYSIS auto release() -> void {
        for (usize i = Count; i > 0; --i) {
            if (m_guards[i - 1]) {
                m_guards[i - 1]->unlock();
            }
        }
    }

    /// Whether every lock is held.
    explicit operator bool() const {
        for (auto const &guard: m_guards) {
            if (!guard || !guard->held()) {
                return false;
            }
        }
        return true;
    }

  private:
    USTL_NO_MOVEABLE_AND_COPYABLE(ScopedGuard);

    /// Insertion sort: Count is a handful, and for two it is one compare and
    /// swap.
    static auto sort_by_address(Lock<LockType> *(&locks)[Count]) -> void {
        for (usize i = 1; i < Count; ++i) {
            auto *const lock = locks[i];
            auto j = i;
            while (j > 0 && locks[j - 1] > lock) {
                locks[j] = locks[j - 1];
                --j;
            }
            locks[j] = lock;
        }
    }

    /// Guards are neither movable nor default-constructible, and have to be
    /// constructed after sorting; an optional per slot is the plain way to
    /// construct each in place, where Zircon uses a union.
    std::optional<Guard<LockType, Option>> m_guards[Count];
};

} // namespace lockdep

#endif // #ifndef LOCKDEP_GUARD_HPP
