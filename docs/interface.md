# lockdep interface

This is the public interface lockdep should expose, modeled on Zircon's
lockdep (`fuchsia/src/lib/lockdep/include/lockdep/`, kernel glue in
`fuchsia/zircon/kernel/lib/lockdep/lock_dep.cc`). The current headers predate
it and are to be reshaped to match.

Three choices below were put to the maintainer and settled rather than
inferred: a lock's class is named by a tag type, not by Zircon's `__LINE__`
macro; `Guard` accepts only `lockdep::Lock<T>`, with no transitional overload
for raw locks; and releasing a guard early restores what its policy saved at
that point, as Zircon's `Guard::Release()` does, not at the end of the scope.

Where the design keeps Zircon's shape the Zircon file is cited so the two can
be compared. Where it departs, the reason is given — usually one of this
repository's own rules: composition over inheritance, no macros around what the
language can spell, `UPPER_CASE` constants, `lower_case` functions, underscore
file names, and `std::` facilities over ustl wrappers.

## What the current interface gets wrong

1. **A lock has no class of its own.** `Guard{lock}` takes a raw `SpinLock&`,
   so the class can only be derived from the lock *type*. Every `SpinLock` in
   the kernel is then one class, and taking the pi lock and then a run queue
   lock — two different spinlocks, a perfectly ordinary nesting — is reported
   as re-acquiring the same lock. Turning validation on would bury real reports
   under that noise. Zircon gives every lock *declaration* its own class
   (`LOCK_DEP_INSTRUMENT` in `lock_traits.h`, keyed on the containing type and
   `__LINE__`); ours has to as well.
2. **State savers only half describe a lock.** A saver brackets the
   acquisition but cannot own it, so there is no place for try-lock, for
   release arguments, or for the one ordering constraint that matters: an
   irq-safe lock must disable interrupts *before* lockdep records the
   acquisition, or an interrupt landing in between sees the lock as already
   held. Zircon's `LockPolicy` (`lock_policy.h`) owns all four steps; savers
   are a strict subset of it.
3. **`ScopedGuard` duplicates `Guard`** and cannot carry an option. Zircon has
   no such type: different classes are nested `Guard`s, and several locks of
   one class go through `GuardMultiple`, which orders them by address.
4. **One context per thread is not enough.** A lock taken from interrupt
   context must not land on the interrupted thread's held list. Zircon selects
   the list by the lock's flags (`SystemGetThreadLockState(LockFlags)`: per-CPU
   for irq-safe locks, per-thread otherwise).

## Headers

| Header | Zircon counterpart | Holds |
| --- | --- | --- |
| `lockdep/config.hpp` | `common.h` | feature level, limits |
| `lockdep/types.hpp` | `common.h` | `LockClassId`, `LockResult` |
| `lockdep/lock_traits.hpp` | `lock_traits.h` | `ValidationOptions`, `lockdep_traits` lookup |
| `lockdep/lock_policy.hpp` | `lock_policy.h` | option tags, the `LockPolicy` concept, `lockdep_policy` lookup |
| `lockdep/lock_class.hpp` | `lock_class.h` | `lock_class<Tag>()`, `Lock<T>` |
| `lockdep/guard.hpp` | `guard.h` | `Guard`, `NullGuard`, `assert_held` |
| `lockdep/guard_multiple.hpp` | `guard_multiple.h` | `GuardMultiple` |
| `lockdep/thread_lock_state.hpp` | `thread_lock_state.h` | `AcquiredLockEntry`, `ThreadLockState`, `assert_no_locks_held` |
| `lockdep/lock_class_state.hpp` | `lock_class_state.h` | the graph node and `loop_detection_pass()` |
| `lockdep/runtime_api.hpp` | `runtime_api.h` | what the environment must define |
| `lockdep/lockdep.hpp` | `lockdep.h` | umbrella |

`LockContext` becomes `ThreadLockState`: it is exactly Zircon's type, and the
old name hid that one instance can belong to a CPU rather than a thread.

## Configuration — `config.hpp`

```cpp
enum class FeatureLevel : u8 { Disabled = 0, MetadataOnly = 1, Validation = 2 };

constexpr auto ENABLED_FEATURE_LEVEL = static_cast<FeatureLevel>(LOCK_DEP_ENABLED_FEATURE_LEVEL);
constexpr auto LOCK_METADATA_AVAILABLE = ...;
constexpr auto LOCK_VALIDATION_ENABLED = ...;
constexpr auto MAX_LOCK_DEPENDENCIES = ...;   // from LOCK_DEP_MAX_DEPENDENCIES

using LockClassId = LockClassState const *;
constexpr LockClassId INVALID_LOCK_CLASS_ID = nullptr;

enum class LockResult : u8 {
    Success,
    AlreadyAcquired,     // same class twice, class is not nestable
    OutOfOrder,          // the reverse edge is already in the graph
    InvalidNesting,      // nestable class taken with a non-increasing order
    InvalidIrqSafety,    // irq-unsafe lock taken while holding an irq-safe one
    Reentrance,          // the very same lock instance taken twice
    ShouldNotHold,       // assert_no_locks_held() with locks held
    AcquireAfterLeaf,
    MaxLockDependencies, // non-fatal: the dependency set is full
};
auto to_string(LockResult) -> char const *;
```

Same as Zircon except naming. Zircon's `DependencyExists` is an internal
return value of the dependency set and stays internal here.

## Validation options — `lock_traits.hpp`

Zircon calls this set `LockFlags`. Here it is `ValidationOptions`: the bits
tell the validator which rules to apply to a lock class, and this repository
names a bitmask of that kind `*Options` (as with the scheduler's
`EnqueueOptions`). It used to be `ValidationControl`, which named a controller
rather than a set of options.

```cpp
enum class ValidationOptions : u16 {
    None = 0,
    IrqSafe            = 1 << 0,
    Nestable           = 1 << 1,
    MultiAcquire       = 1 << 2,
    Leaf               = 1 << 3,
    ReportingDisabled  = 1 << 4,
    SingletonLock      = 1 << 5,
    ReAcquireFatal     = 1 << 6,
    ActiveListDisabled = 1 << 7,
    TrackingDisabled   = 1 << 8,
};
```

A lock *type* declares its default flags next to its own definition, with a
function declaration that ADL finds from inside lockdep:

```cpp
namespace ours::sync {
    class SpinLock { ... };
    auto lockdep_traits(SpinLock *) -> lockdep::Traits<lockdep::ValidationOptions::IrqSafe>;
}
```

Zircon wraps this in `LOCK_DEP_TRAITS()`. The macro hides nothing but a
declaration the language already spells, so here it is written out. A type with
no declaration gets `ValidationOptions::None`, as in Zircon.

`Traits<Flags>` rejects `MultiAcquire | Nestable` and
`MultiAcquire | ReAcquireFatal` at compile time, like Zircon's
`DefaultLockFlags`.

## Lock policies — `lock_policy.hpp`

A policy says how to take and drop one lock type under one option, and what
state the guard must keep in between. It replaces `LOCKDEP_STATE_SAVER`.

```cpp
template <typename P, typename LockType>
concept LockPolicy = requires(LockType &lock, typename P::State &state) {
    typename P::State;
    typename P::ValidationGuard;                             // RAII around every held-list update
    { P::pre_validate(lock, state) } -> std::same_as<void>;  // before lockdep records the acquisition
    { P::acquire(lock, state) }      -> std::same_as<bool>;  // false: try-lock failed
    { P::release(lock, state) }      -> std::same_as<void>;
};
// Optional: static auto assert_held(LockType const &) -> void;
```

`ValidationGuard` is constructed around each update of the held list. For a
lock whose policy already runs with interrupts off (a spinlock under
`IrqSave`) it is `NullValidationGuard`. For one that does not — a mutex — it
must disable interrupts for the few instructions the list is being relinked,
or an interrupt that takes an irq-safe lock can observe the list half-updated.
That is what Zircon's `LockValidationGuard`
(`kernel/include/kernel/lock_validation_guard.h`) does for `Mutex` and
`BrwLock`.

The mapping from (lock type, option) to a policy is again a declaration next to
the lock type:

```cpp
namespace ours::sync {
    auto lockdep_policy(SpinLock *, lockdep::NoIrqSave *) -> NoIrqSavePolicy;  // Guard<SpinLock, NoIrqSave>
    auto lockdep_policy(SpinLock *, lockdep::IrqSave *)   -> IrqSavePolicy;    // Guard<SpinLock, IrqSave>
}
```

`Guard<Lock>` looks up the option `NoOption`. Zircon uses `void` there; a
`void *` parameter, though, would also accept every option tag the lock
declares nothing for, through the implicit `T *` to `void *` conversion, and
quietly resolve it to the option-less policy instead of failing to compile.

A lock with no safe way to be held without an option says so with
`auto lockdep_option_required(Lock *) -> void;`, and then has no option-less
policy: `Guard<SpinLock>` does not compile, and every holder names what it
saves, as with Zircon's `LOCK_DEP_POLICY_OPTION`. It was an option-less
`Guard guard{spinlock}` that held a PmZone lock with interrupts and preemption
both enabled. `NoIrqSave` is for a caller that already has interrupts off and
says so, as in Zircon, where `SpinLock::Acquire()` asserts it.

Otherwise a lock that declares no option-less policy gets `DefaultLockPolicy`,
which calls `acquire()` and `release()` and nothing else. Zircon's kernel build
has no default at all; here the kernel's mutexes are guarded without an option
and rely on it, and the lock that must not have a default is the one that says
so.

Option tags are empty types. lockdep owns the common ones -- `IrqSave`,
`PreemptDisable`, `IrqSaveAndPreemptDisable` -- and a lock may define its own
for a policy only it has, such as a try-lock. A look-alike declared elsewhere,
which is what `ours::IrqSave` once was, finds no policy and fails to compile.

**State savers keep working.** A lock that declares a saver through
`LOCKDEP_STATE_SAVER` rather than a policy is held through `SaverPolicy`, which
constructs the saver in `pre_validate()` and destroys it in `release()`. A
declared policy wins where a lock has both. The savers are a transition, not a
second interface: nothing new should be declared through them.

## Lock classes — `lock_class.hpp`

A lock class belongs to a **(tag, lock type)** pair. The declaration names the
tag, and may add options; the lock type contributes its own options, the ones
every lock of its kind has — every `SpinLock` is `IrqSafe`. The class's options
are the union of the two, and only the place where both are known can make it:
the `Lock<T>` constructor. So the declaration passes a name, not an id:

```cpp
template <typename Tag, ValidationOptions Options = ValidationOptions::None>
constexpr auto lock_class() -> LockClassName<Tag, Options>;   // an empty token
```

and `Lock<T>` turns the token into the class of `(Tag, T)`, whose state object
exists once per instantiation and whose address is the id, exactly like
Zircon's `LockClassStateStorage::lock_class_state_`. Zircon keys `LockClass`
on the lock type for the same reason. The merged options are checked there
too: `MultiAcquire` excludes `Nestable` and `ReAcquireFatal`, as in Zircon's
`DefaultLockFlags`.

An earlier draft had the declaration call `class_id<Tag>()` and hand the
resulting id to the lock. That id could only carry the tag's options, so every
`SpinLock` declaration would have had to repeat `IrqSafe` by hand, and one tag
used on two lock types would silently have been one class.

A lock is the underlying lock **plus** its class id — composition, no base
class:

```cpp
template <typename LockType>
class TA_CAPABILITY("lock") Lock {
  public:
    template <typename Tag, ValidationOptions Options, typename... Args>
    constexpr explicit Lock(LockClassName<Tag, Options>, Args &&...args);

    auto lock() -> LockType &;                  // TA_RETURN_CAPABILITY(*this)
    auto lock() const -> LockType const &;
    constexpr auto id() const -> LockClassId;   // INVALID_LOCK_CLASS_ID when metadata is off

    Lock(Lock const &) = delete;
    auto operator=(Lock const &) -> Lock & = delete;

  private:
    LockType m_lock;
    [[no_unique_address]] ClassIdHolder m_id;   // empty when metadata is off
};
```

Declaring a lock names its class at the declaration:

```cpp
class Thread {
    lockdep::Lock<sync::SpinLock> m_pi_lock{lockdep::lock_class<struct ThreadPiLock>()};
    lockdep::Lock<sync::SpinLock> m_task_lock{
        lockdep::lock_class<struct ThreadTaskLock, lockdep::ValidationOptions::Leaf>()};
};
```

`lock_class` is a function rather than a variable template only because a
constexpr variable is named in `UPPER_CASE` here.

How this differs from Zircon, and why:

- Zircon makes the class part of the lock's *type*,
  `LockDep<Class, LockType, Index> : Lock<LockType>`, and needs the base class
  to erase it again so `get_lock()` can return `Lock<SpinLock>*`. Carrying the
  id as a member instead makes `Lock<SpinLock>` the one type from the start —
  no inheritance, nothing to erase. Zircon stores the id in the lock anyway
  (`AvailableState::id_`), so this costs nothing it did not already pay.
- Zircon distinguishes two locks of one type in one class with `__LINE__`
  inside `LOCK_DEP_INSTRUMENT`. A tag type does the same job in standard
  syntax, and its name is what a report prints — `ThreadPiLock` reads better
  than `Thread:142`.

The `Lock` is the capability the thread-safety analysis sees: members are
`TA_GUARDED(m_lock)` with `m_lock` the `Lock`, and `lock()` returns the inner
lock as that same capability, so taking the inner lock through it balances.

Global locks are plain `Lock<T>` objects at namespace scope. When the inner
lock is constant-constructible they are `constinit`, class id included, which
keeps them out of static-initialisation order. Zircon's `SingletonLockDep`
adds a `Get()` accessor and the `SingletonLock` option on top of that; nothing
here needs either yet, so it is left out until something does, along with
`LOCK_DEP_SINGLETON_LOCK_WRAPPER`.

## Guards — `guard.hpp`

```cpp
template <typename LockType, typename Option = NoOption>
class Guard {
  public:
    explicit Guard(Lock<LockType> &lock);       // recorded under the declared class
    Guard(LockType &lock);                      // transitional: DefaultLockClass<LockType>
    Guard(Lock<LockType> &lock, Option);        // tag-object form, Guard guard{lock, IRQ_SAVE}

    ~Guard();

    auto unlock() -> void;                      // Zircon's Release()
    auto unguard() -> void;                     // hand the lock off without releasing it
    auto call_unlocked(auto &&op) -> void;
    auto call_untracked(auto &&op) -> void;

    auto held() const -> bool;
    explicit operator bool() const;             // false after unlock or a failed try-lock

    Guard(Guard const &) = delete;
    auto operator=(Guard const &) -> Guard & = delete;
};

// On Lock<LockType>, for a lock type that can check it:
auto Lock<LockType>::assert_held() const -> void;   // TA_ASSERT()
```

`assert_held()` is a member of `Lock<T>` rather than Zircon's free
`AssertHeld()`: the analysis attribute has to name what it asserts, and with
trailing return types only a member can name its object (`TA_ASSERT()`, meaning
`*this`) in the leading position left to it. The inner lock's own
`assert_held()` does the runtime check; a lock type without one cannot be
asserted on.

Still to come, after `ThreadLockState` (they depend on it):

- `take()` and `Guard(AdoptLock, Lock<LockType> &, Adoptable &&)`, which move a
  held lock's record to another scope. That means replacing a node in the
  held-lock list in place, O(1) only once the list is doubly linked.
- The nestable constructor `Guard(Lock<LockType> &, usize order)`, whose order
  the validator has to compare against the held entry of the same class.

The acquire sequence is Zircon's `ValidateAndAcquire`, and its order is the
point of the policy split:

1. `policy::pre_validate` — e.g. disable interrupts;
2. record the acquisition in `ThreadLockState` (validation happens here);
3. `policy::acquire` — if it returns false, undo step 2.

`release` does the reverse, un-recording **before** `policy::release`, for the
reason Zircon documents on `Guard::Release`: releasing may re-enable
interrupts, and an interrupt taking the same lock before the bookkeeping is
cleared would be reported as re-entrance.

Differences from Zircon: locks are passed by reference, since a guard of a null
lock is never meaningful; the shared-mode specialisation, `AliasedLock` and
`AssertOrderedLock` are left out until a reader-writer lock or type-erased
nestable lock needs them.

## Several locks of one class — `guard_multiple.hpp`

```cpp
template <usize N, typename LockType, typename Option = NoOption>
class GuardMultiple {
  public:
    explicit GuardMultiple(std::array<Lock<LockType> *, N> locks);  // all of one class
    auto release() -> void;
    explicit operator bool() const;
};
```

Takes the locks in address order, so any two sites locking the same set agree
on the order. Replaces `ScopedGuard`; locks of *different* classes are simply
nested `Guard`s, whose order is exactly what lockdep is there to check.

It lands after `ThreadLockState`. Two locks of one class held together read as
a re-acquisition unless each record carries an order -- the address, here --
and the validator checks that the orders rise, as Zircon's
`ThreadLockState::Acquire()` does with `AcquiredLockEntry::order()`.

## Per-context state — `thread_lock_state.hpp`

```cpp
class AcquiredLockEntry {
  public:
    explicit AcquiredLockEntry(void const *address, LockClassId id, usize order = 0);
    auto address() const -> void const *;
    auto id() const -> LockClassId;
    auto order() const -> usize;
    auto is_linked() const -> bool;
  private:
    ...
    ustl::collections::intrusive::ListMemberHook<> m_hook;   // member, not a base
};

class ThreadLockState {
  public:
    auto acquire(AcquiredLockEntry &) -> void;   // validate, record edges, list
    auto release(AcquiredLockEntry &) -> void;
    auto assert_no_locks_held() -> void;
    auto empty() const -> bool;
    auto last_result() const -> LockResult;     // for tests
    auto reporting_disabled() const -> bool;
};

auto assert_no_locks_held() -> void;

using LockContext = /* ThreadLockState, or an empty stand-in without validation */;
```

The checks inside `acquire` are Zircon's `ThreadLockState::Acquire()`, in its
order, for each held entry:

1. the held lock is a `Leaf` — `AcquireAfterLeaf`, fatal;
2. the held lock is of the same class — the same address is `Reentrance`;
   otherwise, unless the class is `MultiAcquire`, the new order must rise above
   the held one, and failing that a non-`Nestable` class at order zero is
   `AlreadyAcquired` (fatal if `ReAcquireFatal`) and anything else
   `InvalidNesting`;
3. otherwise the pair is an edge held → new. Only a **new** edge is checked --
   `InvalidIrqSafety` if the held lock is irq-safe and the new one is not,
   `OutOfOrder` if the reverse edge is in the graph -- and only a new edge
   triggers loop detection. An edge already in the graph was checked when it
   went in, so an ordering that recurs is reported once rather than at every
   acquisition.

`ReportingDisabled` is a counter: held `ReportingDisabled` locks, plus one for
the duration of every report and trigger, so a reporter that takes a lock -- a
console lock, say -- cannot recurse into another report. `ActiveListDisabled`
validates but does not keep the entry; `TrackingDisabled` skips the lock
entirely.

The held list is doubly linked, like Zircon's `fbl::DoublyLinkedList`:
out-of-order release is O(1), and so will be moving an entry to another frame
once `take()` needs it. The hook's `SafeLink` mode traps an entry destroyed
while still listed, which is Zircon's `!InContainer()` assertion.

`LockContext` is the name the kernel's `Thread` member was declared with; it
stays as the alias that collapses to an empty type without validation.

## Graph — `lock_class_state.hpp`

```cpp
class ValidatorLockClassState {
  public:
    auto has_dependency(Self const *) const -> bool;
    auto add_dependency(Self *) -> AddResult;   // Added, Present, Full
    ...
};

auto options_of(LockClassId) -> ValidationOptions;
auto loop_detection_pass() -> usize;             // cycles found, each reported once
```

An edge held → new is stored in the held class's set, its successors; Zircon
stores the same edge in the new class's set, its predecessors. Either works;
what matters is that `OutOfOrder` asks for the reverse edge in the matching set.
`add_dependency` tells a new edge from a known one, which is what lets the
validator check and trigger only on new edges -- Zircon's `AddLockClass()`
returning `Success` or `DependencyExists`.

`loop_detection_pass()` is Tarjan's strongly-connected-components walk,
iterative, with its scratch state in the nodes. It reports each cycle once,
through `system_circular_lock_dependency_detected()`, naming the component's
root as Zircon's detector does.

Still to come: the metadata and validator parts as members selected by the
feature level rather than Zircon's inheritance chain, and the dependency set
as Zircon's lock-free open-addressed hash set (`lock_dependency_set.h`) instead
of a linear array.

## Environment — `runtime_api.hpp`

Declared by lockdep, defined by the kernel; referenced only when validation is
enabled.

```cpp
auto system_get_thread_lock_state(ValidationOptions options) -> ThreadLockState *;
auto system_lock_validation_error(AcquiredLockEntry const &entry,
                                  AcquiredLockEntry const *conflicting,
                                  ThreadLockState const &state, LockResult result) -> void;
auto system_lock_validation_fatal(AcquiredLockEntry const &entry,
                                  ThreadLockState const &state, LockResult result) -> void;
auto system_circular_lock_dependency_detected(LockClassId root) -> void;
auto system_trigger_loop_detection() -> void;
```

`system_get_thread_lock_state` receives the lock's options so the kernel can
return the per-CPU state for irq-safe locks and the per-thread state otherwise,
as Zircon's `lock_dep.cc` does: an irq-safe lock can be taken in interrupt
context, where the running thread is only the interrupted one. It may return
nullptr before there is any context, and the acquisition then goes unrecorded.
`system_trigger_loop_detection` only takes note; the environment runs
`loop_detection_pass()` later on a context of its own -- detection never runs
on the acquire path.

Compared with Zircon: no caller address or frame is passed, since the
environment's reporter sits in the same call chain and can take a backtrace
itself; `system_init_thread_lock_state` is left out, as a default-constructed
`ThreadLockState` is ready to use.

## What changes for the kernel

- Lock members become `lockdep::Lock<T>` with a class tag. There is no raw-lock
  overload of `Guard`: keeping one would keep the one-class-per-type noise.
- `LOCKDEP_STATE_SAVER` declarations become `lockdep_policy` declarations with
  policies modeled on Zircon's `kernel/include/kernel/spinlock.h`
  (`NoIrqSavePolicy`, `IrqSavePolicy`, the try-lock variants).
- `Thread` holds a `ThreadLockState`; each CPU's local data holds another.
- `ScopedGuard` call sites become nested `Guard`s.

## Layout

Per this repository's library shape: `include/lockdep/`, `src/`, `test/`,
`.gitignore`, `.clang-format`. The tests are meant to be built by GN in the
end; until lockdep has a GN test target, `test/run_tests.py` drives them.
