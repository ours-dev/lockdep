/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_STATE_SAVER_HPP
#define LOCKDEP_STATE_SAVER_HPP 1

#include <lockdep/config.hpp>

#include <ustl/util/enum_bits.hpp>

namespace lockdep {
enum class SavedStateOption {
    IrqSave = (1 << 0),
    PreemptDisable = (1 << 1),

    IrqSaveAndPreemptDisable = IrqSave | PreemptDisable,
};
USTL_ENABLE_ENUM_BITMASK(SavedStateOption);

template <SavedStateOption Option>
struct SavedStateContainer {};

/// Whether |T| is one of the tags a Guard accepts as an option.
///
/// Guard checks its options against this so that a look-alike tag -- an empty
/// struct of the same name declared somewhere else, which is exactly what
/// ours::IrqSave used to be -- is rejected with a message that names the
/// mistake, rather than surfacing as a failed lookup of
/// lockdep_get_state_saver() several instantiations deep.
template <typename T>
constexpr bool IsSavedStateContainerV = false;

template <SavedStateOption Option>
constexpr bool IsSavedStateContainerV<SavedStateContainer<Option>> = true;

/// The state-saving object a Guard has to hold for |Option| while it guards a
/// |Lock|, where |Option| is one of the SavedStateContainer tags below.
///
/// It is resolved through an overload of lockdep_get_state_saver() that each
/// lock's own header declares -- and never defines -- with
/// LOCKDEP_STATE_SAVER. That indirection is the point: lockdep is a standalone
/// library and has no way to name arch::AutoInterruptDisabler, nor should it.
/// The lock's header already includes the disabler it needs, so it is the one
/// that names it, and the call is only ever used unevaluated.
///
/// There is deliberately no fallback overload. A lock that is asked for an
/// option it declares no saver for fails to compile here, which is the only
/// safe answer: a fallback to "save nothing" would turn a forgotten
/// declaration back into the silent Guard<Lock, IrqSave> that leaves
/// interrupts enabled.
///
/// This alias used to read
///
///     decltype(lockdep_get_state_saver(static_cast<Lock *>(nullptr)),
///              static_cast<SavedStateContainer<Option> *>(nullptr))
///
/// which is not a two-argument call: the closing parenthesis lands after the
/// first argument, making the whole thing a comma expression whose type is
/// that of its right operand. So the alias resolved to
/// `SavedStateContainer<Option> *` for every lock and every option -- a
/// pointer to an empty tag struct, which disables nothing. Nothing caught it
/// because the alias had no users; Guard stored the option tags directly and
/// never looked at them, so every `Guard<SpinLock, IrqSave>` in the kernel was
/// a bare spin lock that left interrupts enabled.
template <typename Lock, typename Option>
using StateSaver =
    decltype(lockdep_get_state_saver(static_cast<Lock *>(nullptr), static_cast<Option *>(nullptr)));

/// Whether a Guard on |Lock| has to name at least one option.
///
/// A lock opts in by declaring -- never defining -- an overload next to its
/// own type, found by argument-dependent lookup exactly as its state savers
/// are:
///
///     auto lockdep_option_required(MyLock *) -> void;
///
/// It is for a lock whose acquire() changes nothing about the running CPU, so
/// that holding it safely depends entirely on a saver. A spin lock is the
/// case in point: a `Guard<SpinLock>` with no option holds it with interrupts
/// and preemption both enabled, and the holder can be preempted -- or
/// interrupted by a handler that wants the same lock -- while the next thing
/// to run on that CPU spins on it for ever. Such a lock has no safe default,
/// so the only answer is to make the caller say which state it saves.
///
/// Zircon reaches the same place by another road: LOCK_DEP_POLICY_OPTION in
/// lock_policy.h declares the lock's `void` option as AmbiguousOption, and
/// Guard's static_assert refuses it. The `void` spelling does not carry over
/// to StateSaver, which looks an option up through a pointer to it: a void*
/// parameter would also catch every option tag the lock declares no saver
/// for, through the implicit T* -> void* conversion, and quietly resolve it
/// to this marker instead of failing to compile. A hook with a name of its
/// own cannot be reached by accident.
template <typename Lock>
concept OptionRequired = requires(Lock *lock) { lockdep_option_required(lock); };

using IrqSave = SavedStateContainer<SavedStateOption::IrqSave>;
using PreemptDisable = SavedStateContainer<SavedStateOption::PreemptDisable>;
using IrqSaveAndPreemptDisable = SavedStateContainer<SavedStateOption::IrqSaveAndPreemptDisable>;

constexpr auto IRQ_SAVE = IrqSave{};
constexpr auto PREEMPT_DISABLE = PreemptDisable{};
constexpr auto IRQ_SAVE_AND_PREEMPT_DISABLE = IrqSaveAndPreemptDisable{};

} // namespace lockdep

#define LOCKDEP_STATE_SAVER(LOCK, Option, STATE_SAVER)                                             \
    auto lockdep_get_state_saver(LOCK *, lockdep::SavedStateContainer<Option> *) -> STATE_SAVER;

#endif // #ifndef LOCKDEP_STATE_SAVER_HPP
