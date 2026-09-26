/// SPDX-License-Identifier: MIT
/// Copyright (c) 2026 ours Developers
#ifndef LOCKDEP_LOCK_CLASS_STATE_HPP
#define LOCKDEP_LOCK_CLASS_STATE_HPP 1

#include <lockdep/config.hpp>
#include <lockdep/lock_traits.hpp>
#include <lockdep/runtime_api.hpp>
#include <lockdep/types.hpp>

#include <array>
#include <atomic>
#include <string_view>
#include <type_traits>

namespace lockdep {
class LockClassState;

namespace priv {
    template <bool Enabled>
    class LoopDetector;

    /// The name of |T| as the compiler spells it, cut out of
    /// __PRETTY_FUNCTION__ at compile time -- what Zircon's LockNameHelper
    /// (lockdep/lock_name_helper.h) does for the class and line of a
    /// LOCK_DEP_INSTRUMENT. For a lock declared with
    /// `lock_class<struct ThreadPiLock>()`, it is the tag's qualified name.
    ///
    /// Clang renders the function as `... type_name() [T = ThreadPiLock]`;
    /// GCC as `... [with T = ThreadPiLock; std::string_view = ...]`, hence
    /// the stop at the first ';' too.
    template <typename T>
    constexpr auto type_name() -> std::string_view {
        std::string_view const function{__PRETTY_FUNCTION__};
        constexpr std::string_view MARKER{"T = "};
        auto const start = function.find(MARKER) + MARKER.size();
        auto end = function.find(';', start);
        if (end == std::string_view::npos) {
            end = function.rfind(']');
        }
        return function.substr(start, end - start);
    }

    /// type_name() copied into a null-terminated array with static storage,
    /// so that a class state can keep a plain `char const *` to it.
    template <typename T>
    struct TypeName {
        static constexpr std::string_view VIEW = type_name<T>();

        static constexpr auto STORAGE = [] {
            std::array<char, VIEW.size() + 1> storage{};
            for (usize i = 0; i < VIEW.size(); ++i) {
                storage[i] = VIEW[i];
            }
            return storage;
        }();

        static constexpr char const *VALUE = STORAGE.data();
    };

    /// The smallest prime from a short table that is at least |n| -- Zircon's
    /// NextPrime() (lockdep/common.h), and the same table. Each step is a
    /// little under double the one before and as far as it can be from the
    /// powers of two either side; 0 when |n| is beyond the table.
    constexpr auto next_prime(usize const n) -> usize {
        if (n < (1U << 4)) {
            return 23;
        }
        if (n < (1U << 5)) {
            return 53;
        }
        if (n < (1U << 6)) {
            return 97;
        }
        if (n < (1U << 7)) {
            return 193;
        }
        if (n < (1U << 8)) {
            return 389;
        }
        if (n < (1U << 9)) {
            return 769;
        }
        if (n < (1U << 10)) {
            return 1543;
        }
        return 0;
    }

} // namespace priv

/// How many slots each class's dependency set has: LOCKDEP_MAX_DEPENDENCIES
/// raised to a prime. The set is hashed by the address of a class state, and
/// those addresses are all aligned alike; taken modulo a power of two they
/// would crowd into a few slots, modulo a prime they spread.
constexpr usize DEPENDENCY_SET_CAPACITY = priv::next_prime(MAX_DEPENDENCIES);
static_assert(DEPENDENCY_SET_CAPACITY != 0, "LOCKDEP_MAX_DEPENDENCIES is beyond the prime table");

namespace priv {
    enum class AddResult {
        /// The edge is new: the graph changed.
        Added,

        /// The edge was already recorded.
        Present,

        /// The set is full and the edge went unrecorded, which costs
        /// completeness rather than soundness.
        Full,
    };

    /// The successors of one lock class: a lock-free, wait-free hash set of
    /// class states. Zircon's LockDependencySet (lockdep/lock_dependency_set.h).
    ///
    /// Open addressing with linear probing over a fixed array, whose slots only
    /// ever change from empty to holding a class and never back. That one
    /// invariant carries the rest:
    ///
    ///   - a lookup stops at the first empty slot, since nothing it looks for
    ///     can lie beyond a slot that has never been filled;
    ///   - an insertion claims the first empty slot with a compare-exchange;
    ///     losing the race to the same class means the edge is present, losing
    ///     it to another means the probe carries on -- so an edge recorded by
    ///     several CPUs at once takes exactly one slot;
    ///   - the set is full only once every slot has been probed.
    ///
    /// All of it is relaxed, as in Zircon. The only thing one CPU publishes to
    /// another is the pointer itself, and what it points at is a class state
    /// with static storage, constant-initialised before anything runs; nothing
    /// is read through the pointer that its store would have to order.
    class DependencySet {
        using Slot = std::atomic<LockClassState *>;

      public:
        constexpr DependencySet() = default;

        DependencySet(DependencySet const &) = delete;
        auto operator=(DependencySet const &) -> DependencySet & = delete;

        [[nodiscard]]
        auto contains(LockClassState const *const state) const -> bool {
            for (usize probe = 0; probe < DEPENDENCY_SET_CAPACITY; ++probe) {
                auto const *const held = slot_for(state, probe).load(std::memory_order_relaxed);
                if (held == state) {
                    return true;
                }
                if (held == nullptr) {
                    return false;
                }
            }
            return false;
        }

        auto insert(LockClassState *const state) -> AddResult {
            for (usize probe = 0; probe < DEPENDENCY_SET_CAPACITY; ++probe) {
                auto &slot = slot_for(state, probe);
                auto *held = slot.load(std::memory_order_relaxed);
                if (held == state) {
                    return AddResult::Present;
                }
                // Retried only while the slot still reads empty: a spurious
                // failure of the weak exchange, not a lost race.
                while (held == nullptr) {
                    if (slot.compare_exchange_weak(held, state, std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
                        return AddResult::Added;
                    }
                    if (held == state) {
                        return AddResult::Present;
                    }
                }
            }
            return AddResult::Full;
        }

        /// The class in |slot|, or nullptr if the slot is empty: how the loop
        /// detector walks the set.
        [[nodiscard]]
        auto at(usize const slot) const -> LockClassState * {
            return m_slots[slot].load(std::memory_order_relaxed);
        }

      private:
        /// Zircon's trivial hash: the address, plus the probe, modulo the prime
        /// capacity.
        auto slot_for(LockClassState const *const state, usize const probe) -> Slot & {
            return m_slots[index_of(state, probe)];
        }

        auto slot_for(LockClassState const *const state, usize const probe) const -> Slot const & {
            return m_slots[index_of(state, probe)];
        }

        static auto index_of(LockClassState const *const state, usize const probe) -> usize {
            return (reinterpret_cast<usize>(state) + probe) % DEPENDENCY_SET_CAPACITY;
        }

        Slot m_slots[DEPENDENCY_SET_CAPACITY]{};
    };

    /// A lock class's node in the lock-order graph: the part of its state that
    /// only validation needs.
    ///
    /// An edge A -> B means "a lock of class B was acquired while a lock of
    /// class A was held", and it is stored in A's node, among A's successors.
    /// Zircon stores the same edge in B's set, among its predecessors; either
    /// works, as long as the reverse-edge check asks the matching set.
    ///
    /// Edges are recorded from inside the acquire path, so nothing here
    /// allocates, takes a lock, or walks the whole graph: the successors are a
    /// lock-free hash set and the registry of nodes is a lock-free stack. The
    /// walk that finds cycles is LoopDetector, run off the acquire path.
    class GraphNode {
      public:
        constexpr GraphNode() = default;

        /// Head of the registry of every class that has taken part in an
        /// acquisition. The detector walks it to find roots.
        static auto registry() -> LockClassState * {
            return m_registry.load(std::memory_order_acquire);
        }

        [[nodiscard]]
        auto next_class() const -> LockClassState * {
            return m_next_class;
        }

        /// Puts |owner| -- the class this node belongs to -- in the registry,
        /// once.
        ///
        /// Called from the acquire path, so it must not block. A second
        /// context that arrives while the first is still pushing returns early;
        /// the worst case is that a detector pass running right then does not
        /// see this class, and the next pass does.
        auto ensure_registered(LockClassState &owner) -> void {
            if (m_registered.load(std::memory_order_acquire)) {
                return;
            }
            if (m_registered.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            auto *head = m_registry.load(std::memory_order_relaxed);
            do {
                m_next_class = head;
            } while (!m_registry.compare_exchange_weak(head, &owner, std::memory_order_release,
                                                       std::memory_order_relaxed));
        }

        /// How many slots the successor set has, and the class in one of them
        /// or nullptr: what the loop detector walks.
        [[nodiscard]]
        static constexpr auto dependency_slots() -> usize {
            return DEPENDENCY_SET_CAPACITY;
        }

        [[nodiscard]]
        auto dependency_at(usize const slot) const -> LockClassState * {
            return m_dependencies.at(slot);
        }

        [[nodiscard]]
        auto has_dependency(LockClassState const *const dependency) const -> bool {
            return m_dependencies.contains(dependency);
        }

        /// Records the edge to |dependency|.
        ///
        /// The validator needs to tell a new edge from a known one: only a new
        /// edge can have changed what is true of the graph, so only then does
        /// it check the pair and ask for a loop-detection pass. That is
        /// Zircon's AddLockClass() returning Success versus DependencyExists.
        /// The set makes the answer exact even under a race: an edge recorded
        /// by several CPUs at once is Added for exactly one of them.
        auto add_dependency(LockClassState *const dependency) -> AddResult {
            return m_dependencies.insert(dependency);
        }

      private:
        template <bool>
        friend class LoopDetector;

        /// Registry of every class that has been acquired at least once.
        static inline std::atomic<LockClassState *> m_registry{nullptr};

        LockClassState *m_next_class{nullptr};
        std::atomic<bool> m_registered{false};

        /// Append-only: an edge is never withdrawn, because an ordering
        /// observed once stays a fact about the code.
        DependencySet m_dependencies;

        /// Scratch space for LoopDetector. It lives in the node so the walk
        /// needs no stack of its own: `parent` is the depth-first call stack
        /// and `next` is Tarjan's stack, and a node sits on both at once.
        struct LoopNode {
            LockClassState *state{nullptr};

            LoopNode *parent{nullptr};
            LoopNode *next{nullptr};

            usize index{0};
            usize least{0};

            /// Next successor slot to visit, so the walk can resume after
            /// descending.
            usize cursor{0};

            /// Which detector pass last touched this node. Comparing against
            /// the detector's generation is what makes a pass free of any
            /// reset sweep over the graph.
            usize generation{0};

            bool on_stack{false};
        };
        LoopNode m_loop_node{};
    };

    /// GraphNode's interface with nothing behind it, for a class state where
    /// validation is compiled out. The acquire path is written once against
    /// the node's interface, and has to compile at every feature level.
    class NoGraphNode {
      public:
        static auto registry() -> LockClassState * {
            return nullptr;
        }

        [[nodiscard]]
        auto next_class() const -> LockClassState * {
            return nullptr;
        }

        auto ensure_registered(LockClassState &) -> void {}

        [[nodiscard]]
        static constexpr auto dependency_slots() -> usize {
            return 0;
        }

        [[nodiscard]]
        auto dependency_at(usize) const -> LockClassState * {
            return nullptr;
        }

        [[nodiscard]]
        auto has_dependency(LockClassState const *) const -> bool {
            return false;
        }

        auto add_dependency(LockClassState *) -> AddResult {
            return AddResult::Present;
        }
    };

} // namespace priv

/// The state of one lock class: its name, its options, and -- where
/// validation is compiled in -- its node in the lock-order graph. Its
/// address is the class id, so it is never copied or moved.
///
/// One class holding its parts as members, where Zircon chains LockClassState
/// -> MetadataLockClassState -> ValidatorLockClassState through inheritance
/// and picks the most derived one per feature level. Composition is this
/// repository's default; it also leaves nothing to downcast, where the chain
/// had every reader cast a LockClassId to whichever level it assumed.
class LockClassState {
    using Self = LockClassState;

  public:
    using AddResult = priv::AddResult;

    /// Constant-initialisable, so the per-class state in LockClass needs no
    /// static constructor to run before the first acquisition.
    constexpr LockClassState(char const *const name, ValidationOptions const options)
        : m_name(name), m_options(options) {}

    LockClassState(Self const &) = delete;
    auto operator=(Self const &) -> Self & = delete;

    [[nodiscard]]
    constexpr auto id() const -> LockClassId {
        return this;
    }

    /// The state behind a class id, or nullptr for INVALID_LOCK_CLASS_ID.
    static auto from_id(LockClassId const id) -> Self * {
        return const_cast<Self *>(id);
    }

    /// The name reports print: the tag of a declared lock, or the lock type
    /// for a bare one.
    [[nodiscard]]
    constexpr auto name() const -> char const * {
        return m_name;
    }

    [[nodiscard]]
    constexpr auto options() const -> ValidationOptions {
        return m_options;
    }

    /// Whether every bit in |options| is set for this class.
    [[nodiscard]]
    constexpr auto has(ValidationOptions const options) const -> bool {
        return (m_options & options) == options;
    }

    /// Whether any bit in |options| is set for this class.
    [[nodiscard]]
    constexpr auto has_any(ValidationOptions const options) const -> bool {
        return (m_options & options) != ValidationOptions::None;
    }

    /// Whether this class opted out of order tracking entirely.
    [[nodiscard]]
    constexpr auto tracking_disabled() const -> bool {
        return has(ValidationOptions::TrackingDisabled);
    }

    // The graph, forwarded to the node. Every call is a no-op where
    // validation is compiled out.

    static auto registry() -> Self * {
        return Graph::registry();
    }

    [[nodiscard]]
    auto next_class() const -> Self * {
        return m_graph.next_class();
    }

    auto ensure_registered() -> void {
        m_graph.ensure_registered(*this);
    }

    [[nodiscard]]
    static constexpr auto dependency_slots() -> usize {
        return Graph::dependency_slots();
    }

    [[nodiscard]]
    auto dependency_at(usize const slot) const -> Self * {
        return m_graph.dependency_at(slot);
    }

    [[nodiscard]]
    auto has_dependency(Self const *const dependency) const -> bool {
        return m_graph.has_dependency(dependency);
    }

    auto add_dependency(Self *const dependency) -> AddResult {
        return m_graph.add_dependency(dependency);
    }

  private:
    template <bool>
    friend class priv::LoopDetector;

    using Graph = IfLockValidationEnabled<priv::GraphNode, priv::NoGraphNode>;

    /// The node, typed by a template argument so that an expression reading
    /// the real node's scratch space depends on it -- and is left alone at a
    /// feature level where the node is the stand-in, instead of being checked
    /// against it.
    template <bool Enabled>
    auto graph_node() -> std::conditional_t<Enabled, priv::GraphNode, priv::NoGraphNode> & {
        return m_graph;
    }

    char const *m_name;
    ValidationOptions m_options;

    [[no_unique_address]]
    Graph m_graph;
};

namespace priv {
    /// Finds cycles in the lock-order graph with Tarjan's strongly-connected-
    /// components algorithm.
    ///
    /// Any component with more than one class is a cycle, and so is a class
    /// that depends on itself. The walk is iterative: the graph is built by
    /// arbitrary kernel code, so its depth is not something to spend stack
    /// on.
    ///
    /// A template only so that it is instantiated where validation is: its
    /// body reads the graph nodes' scratch space, which the stand-in node of
    /// the other feature levels does not have, and the member functions of an
    /// ordinary class are compiled whether used or not.
    template <bool Enabled>
    class LoopDetector {
        using Node = GraphNode::LoopNode;

      public:
        constexpr LoopDetector() = default;

        /// Runs one pass over the whole registry. Returns the number of
        /// cycles found, reporting each one.
        ///
        /// One pass at a time: the walk's scratch space lives in the nodes,
        /// so two passes running at once would overwrite each other's marks.
        auto run() -> usize {
            // The generation has to come from a counter shared by every
            // detector, not from this object. The marks it is compared
            // against live in the graph and outlast any one detector, so a
            // fresh detector starting over at one would read the previous
            // pass's marks as its own and skip the whole graph.
            m_generation = m_next_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
            m_index = 0;
            m_stack = nullptr;

            usize cycles = 0;
            for (auto *state = LockClassState::registry(); state != nullptr;
                 state = state->next_class()) {
                if (node_of(state).generation != m_generation) {
                    cycles += visit(state);
                }
            }
            return cycles;
        }

      private:
        static auto node_of(LockClassState *const state) -> Node & {
            return state->template graph_node<Enabled>().m_loop_node;
        }

        auto enter(LockClassState *const state) -> Node * {
            auto *const node = &node_of(state);
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

        /// Pops the component rooted at |root| and reports it once if it is a
        /// cycle, naming its root -- as Zircon's detector reports a connected
        /// set by its root rather than member by member.
        auto close(Node *const root) -> usize {
            usize members = 0;
            Node *member = nullptr;
            do {
                member = m_stack;
                m_stack = member->next;
                member->next = nullptr;
                member->on_stack = false;
                ++members;
            } while (member != root);

            // A single-node component is a cycle only when the class depends
            // on itself: one context taking two locks of that class in a
            // fixed order can meet another taking them in the other order.
            auto const cyclic = members > 1 || root->state->has_dependency(root->state);
            if (!cyclic) {
                return 0;
            }
            if (!root->state->has(ValidationOptions::ReportingDisabled)) {
                system_circular_lock_dependency_detected(root->state->id());
            }
            return 1;
        }

        auto visit(LockClassState *const root) -> usize {
            usize cycles = 0;

            auto *current = enter(root);
            current->parent = nullptr;

            while (current != nullptr) {
                auto *const state = current->state;
                if (current->cursor < LockClassState::dependency_slots()) {
                    auto *const successor = state->dependency_at(current->cursor);
                    ++current->cursor;
                    if (successor == nullptr) {
                        continue;
                    }

                    auto *const child = &node_of(successor);
                    if (child->generation != m_generation) {
                        // Descend.
                        auto *const entered = enter(successor);
                        entered->parent = current;
                        current = entered;
                    } else if (child->on_stack) {
                        // Back edge into the component being built.
                        if (child->index < current->least) {
                            current->least = child->index;
                        }
                    }
                    // Otherwise the successor is in a component already
                    // closed.
                    continue;
                }

                // Every successor visited: this node is done.
                if (current->least == current->index) {
                    cycles += close(current);
                }

                auto *const parent = current->parent;
                if (parent != nullptr && current->least < parent->least) {
                    parent->least = current->least;
                }
                current = parent;
            }

            return cycles;
        }

        /// Shared by every detector so that no pass ever reuses the number a
        /// previous pass stamped into the graph. Node marks start at zero, so
        /// the first pass is generation one.
        static inline std::atomic<usize> m_next_generation{0};

        usize m_generation{0};
        usize m_index{0};

        Node *m_stack{nullptr};
    };

} // namespace priv

/// Walks the lock-order graph once and reports every cycle it finds, through
/// system_circular_lock_dependency_detected(). Zircon's LoopDetectionPass().
///
/// The environment runs it in response to system_trigger_loop_detection(), on
/// a context of its own and never concurrently with itself. Returns the number
/// of cycles found, so a test or a boot-time check can assert on it.
///
/// A template on the feature level for the same reason LoopDetector is: the
/// validation branch must not be instantiated where there is no graph.
template <bool Enabled = LOCK_VALIDATION_ENABLED>
auto loop_detection_pass() -> usize {
    if constexpr (Enabled) {
        priv::LoopDetector<Enabled> detector;
        return detector.run();
    } else {
        return 0;
    }
}

/// The options of the class behind |id|, as merged from its declaration and
/// its lock type; None where there is no metadata to read them from.
inline auto options_of(LockClassId const id) -> ValidationOptions {
    if (id == INVALID_LOCK_CLASS_ID) {
        return ValidationOptions::None;
    }
    return id->options();
}

/// The name of the class behind |id|; empty where there is no metadata.
inline auto name_of(LockClassId const id) -> char const * {
    if (id == INVALID_LOCK_CLASS_ID) {
        return "";
    }
    return id->name();
}

} // namespace lockdep

#endif // #ifndef LOCKDEP_LOCK_CLASS_STATE_HPP
