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

template <typename Lock, SavedStateOption Option>
using StateSaver = decltype(lockdep_get_state_saver(static_cast<Lock *>(nullptr)),
                            static_cast<SavedStateContainer<Option> *>(nullptr));

using IrqSave = SavedStateContainer<SavedStateOption::IrqSave>;
using PreemptDisable = SavedStateContainer<SavedStateOption::PreemptDisable>;
using IrqSaveAndPreemptDisable = SavedStateContainer<SavedStateOption::IrqSaveAndPreemptDisable>;

constexpr auto const kIrqSave = IrqSave{};
constexpr auto const kPreemptDisable = PreemptDisable{};
constexpr auto const kIrqSaveAndPreemptDisable = IrqSaveAndPreemptDisable{};

} // namespace lockdep

#define LOCKDEP_STATE_SAVER(LOCK, Option, STATE_SAVER)                                             \
    auto lockdep_get_state_saver(LOCK *, lockdep::SavedStateContainer<Option> *) -> STATE_SAVER;

#endif // #ifndef LOCKDEP_STATE_SAVER_HPP
