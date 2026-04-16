#include "lockdep/lock-class.hpp"

namespace lockdep {
    auto LockClass::add_dependency(Id prev, Id next) -> LockResult {
        auto const from = LockClass::get(prev);
        auto const to = LockClass::get(next);

    }
} // namespace lockdep