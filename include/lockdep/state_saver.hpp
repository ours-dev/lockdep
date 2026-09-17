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
using StateSaver = decltype(lockdep_get_state_saver(static_cast<Lock *>(nullptr),
                                                    static_cast<Option *>(nullptr)));

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
