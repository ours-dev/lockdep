/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_LOCK_CLASS_STATE_HPP
#define LOCKDEP_LOCK_CLASS_STATE_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/lock_traits.hpp>
#include <lockdep/report.hpp>
#include <lockdep/types.hpp>

#include <ustl/sync/atomic.hpp>

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
class MetadataLockClassState: public LockClassState {
  public:
    constexpr explicit MetadataLockClassState(ValidationControl control) : m_control(control) {}

    constexpr auto control() const -> ValidationControl {
        return m_control;
    }

    /// Whether every bit in `control` is set for this class.
    constexpr auto has(ValidationControl control) const -> bool {
        return (m_control & control) == control;
    }

    /// Whether any bit in `control` is set for this class.
    constexpr auto has_any(ValidationControl control) const -> bool {
        return (m_control & control) != ValidationControl::None;
    }

  protected:
    /// Lives here rather than in the validator so that metadata-only builds
    /// can still report what a lock class declared.
    ValidationControl m_control;
};

/// A node in the lock dependency graph.
///
/// An edge `A -> B` means "a lock of class B was acquired while a lock of
/// class A was held".  A cycle in that graph is a lock ordering that two
/// contexts can run in opposite directions, which is a deadlock waiting for
/// the right interleaving.
///
/// Edges are recorded from inside the acquire path, so nothing here
/// allocates, takes a lock, or walks the whole graph: the set of successors
/// is a fixed-capacity array and the registry is a lock-free stack.  The
/// walk that actually finds cycles is `LoopDetector`, which is meant to run
/// somewhere idle rather than under a held lock.
class ValidatorLockClassState: public MetadataLockClassState {
    using Self = ValidatorLockClassState;

  public:
    /// Constant-initialisable, so the per-class state in `LockClass` needs
    /// no static constructor to run before the first acquisition.
    constexpr explicit ValidatorLockClassState(ValidationControl control)
        : MetadataLockClassState(control) {}

    /// Head of the registry of every class that has taken part in an
    /// acquisition.  The detector walks it to find roots.
    static auto registry() -> Self * {
        return m_registry.load(ustl::sync::MemoryOrder::Acquire);
    }

    auto next_class() const -> Self * {
        return m_next_class;
    }

    /// Puts this class in the registry, once.
    ///
    /// Called from the acquire path, so it must not block.  A second
    /// context that arrives while the first is still pushing returns early;
    /// the worst case is that a detector pass running right then does not
    /// see this class, and the next pass does.
    auto ensure_registered() -> void {
        using ustl::sync::MemoryOrder;
        if (m_registered.load(MemoryOrder::Acquire)) {
            return;
        }
        if (m_registered.exchange(1, MemoryOrder::AcqRel)) {
            return;
        }

        auto *head = m_registry.load(MemoryOrder::Relaxed);
        do {
            m_next_class = head;
        } while (!m_registry.compare_exchange_weak(head, this, MemoryOrder::Release,
                                                   MemoryOrder::Relaxed));
    }

    /// Number of successors recorded so far, clamped to the capacity.
    auto dependency_count() const -> usize {
        auto const count = m_dependency_count.load(ustl::sync::MemoryOrder::Acquire);
        return count < MAX_DEPENDENCIES ? count : MAX_DEPENDENCIES;
    }

    auto dependency_at(usize index) const -> Self * {
        return m_dependencies[index].load(ustl::sync::MemoryOrder::Acquire);
    }

    auto has_dependency(Self const *dependency) const -> bool {
        auto const count = dependency_count();
        for (usize i = 0; i < count; ++i) {
            if (dependency_at(i) == dependency) {
                return true;
            }
        }
        return false;
    }

    /// Records the edge `this -> dependency`.  Returns false only when the
    /// set is full, which costs completeness rather than soundness.
    ///
    /// Two contexts recording the same edge at once can both miss the
    /// duplicate check and take two slots.  That wastes capacity but does
    /// not change what the detector concludes.
    auto add_dependency(Self *dependency) -> bool {
        using ustl::sync::MemoryOrder;
        if (has_dependency(dependency)) {
            return true;
        }

        auto const slot = m_dependency_count.fetch_add(1, MemoryOrder::AcqRel);
        if (slot >= MAX_DEPENDENCIES) {
            return false;
        }

        m_dependencies[slot].store(dependency, MemoryOrder::Release);
        return true;
    }

    /// Recovers the node behind a class id.
    ///
    /// Only valid where validation is enabled, which is exactly where the
    /// storage in `LockClass` is a `ValidatorLockClassState`; the cast is
    /// what lets `LockClassId` stay a pointer to the common base.
    static auto from_id(LockClassId id) -> Self * {
        if (id == INVALID_LOCK_CLASS_ID) {
            return nullptr;
        }
        return static_cast<Self *>(const_cast<LockClassState *>(id));
    }

    /// Whether this class opted out of order tracking entirely.
    constexpr auto tracking_disabled() const -> bool {
        return has(ValidationControl::TrackingDisabled);
    }

    /// Whether a context may hold two locks of this class at once.
    constexpr auto nestable() const -> bool {
        return has_any(ValidationControl::Nestable | ValidationControl::MultiAcquire);
    }

    class LoopDetector;

  private:
    /// Registry of every class that has been acquired at least once.
    static inline ustl::sync::Atomic<Self *> m_registry{nullptr};

    Self *m_next_class{nullptr};
    ustl::sync::AtomicU32 m_registered{0};

    /// Successors of this class.  Append-only: an edge is never withdrawn,
    /// because an ordering observed once stays a fact about the code.
    ustl::sync::AtomicUsize m_dependency_count{0};
    ustl::sync::Atomic<Self *> m_dependencies[MAX_DEPENDENCIES]{};

    /// Scratch space for `LoopDetector`.  It lives in the node so the walk
    /// needs no stack of its own: `parent` is the depth-first call stack and
    /// `next` is Tarjan's stack, and a node sits on both at once.
    struct LoopNode {
        Self *state{nullptr};

        LoopNode *parent{nullptr};
        LoopNode *next{nullptr};

        usize index{0};
        usize least{0};

        /// Next successor to visit, so the walk can resume after descending.
        usize cursor{0};

        /// Which detector pass last touched this node.  Comparing against
        /// the detector's generation is what makes a pass free of any reset
        /// sweep over the graph.
        usize generation{0};

        bool on_stack{false};
    };
    LoopNode m_loop_node{};
};

/// Finds cycles in the lock dependency graph with Tarjan's
/// strongly-connected-components algorithm.
///
/// Any component with more than one class is a cycle, and so is a class
/// that depends on itself.  The walk is iterative: the graph is built by
/// arbitrary kernel code, so its depth is not something to spend stack on.
///
/// This is deliberately not called from the acquire path.  Recording an
/// edge is O(held locks); a full pass is O(classes + edges) and belongs on
/// an idle thread or in a test.
class ValidatorLockClassState::LoopDetector {
    using Self = ValidatorLockClassState::LoopDetector;
    using Node = ValidatorLockClassState::LoopNode;

  public:
    constexpr LoopDetector() = default;

    /// Runs one pass over the whole registry.  Returns the number of cycles
    /// found, reporting each one.
    ///
    /// One pass at a time: the walk's scratch space lives in the nodes, so
    /// two passes running at once would overwrite each other's marks.
    auto run() -> usize {
        // The generation has to come from a counter shared by every
        // detector, not from this object.  The marks it is compared against
        // live in the graph and outlast any one detector, so a fresh
        // detector starting over at one would read the previous pass's
        // marks as its own and skip the whole graph.
        m_generation = m_next_generation.fetch_add(1, ustl::sync::MemoryOrder::AcqRel) + 1;
        m_index = 0;
        m_stack = nullptr;

        usize cycles = 0;
        for (auto *state = ValidatorLockClassState::registry(); state != nullptr;
             state = state->next_class()) {
            if (state->m_loop_node.generation != m_generation) {
                cycles += visit(state);
            }
        }
        return cycles;
    }

  private:
    auto enter(ValidatorLockClassState *state) -> Node * {
        auto *node = &state->m_loop_node;
        node->state = state;
        node->generation = m_generation;
        node->index = m_index;
        node->least = m_index;
        node->cursor = 0;
        ++m_index;

        node->next = m_stack;
        m_stack = node;
        node->on_stack = true;
        return node;
    }

    /// Pops the component rooted at `root` and reports it if it is a cycle.
    auto close(Node *root) -> usize {
        usize members = 0;
        Node *member = nullptr;
        do {
            member = m_stack;
            m_stack = member->next;
            member->next = nullptr;
            member->on_stack = false;
            ++members;

            if (member != root) {
                report(root->state, member->state);
            }
        } while (member != root);

        if (members > 1) {
            return 1;
        }

        // A single-node component is still a cycle when the class depends
        // on itself: one context taking two locks of that class in a fixed
        // order can meet another taking them in the other order.
        if (root->state->has_dependency(root->state)) {
            report(root->state, root->state);
            return 1;
        }
        return 0;
    }

    static auto report(ValidatorLockClassState *root, ValidatorLockClassState *member) -> void {
        if (root->has(ValidationControl::ReportingDisabled) ||
            member->has(ValidationControl::ReportingDisabled)) {
            return;
        }
        report_violation(ViolationReport{
            Violation::CircularDependency,
            member->id(),
            root->id(),
            root->has(ValidationControl::ReAcquireFatal),
        });
    }

    auto visit(ValidatorLockClassState *root) -> usize {
        usize cycles = 0;

        auto *current = enter(root);
        current->parent = nullptr;

        while (current != nullptr) {
            auto *state = current->state;
            if (current->cursor < state->dependency_count()) {
                auto *successor = state->dependency_at(current->cursor);
                ++current->cursor;
                if (successor == nullptr) {
                    continue;
                }

                auto *child = &successor->m_loop_node;
                if (child->generation != m_generation) {
                    // Descend.
                    auto *entered = enter(successor);
                    entered->parent = current;
                    current = entered;
                } else if (child->on_stack) {
                    // Back edge into the component being built.
                    if (child->index < current->least) {
                        current->least = child->index;
                    }
                }
                // Otherwise the successor is in a component already closed.
                continue;
            }

            // Every successor visited: this node is done.
            if (current->least == current->index) {
                cycles += close(current);
            }

            auto *parent = current->parent;
            if (parent != nullptr && current->least < parent->least) {
                parent->least = current->least;
            }
            current = parent;
        }

        return cycles;
    }

    /// Shared by every detector so that no pass ever reuses the number a
    /// previous pass stamped into the graph.  Node marks start at zero, so
    /// the first pass is generation one.
    static inline ustl::sync::AtomicUsize m_next_generation{0};

    usize m_generation{0};
    usize m_index{0};

    Node *m_stack{nullptr};
};

/// Walks the dependency graph once and reports every cycle it finds.
///
/// Safe to call from anywhere that is not holding a lock being validated.
/// Returns the number of cycles found, so a test or a boot-time check can
/// assert on it.
inline auto detect_lock_order_cycles() -> usize {
    if constexpr (LOCK_VALIDATION_ENABLED) {
        ValidatorLockClassState::LoopDetector detector;
        return detector.run();
    } else {
        return 0;
    }
}

} // namespace lockdep

#endif // #ifndef LOCKDEP_LOCK_CLASS_STATE_HPP
