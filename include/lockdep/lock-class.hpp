/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef LOCKDEP_LOCK_CLASS_HPP
#define LOCKDEP_LOCK_CLASS_HPP 1

#include <lockdep/types.hpp>
#include <lockdep/lock-traits.hpp>
#include <lockdep/loop-detector.h>

#include <ustl/collections/intrusive/list.hpp>

namespace lockdep {
    class LockClass {
        using Self = LockClass;
      public:
        using Id = LockClassId;
        using Flags = LockFlags;

        static auto get(Id id) -> Self * { return const_cast<Self *>(id); }

        LockClass(Flags flags)
            : m_flags(flags), m_validator_state(), m_environment_hook() 
        {}

        auto id() const -> Id { return this; }

        auto is_leaf() const -> bool { return !!(m_flags & Flags::Leaf); }
        auto is_nestable() const -> bool { return !!(m_flags & Flags::Nestable); }
        auto is_multi_acquire() const -> bool { return !!(m_flags & Flags::MultiAcquire); }
        auto is_tracking_disabled() const -> bool { return !!(m_flags & Flags::TrackingDisabled); }
        auto is_reporting_disabled() const -> bool { return !!(m_flags & Flags::ReportingDisabled); }

        static auto add_dependency(Id prev, Id next) -> LockResult;
      private:
        LockClass(Self const &) = delete;
        auto operator=(Self const &) -> Self & = delete;
        LockClass(Self &&) = delete;
        auto operator=(Self &&) -> Self & = delete;

        Flags m_flags;
        priv::ValidatorState m_validator_state;

        ustl::collections::intrusive::ListMemberHook<> m_environment_hook;
        USTL_DECLARE_HOOK_OPTION(Self, m_environment_hook, EnvironmentHookOption);
      public:
        USTL_DECLARE_LIST(Self, EnvironmentList, EnvironmentHookOption);
    }; // class LockClass

} // namespace lockdep

#endif // #ifndef LOCKDEP_LOCK_CLASS_HPP