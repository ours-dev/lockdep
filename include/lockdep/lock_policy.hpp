/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_LOCK_POLICY_HPP
#define LOCKDEP_LOCK_POLICY_HPP 1

#include <lockdep/state_saver.hpp>

#include <concepts>
#include <optional>
#include <type_traits>

#include <ours/compiler.hpp>

/// How a Guard takes and drops a lock.
///
/// A lock *policy* is the whole of one way to hold one lock type: what state
/// has to be kept while it is held, what must happen before lockdep records
/// the acquisition, how the lock is acquired, and how it is released. It
/// replaces the state saver, which could only bracket an acquisition from
/// outside -- it had no say in the acquire itself, so there was nowhere to
/// put a try-lock, and nothing that could run before the acquisition was
/// recorded. Zircon's counterpart is lockdep/lock_policy.h.
///
/// A lock type maps each option it supports to a policy with a declaration
/// next to its own definition, found by argument-dependent lookup:
///
///     auto lockdep_policy(SpinLock *, lockdep::IrqSave *) -> IrqSavePolicy;
///
/// The declaration is never defined; it exists only to be named by decltype.
/// Zircon spells the same thing through LOCK_DEP_POLICY_OPTION, a macro that
/// hides nothing but this declaration.
namespace lockdep {
/// The option a Guard takes when none is written, `Guard<Lock>`.
///
/// A tag of its own rather than `void`, which is what Zircon uses. A lock
/// maps its option-less form with `lockdep_policy(Lock *, NoOption *)`; had
/// that parameter been `void *`, every option tag the lock declares no policy
/// for would convert to it and silently resolve to the option-less policy,
/// instead of failing to compile.
struct NoOption {};

/// The validation guard for a policy that needs none.
///
/// A policy's ValidationGuard is held around every update of the held-lock
/// list. A lock whose policy already runs with interrupts disabled -- a spin
/// lock taken with IrqSave -- needs nothing more. One that runs with them
/// enabled -- a mutex -- has to disable them for the few instructions the
/// list is being relinked, which is what Zircon's LockValidationGuard
/// (kernel/include/kernel/lock_validation_guard.h) does for its Mutex.
struct NullValidationGuard {
    /// User-provided, so that a guard declared only for its lifetime is not
    /// flagged as an unused variable -- Zircon's NullValidationGuard does the
    /// same.
    NullValidationGuard() {}
};

/// What every policy must provide.
///
/// The Guard drives it in this order, which is Zircon's ValidateAndAcquire:
///
///   1. pre_validate() -- e.g. disable interrupts;
///   2. the acquisition is recorded, under a ValidationGuard;
///   3. acquire() -- false means a try-lock failed, and step 2 is undone.
///
/// and in reverse on release: the record is removed first, then release()
/// drops the lock and restores whatever pre_validate() saved.
///
/// The order is not a stylistic choice. An irq-safe lock that is recorded
/// before interrupts are off can be interrupted between the two, and a
/// handler taking the same lock then finds it recorded as held and reports a
/// re-entrance that never happened. Symmetrically, a lock released before its
/// record is removed can be retaken by a handler while it is still recorded.
///
/// acquire() and release() declare TA_ACQUIRE(lock) and TA_RELEASE(lock), as
/// Zircon's policies declare __TA_ACQUIRE and __TA_RELEASE, and keep their
/// bodies out of the analysis with TA_NO_THREAD_SAFETY_ANALYSIS -- the split
/// a kernel lock makes between its annotated interface and its body. Without
/// the exclusion the analysis checks the body against the lock's own
/// annotations, which cannot be right for every lock at once: for an
/// annotated lock an unannotated acquire() reads as returning with the lock
/// still held, which failed every Guard in the kernel; for an unannotated
/// lock an annotated one reads as returning without it. The attributes go in
/// front of the declaration: after the trailing return type they would attach
/// to the type and be ignored.
template <typename Policy, typename Lock>
concept LockPolicy = requires(Lock &lock, typename Policy::State &state) {
    typename Policy::State;
    typename Policy::ValidationGuard;
    { Policy::pre_validate(lock, state) } -> std::same_as<void>;
    { Policy::acquire(lock, state) } -> std::same_as<bool>;
    { Policy::release(lock, state) } -> std::same_as<void>;
};

/// Takes and drops a lock with nothing around it.
///
/// What `Guard<Lock>` uses for a lock type that declares no policy of its own
/// and does not demand an option -- a sleeping mutex, whose acquire() already
/// does everything holding it requires. Zircon compiles its equivalent,
/// DefaultLockPolicy, only outside the kernel; here a lock that must not have
/// a default says so with lockdep_option_required() instead, see below.
struct DefaultLockPolicy {
    struct State {};

    using ValidationGuard = NullValidationGuard;

    template <typename Lock>
    static auto pre_validate(Lock &, State &) -> void {}

    template <typename Lock>
    TA_ACQUIRE(lock)
    TA_NO_THREAD_SAFETY_ANALYSIS static auto acquire(Lock &lock, State &) -> bool {
        lock.acquire();
        return true;
    }

    template <typename Lock>
    TA_RELEASE(lock)
    TA_NO_THREAD_SAFETY_ANALYSIS static auto release(Lock &lock, State &) -> void {
        lock.release();
    }
};

/// Whether a Guard on |Lock| has to name an option.
///
/// A lock opts in by declaring -- never defining -- an overload next to its
/// own type, found by argument-dependent lookup like its policies:
///
///     auto lockdep_option_required(MyLock *) -> void;
///
/// It is for a lock whose acquire() changes nothing about the running CPU, so
/// that holding it safely depends entirely on the option. A spin lock is the
/// case in point: a `Guard<SpinLock>` with no option holds it with interrupts
/// and preemption both enabled, and the holder can be preempted -- or
/// interrupted by a handler that wants the same lock -- while the next thing
/// to run on that CPU spins on it for ever. Such a lock has no safe default,
/// so it is denied DefaultLockPolicy and every caller has to say what it
/// saves. Zircon reaches the same place through LOCK_DEP_POLICY_OPTION, which
/// declares the lock's `void` policy as AmbiguousOption.
template <typename Lock>
concept OptionRequired = requires(Lock *lock) { lockdep_option_required(lock); };

namespace priv {
    /// The policy a lock type declares for |Option|, if it declares one.
    template <typename Lock, typename Option>
    concept DeclaresPolicy =
        requires { lockdep_policy(static_cast<Lock *>(nullptr), static_cast<Option *>(nullptr)); };

    /// Whether |Lock| declares a state saver for |Option| through the older
    /// LOCKDEP_STATE_SAVER / lockdep_get_state_saver() interface.
    template <typename Lock, typename Option>
    concept DeclaresStateSaver = requires { typename StateSaver<Lock, Option>; };

    /// Presents a state saver as a policy, so that locks written against the
    /// saver interface keep working unchanged while they move to policies.
    ///
    /// The saver is constructed in pre_validate(), before the acquisition is
    /// recorded -- the point where an IrqSave saver has to have disabled
    /// interrupts -- and destroyed in release(), after the lock is dropped.
    /// Destroying it there rather than when the guard goes out of scope means
    /// an early unlock() restores the saved state at once, as Zircon's
    /// Guard::Release() does.
    ///
    /// The state is a std::optional because a saver is an RAII type with no
    /// operation of its own to restore early: ending its lifetime is the only
    /// way to make it restore.
    template <typename Saver>
    struct SaverPolicy {
        struct State {
            std::optional<Saver> saver;
        };

        using ValidationGuard = NullValidationGuard;

        template <typename Lock>
        static auto pre_validate(Lock &, State &state) -> void {
            state.saver.emplace();
        }

        template <typename Lock>
        TA_ACQUIRE(lock)
        TA_NO_THREAD_SAFETY_ANALYSIS static auto acquire(Lock &lock, State &) -> bool {
            lock.acquire();
            return true;
        }

        template <typename Lock>
        TA_RELEASE(lock)
        TA_NO_THREAD_SAFETY_ANALYSIS static auto release(Lock &lock, State &state) -> void {
            lock.release();
            state.saver.reset();
        }
    };

    /// Resolves (Lock, Option) to a policy, in order of preference:
    ///
    ///   1. the policy the lock declares for the option;
    ///   2. for an option, the state saver the lock declares for it;
    ///   3. with no option, DefaultLockPolicy -- unless the lock demands one.
    ///
    /// Anything else has no policy, and the primary template's missing Type
    /// is what Guard reports.
    template <typename Lock, typename Option>
    struct PolicyLookup {};

    template <typename Lock, typename Option>
        requires DeclaresPolicy<Lock, Option>
    struct PolicyLookup<Lock, Option> {
        using Type =
            decltype(lockdep_policy(static_cast<Lock *>(nullptr), static_cast<Option *>(nullptr)));
    };

    template <typename Lock, typename Option>
        requires(!DeclaresPolicy<Lock, Option> && !std::is_same_v<Option, NoOption> &&
                 DeclaresStateSaver<Lock, Option>)
    struct PolicyLookup<Lock, Option> {
        using Type = SaverPolicy<StateSaver<Lock, Option>>;
    };

    template <typename Lock>
        requires(!DeclaresPolicy<Lock, NoOption> && !OptionRequired<Lock>)
    struct PolicyLookup<Lock, NoOption> {
        using Type = DefaultLockPolicy;
    };

} // namespace priv

/// Whether a Guard can hold |Lock| under |Option| at all.
template <typename Lock, typename Option>
concept HasPolicy = requires { typename priv::PolicyLookup<Lock, Option>::Type; };

/// The policy a `Guard<Lock, Option>` holds its lock with.
template <typename Lock, typename Option>
using PolicyFor = typename priv::PolicyLookup<Lock, Option>::Type;

} // namespace lockdep

#endif // #ifndef LOCKDEP_LOCK_POLICY_HPP
