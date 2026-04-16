#pragma once

#include <atomic>

#include <lockdep/types.hpp>

#include <ustl/collections/intrusive/list.hpp>

namespace lockdep::priv {
    class ValidatorState {
        using Self = ValidatorState;
      public:
        /// Describe the edge information of the lock dependency graph.
        struct Dependency {
            /// The lock class that this dependency is pointing to.
            Self *target;
            std::atomic<Dependency *> parent;
            ustl::collections::intrusive::ListMemberHook<> hook;
            USTL_DECLARE_HOOK_OPTION(Dependency, hook, HookOption);
        };
        USTL_DECLARE_LIST(Dependency, Dependencies, typename Dependency::HookOption);

        auto add_dependency(Self &other) -> void {
            auto dependency = new Dependency{&other};
        }

        auto dependencies_forwards() const -> Dependencies const & { return m_locks_forwards; }
        auto dependencies_backwards() const -> Dependencies const & { return m_locks_backwards; }
      private:
        /// Acquired after this lock class
        Dependencies m_locks_forwards;
        /// Acquired before this lock class
        Dependencies m_locks_backwards;
    }; // class ValidatorLockClassState
    
    class LoopDetector {
        using Self = LoopDetector;
      public:
      private:
        usize m_generation;
        usize m_index;
    }; // class LoopDetector
     
} // namespace lockdep::priv