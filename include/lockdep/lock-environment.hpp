/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers

#ifndef LOCKDEP_LOCK_ENVIRONMENT_HPP
#define LOCKDEP_LOCK_ENVIRONMENT_HPP 1

#include <lockdep/lock-class.hpp>

namespace lockdep {
    class LockEnvironment {
        using Self = LockEnvironment;
      public:
        virtual auto report(LockClassId bad, LockClassId conflicting, LockResult result) -> void = 0;

        static auto get() -> Self & { return *CURRENT_ENVIRONMENT; }
        static auto set(Self &self) -> void { CURRENT_ENVIRONMENT = &self; }
      private:
        inline static Self *CURRENT_ENVIRONMENT;
    }; // class LockEnvironment

} // namespace lockdep

#endif // #ifndef LOCKDEP_LOCK_ENVIRONMENT_HPP