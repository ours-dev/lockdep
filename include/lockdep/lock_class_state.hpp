#pragma once

#include "lock_traits.hpp"
#include "types.hpp"

namespace lockdep {
class LockClassState {
  public:
    auto id() const -> LockClassId {
        return this;
    }
};

class MetadataLockClassState: public LockClassState {};

class ValidatorLockClassState: public MetadataLockClassState {
    using Self = ValidatorLockClassState;

  public:
  private:
    ValidationControl m_control;

    Self *m_head;
    Self *m_next;

    struct LoopNode {
        Self *state;

        LoopNode *parent;
        LoopNode *next;

        usize index;
        usize least;
    };
    LoopNode m_loop_node;

    class LoopDetector;
};

class ValidatorLockClassState::LoopDetector {
    using Self = ValidatorLockClassState::LoopDetector;

  public:
  private:
    usize m_generation;
    usize m_index;

    LoopNode *m_stack;
};

} // namespace lockdep