/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_LOCK_CLASS_STATE_HPP
#define LOCKDEP_LOCK_CLASS_STATE_HPP 1

#include <lockdep/lock-traits.hpp>
#include <lockdep/types.hpp>

namespace lockdep {
    /// Base of every lock class state.  Its address is the class id, so it must
    /// never be copied or moved.
    class LockClassState {
        using Self = LockClassState;

      public:
        constexpr LockClassState() = default;

        constexpr auto id() const -> LockClassId {
            return this;
        }

        LockClassState(Self const &) = delete;
        auto operator=(Self const &) -> Self & = delete;
    };

    /// Carries the traits of a lock class without tracking any acquisition.
    /// This is what a `MetadataOnly` build gets.
    class MetadataLockClassState : public LockClassState {
      public:
        constexpr explicit MetadataLockClassState(ValidationControl control)
            : m_control(control) {}

        constexpr auto control() const -> ValidationControl {
            return m_control;
        }

        /// Whether every bit in `control` is set for this class.
        constexpr auto has(ValidationControl control) const -> bool {
            return (m_control & control) == control;
        }

      protected:
        /// Lives here rather than in the validator so that metadata-only builds
        /// can still report what a lock class declared.
        ValidationControl m_control;
    };

    /// Adds the dependency graph used to validate acquisition order.
    ///
    /// The graph itself is not walked yet -- `LoopDetector` is the seam where
    /// cycle detection will live.  Everything is initialised so that a class
    /// which is never validated still has a well-defined state.
    class ValidatorLockClassState : public MetadataLockClassState {
        using Self = ValidatorLockClassState;

      public:
        constexpr explicit ValidatorLockClassState(ValidationControl control)
            : MetadataLockClassState(control) {}

        /// Whether this class opted out of order tracking entirely.
        constexpr auto tracking_disabled() const -> bool {
            return has(ValidationControl::TrackingDisabled);
        }

      private:
        /// Singly linked list of classes that have been observed held when a
        /// lock of this class was taken.
        [[maybe_unused]] Self *m_head{nullptr};
        [[maybe_unused]] Self *m_next{nullptr};

        /// Bookkeeping for Tarjan's strongly-connected-components walk.
        struct LoopNode {
            Self *state{nullptr};

            LoopNode *parent{nullptr};
            LoopNode *next{nullptr};

            Size index{0};
            Size least{0};
        };
        LoopNode m_loop_node{};

        class LoopDetector;
    };

    class ValidatorLockClassState::LoopDetector {
        using Self = ValidatorLockClassState::LoopDetector;

      public:
        constexpr LoopDetector() = default;

      private:
        [[maybe_unused]] Size m_generation{0};
        [[maybe_unused]] Size m_index{0};

        [[maybe_unused]] LoopNode *m_stack{nullptr};
    };

} // namespace lockdep

#endif // #ifndef LOCKDEP_LOCK_CLASS_STATE_HPP
