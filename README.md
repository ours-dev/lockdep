# lockdep

Lock ordering validation for the ours kernel: a header-only library that records
which locks are held when another is taken, and reports orderings that can
deadlock.

## What it does

Every lock type belongs to a **lock class**. When a context takes a lock of
class `B` while already holding one of class `A`, lockdep records the edge
`A -> B`. A cycle in that graph is an ordering that two contexts can run in
opposite directions, which deadlocks as soon as they interleave badly — whether
or not it has ever actually deadlocked.

Work is split by cost:

- **On every acquisition** (`ThreadLockState::acquire`) lockdep records the
  edges and checks what is cheap to see from there: a direct inversion of an
  ordering already recorded, a class acquired twice without `Nestable`, and
  anything taken while a `Leaf` lock is held. This is O(locks currently held)
  and allocates nothing.
- **On demand** (`loop_detection_pass()`) it walks the whole graph with
  Tarjan's algorithm and reports every cycle, including transitive ones like
  `A -> B -> C -> A` that no single acquisition can see. This is
  O(classes + edges) and belongs on an idle thread or in a test, not on the
  acquire path.

## Using it

```cpp
#include <ours/lockdep.hpp>

// The option selects the lock's policy, and the policy disables interrupts
// before the lock is taken and restores them when it is released.
Guard<SpinLock, IrqSave> guard{lock};
```

A lock type says how it is held, one policy per option, next to its own
declaration -- declared, never defined, found by argument-dependent lookup:

```cpp
auto lockdep_policy(MyLock *, lockdep::IrqSave *) -> MyIrqSavePolicy;
LOCK_DEP_TRAITS(MyLock, lockdep::ValidationOptions::IrqSafe);
```

A policy (`include/lockdep/lock_policy.hpp`) provides a `State`, a
`ValidationGuard`, and `pre_validate()`, `acquire()` returning bool, and
`release()`. The guard calls them in Zircon's order: prepare, record the
acquisition, acquire; and on release, withdraw the record, then release.
Releasing early restores what the policy saved at that point.

A lock that declares no option-less policy is held by `DefaultLockPolicy`,
which just calls `acquire()` and `release()` -- unless it has no safe
option-less form, and says so:

```cpp
auto lockdep_option_required(MyLock *) -> void;   // declared, never defined
```

`Guard<MyLock>` (which is what `Guard guard{lock}` deduces to) then fails to
compile, and every call site has to name the state it saves. That is Zircon's
`LOCK_DEP_POLICY_OPTION`, which leaves the lock's option-less policy ambiguous
for the same reason. Asking for an option the lock declares nothing for does
not compile either: the only other answer -- save nothing -- is the bug that
left every `Guard<SpinLock, IrqSave>` running with interrupts enabled.

Locks declared against the older state-saver interface
(`LOCKDEP_STATE_SAVER`) keep working: each saver is presented as a policy that
constructs it before the acquisition is recorded and destroys it on release.
Nothing new should be declared that way.

Traits are optional: a lock that declares none gets `ValidationOptions::None`.

## Feature levels

The level comes from the build, never from the source. `config("lockdep")` in
`//kernel/products/BUILD.gn` turns the `lockdep_*` GN arguments into
`CONFIG_LOCK_DEP` and `LOCK_DEP_ENABLED_FEATURE_LEVEL`:

| `lockdep_enable` | Level | What is compiled in |
| --- | --- | --- |
| unset | `Disabled` | Nothing. `Guard` is the lock reference and a flag; `LockContext` is empty. |
| `lockdep_metadata_only` | `MetadataOnly` | Lock classes, with their names and options; no tracking. |
| `true` | `Validation` | The graph, the acquire-time checks, and the detector. |

At `Disabled` every lockdep member vanishes — `Guard` costs no more than a hand
written scope guard, and `Thread` carries no lock context.

## What the environment must supply

Five functions, declared in `include/lockdep/runtime_api.hpp` and referenced
only when validation is enabled, so a build with `lockdep_enable=false` links
without them:

- `system_get_thread_lock_state(options)` -- the held-lock state an acquisition
  belongs on: the CPU's for an irq-safe lock, the running thread's otherwise.
- `system_lock_validation_error()` and `system_lock_validation_fatal()` -- how
  this kernel logs or panics.
- `system_circular_lock_dependency_detected()` -- a cycle found by
  `loop_detection_pass()`.
- `system_trigger_loop_detection()` -- a new ordering edge; arrange for
  `loop_detection_pass()` to run later, off the acquire path.

## Tests

```sh
test/run_tests.py                    # everything
test/run_tests.py --suite smoke      # logic, at all three feature levels
test/run_tests.py --suite threaded   # real threads, plain and under the sanitizers
```

The headers lean on the ours tree -- the force-included prelude for `usize`,
ustl, and the thread-safety macros in `system/public` -- so the script finds
the ours checkout above this library, or takes one with `--ours`.

Lock validation turns out to be well suited to a host test, because everything
lockdep concludes is a property of a graph, and a graph does not care whether
the locks that built it were spinlocks or `std::mutex`. `test/smoke.cpp` covers
that logic at every feature level with `-Wall -Wextra -Werror`, using fake locks:
that a consistent order stays clean, that an inversion is caught at acquire time,
that a three-node cycle is invisible to the acquire path but found by the
detector, that repeated passes agree, and that a guard costs nothing extra when
validation is compiled out.

`test/threaded.cpp` drives it the way a kernel does — one `LockContext` per
thread, real locks, several threads recording into the shared graph at once —
and checks that two threads taking the same pair of locks in opposite orders are
reported even though the run never actually deadlocks. Its ThreadSanitizer
variant is what says the lock-free registry and the dependency set are safe to
record into from several CPUs.

What no host test can cover is the arch integration: interrupt and preemption
state savers have no userspace meaning, so `Guard<SpinLock, IrqSave>` is only
checked here for saver *ordering* against fakes, never against real hardware
state.

## Known limits

- A lock declared as `lockdep::Lock<SpinLock> m_lock{lockdep::lock_class<struct MyLock>()}`
  has a class of its own, and a `Guard` over it records it under that class. A
  bare lock -- every lock in the kernel today -- still falls back to
  `DefaultLockClass<Lock>`, shared by every lock of its type, so two different
  bare spinlocks still look like one class to the validator.
- `LOCKDEP_MAX_DEPENDENCIES` (default 32, raised to the prime 97) bounds the
  successors one class can record. Overflow is reported as
  `MaxLockDependencies` and costs completeness, not soundness.
- `ValidationOptions::IrqSafe`, `SingletonLock` and `ActiveListDisabled` are
  carried on the class but not yet acted on.
- Only one detector pass may run at a time; the walk keeps its marks in the
  graph nodes.
