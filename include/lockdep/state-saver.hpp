/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_STATE_SAVER_HPP
#define LOCKDEP_STATE_SAVER_HPP 1

#include <lockdep/config.hpp>

#include <ustl/util/enum_bits.hpp>

namespace lockdep {
    enum class SavedStateOption {
        None = 0,

        IrqSave = (1 << 0),

        PreemptDisable = (1 << 1),

        IrqSaveAndPreemptDisable = IrqSave | PreemptDisable,
    };
    USTL_ENABLE_ENUM_BITMASK(SavedStateOption);

    /// Tag naming the state a `Guard` must save before it takes the lock.
    template <SavedStateOption Option>
    struct SavedStateContainer {
        static constexpr auto kOption = Option;
    };

    /// Stand-in for locks that declare no saver for a given option.  It is an
    /// empty type so `[[no_unique_address]]` makes it free in a `Guard`.
    struct NullStateSaver {
        NullStateSaver() = default;
    };

    /// Whether `T` is one of the tags a `Guard` accepts as an option.  Used to
    /// reject look-alike empty structs, which would otherwise be accepted and
    /// silently save no state at all.
    template <typename T>
    inline constexpr bool kIsSavedStateContainer = false;

    template <SavedStateOption Option>
    inline constexpr bool kIsSavedStateContainer<SavedStateContainer<Option>> = true;

} // namespace lockdep

/// Fallback overload.  Declared as a template so that the non-template
/// declarations produced by `LOCKDEP_STATE_SAVER` always win overload
/// resolution for the locks that have one.
///
/// It must be visible before `StateSaver` is defined, so ordinary lookup finds
/// it at template definition time while ADL still reaches the per-lock
/// declarations sitting next to each lock type.
template <typename Lock, lockdep::SavedStateOption Option>
constexpr auto lockdep_get_state_saver(Lock *, lockdep::SavedStateContainer<Option> *)
    -> lockdep::NullStateSaver;

namespace lockdep {
    /// Resolves to the RAII type that saves `Option` for `Lock`, or
    /// `NullStateSaver` when the lock declares none.
    ///
    /// Both arguments have to be passed: `LOCKDEP_STATE_SAVER` declares a
    /// two-parameter overload, and the option is what selects between the
    /// savers a single lock type declares.
    template <typename Lock, SavedStateOption Option>
    using StateSaver = decltype(lockdep_get_state_saver(
        static_cast<Lock *>(nullptr), static_cast<SavedStateContainer<Option> *>(nullptr)));

    /// Resolves to `StateSaver<Lock, Option>` when `Option` is present in
    /// `Requested`, and to `NullStateSaver` otherwise.
    template <typename Lock, SavedStateOption Requested, SavedStateOption Option>
    using StateSaverIfRequested =
        std::conditional_t<!!(Requested & Option), StateSaver<Lock, Option>, NullStateSaver>;

    using IrqSave = SavedStateContainer<SavedStateOption::IrqSave>;
    using PreemptDisable = SavedStateContainer<SavedStateOption::PreemptDisable>;
    using IrqSaveAndPreemptDisable =
        SavedStateContainer<SavedStateOption::IrqSaveAndPreemptDisable>;

    constexpr auto const kIrqSave = IrqSave{};
    constexpr auto const kPreemptDisable = PreemptDisable{};
    constexpr auto const kIrqSaveAndPreemptDisable = IrqSaveAndPreemptDisable{};

} // namespace lockdep

#define LOCKDEP_STATE_SAVER(LOCK, Option, STATE_SAVER)                                             \
    auto lockdep_get_state_saver(LOCK *, lockdep::SavedStateContainer<Option> *) -> STATE_SAVER;

#endif // #ifndef LOCKDEP_STATE_SAVER_HPP
