<!-- from workflow w013b9pqt; verify tree-state claims below -->

> **AMENDMENT (2026-07-16 — supersedes the deferral in §0/Design below):**
> Verified on THIS tree, two facts change the phasing:
> 1. The `#1495` deferred-wake machinery (`wake_handoff` / `REMOTE_RESCHEDULE_PENDING` /
>    `clear_prev_task_on_cpu`) **IS present** — the plan wrongly assumed it absent. The safety
>    objection that pushed wake-redirect to a gated Phase 3 no longer holds.
> 2. Per project direction, **wake-redirect is NOT deferred** — it moves into Phase 2, because it
>    is the mechanism that captures the wake-heavy / latency workloads (mutex, threads, and the
>    RKNN inference pipeline) that idle-pull structurally cannot.
>
> **Revised phasing:**
> - **Phase 1 — idle-pull balancer** (unchanged): throughput base 159→~2046, zero wake-path risk
>   (moves only *Ready* tasks). Cherry-pick `.claude/sched-loadbalance.patch` (~90% done, tested).
> - **Phase 2 — enqueue-time placement (spawn AND wake)**: on spawn or wake, pick the best CPU
>   (prefer an idle big/A76 core) instead of the last/waker core. Subsumes the plan's (d)
>   spawn-placement + (d′) wake-redirect; captures the placement lever (2.2×) AND the wake-heavy
>   paths. Built on the present deferred-wake cross-core enqueue+kick.
>   **Correctness gates (mandatory — this is the live wake path):**
>   (a) use the deferred-wake enqueue+kick, NEVER the old `on_cpu` busy-spin cross-core;
>   (b) target is ALWAYS within the task cpumask (affinity authoritative);
>   (c) never redirect a task still `on_cpu` on its old core — let deferred-wake sequence it;
>   (d) wake-affinity bias + hysteresis to avoid ping-pong / IPI storms (à la Linux
>       `select_idle_sibling`: keep the cache-hot waker core unless a clearly-better idle big
>       core is free);
>   (e) consistent src/dst run-queue + task lock ordering.
> A focused adversarial correctness pass on Phase 2 runs before any code.

# big.LITTLE-aware load-balancing scheduler for axtask — phased implementation plan

## 0. Ground-truth corrections (verified against `feat/rknn-profiling-harness`, not upstream)

Before the plan, three corrections that change the design — all verified by reading the code, not the scout notes:

1. **The `wake_handoff` / deferred-wake / `REMOTE_RESCHEDULE_PENDING` / `force_kick` machinery does NOT exist on this branch.** `grep` returns 0 hits across `os/arceos/modules/axtask/src/`. Investigator #2's entire "SeqCst Dekker handshake / deferred cross-core wake (#1495)" description is a *different tree* (upstream/dev). **On this branch the only SMP-wake invariant is the `on_cpu: AtomicBool` + the busy-spin `while task.on_cpu() { spin_loop() }` at `run_queue.rs:633`** inside `put_task_with_state`, plus `PREV_TASK`/`clear_prev_task_on_cpu` (`run_queue.rs:805-812`). Consequence: **redirecting wakeups cross-core is a *live* deadlock surface here, not a solved one** — see §Correctness Gates. This is why wake-redirect is pushed to Phase 3, gated.

2. **A prototype already exists and encodes ~90% of this correctly**: `.claude/sched-loadbalance.patch` (31 KB) — but it **no longer applies cleanly** (`git apply --check` fails on `os/StarryOS/kernel/Cargo.toml:22` drift). Treat it as a *reference to cherry-pick hunks from*, not a drop-in. Its axsched/axhal/axtask hunks are still valid; only the Cargo.toml feature-wiring drifted.

3. **Line numbers**: investigator #1 and #4 are accurate for this branch; #2's are for the other tree. Authoritative anchors below are all re-verified.

Topology (confirmed in `scripts/profile/RUN_LOG.md:114` and the board DTS): **cpu0-3 = Cortex-A55 (little), cpu4-7 = Cortex-A76 (big)**, `max_cpu_num=8`. `sched_setaffinity`/`taskset` already honored end-to-end (RUN_LOG:1261). Harness is `apps/OScope-harness/harness.py` (execs a prebuilt `tools/starry-syscall-harness/harness.py`); the task's `apps/starry/sysbench-board/harness/` path does not exist.

---

## Design decision (which mechanism, and why staged)

Three candidate mechanisms, evaluated against the measured workload (N long-running, rarely-blocking compute threads = `sysbench --cpu --threads=N`):

| Mechanism | Fixes | Hot-path cost | Correctness risk on THIS branch |
|---|---|---|---|
| **(a) idle-pull / newidle** | spawn pile-up AND wake re-collapse; converges to 1-thread/core | **zero** (only fires when a core would otherwise idle) | **lowest** — only moves *Ready* tasks (never `on_cpu`), one lock at a time |
| (d) placement-at-spawn | spawn pile-up; lands lone task on A76 | one scan per spawn | low — fresh tasks are never `on_cpu`; touches `select_run_queue` only |
| (d′) placement-at-wake | barrier/producer-consumer locality | one scan per wake | **HIGH here** — drives `put_task_with_state`'s `on_cpu` spin cross-core → the unfixed #1495 mutual-wake deadlock |
| (b) periodic tick-balance | imbalance when *no* core is idle (10 threads/8 cores) | **every tick, every core** | medium — global blast radius; needs hysteresis + feature gate |

**Chosen: staged (a) → (d) → (b)/(d′).** This is the CFS decomposition (`newidle_balance` + `task_placement` + periodic `load_balance`) adapted to a per-CPU `BaseScheduler`. Rationale for the ordering:

- **(a) alone captures the throughput ceiling** for the measured workload. `sysbench --cpu` threads never block, so once idle-pull spreads them one-per-core at startup they *stay* spread — no wake path involved. This gets `--threads=8` from ~160 → ~2046 with the least code and the least risk.
- **(d) is required for the *placement* lever** (lone task → A76), which idle-pull *cannot* do: a single running task is `on_cpu` on its A55 and sits in no ready queue, so no idle A76 can pull it. Only spawn-time placement moves it. Hence Phase 2.
- **(d′)/(b) are deferred**: (d′) is unsafe until the deferred-wake fix is ported; (b) has global cost and only matters for uneven counts idle-pull can't see.

---

## PHASE 1 — Work-stealing idle-pull balancer (biggest throughput win, lowest risk)

**Goal / rung:** unpinned `sysbench --cpu --threads=8` rises from ~159 ev/s (one A55) toward the **~2046 ev/s** @800MHz aggregate ceiling. Also: 4 threads masked `{4-7}` rise from 351 (1× A76) → ~1400 (4× A76). No change to any existing hot path; `select_run_queue`/`select_wake_run_queue` are **untouched** in this phase.

### Prerequisite: a load signal (nothing can measure imbalance today)

`BaseScheduler` (`components/axsched/src/lib.rs:21-61`) has no `len()`, and the intrusive `List` ready queue has no length. Add both (the prototype already does this correctly and ships passing tests — `test_len`, `test_steal`):

- **`components/linked_list_r4l/src/raw_list.rs`**: add a `len: usize` field to `RawList`, incremented at the two insert chokepoints (`push_back_internal`, `insert_after`) and decremented at `remove_internal`, each gated on the actual insert/remove result. Expose `RawList::len()` and `List::len()` (`linked_list.rs`).
- **`components/axsched/src/{fifo,round_robin,cfs}.rs`**: add `len()` + `is_empty()` (delegating to the ready queue) and a new **`pick_stealable_task(pred: impl FnMut(&T) -> bool) -> Option<SchedItem>`** that removes and returns the first ready task matching `pred` (for cfs, lowest-vruntime match; recompute `min_vruntime` exactly as `remove_task` does). These are inherent methods on the concrete schedulers — no `BaseScheduler` trait change needed (avoids churning every impl and keeps `remove_task`'s existing signature).

> *Optional lower-contention variant:* instead of `load() = scheduler.lock().len()`, add an `AtomicUsize` runnable counter on `AxRunQueue`, bumped in `add_task`/`put_task_with_state`/`put_prev_task`, decremented in `pick_next_task`/steal, sampled lock-free. Ship the simple `lock().len()` first (matches prototype); upgrade only if idle cores scanning a hot rq show lock contention on the board.

### New code in `os/arceos/modules/axtask/src/run_queue.rs` (all `#[cfg(all(feature = "smp", feature = "sched-loadbalance"))]`)

Insert after `kick_remote_cpu` (`run_queue.rs:158`):

1. **Online-CPU gate** — `static RUN_QUEUE_ONLINE: AtomicUsize`, `mark_cpu_online(cpu_id)`, `cpu_online(cpu)`. **Required**: `RUN_QUEUES` is `[MaybeUninit<&mut AxRunQueue>; MAX_CPU_NUM]` (`run_queue.rs:54`) and `get_run_queue` does `assume_init_mut` (`:147`). A balancer that *scans* all CPUs would dereference an un-booted secondary's uninit slot = UB. `ax_hal::cpu_num()` is the *configured* count, not the *booted* count. Set the bit in `init()` (after `run_queue.rs:839`) and `init_secondary()` (after `:862`).

2. **`load(&self) -> usize`** on `AxRunQueue` → `self.scheduler.lock().len()` (or the atomic counter).

3. **`pull_task(from_cpu, to_rq: &mut AxRunQueue) -> bool`** — the steal primitive, two disjoint critical sections:
   ```
   // CS1: SOURCE lock only
   let stolen = { get_run_queue(from_cpu).scheduler.lock()
        .pick_stealable_task(|t: &TaskInner|
            t.cpumask().get(to_cpu) && !t.on_cpu() && !t.is_idle()) };  // drops src lock
   let Some(task) = stolen else { return false };
   task.set_cpu_id(to_cpu as _);            // set new home BEFORE enqueue (wake routing)
   // CS2: DEST lock only
   to_rq.scheduler.lock().put_prev_task(task, false);
   true
   ```
   The predicate is the load-bearing safety contract (§Gates). No IPI needed — the puller *is* the consumer.

4. **`idle_pull_once() -> bool`** — scan online remotes, pick the busiest with `load>0`, `pull_task` from it. Runs under `current_run_queue::<NoPreemptIrqSave>()` (satisfies `SpinRaw`'s irq/preempt-off precondition). No-op when `cpu_num() <= 1`.

### Hook point

**`os/arceos/modules/axtask/src/api.rs:474-481`, `run_idle()`** — after `yield_now_unchecked()`, before `wait_for_irqs()`:
```rust
#[cfg(all(feature = "smp", feature = "sched-loadbalance"))]
if crate::run_queue::idle_pull_once() { continue; }  // loop back to run the pulled task
```
This is the "about to go idle" moment. It fires reliably because a quiet core's scheduler holds only the per-CPU `gc` task, which is `Blocked` in `WAIT_FOR_EXIT` most of the time → `pick_next_task()` returns `None` → idle task runs → `run_idle` loop → `idle_pull_once`. (Alternative hook: `resched()`'s `pick_next_task()==None` branch at `run_queue.rs:655`; `run_idle` is simpler and matches the prototype.)

### Why this hits ~2046

8 threads spawn from `main` on cpu0 (an A55) and pile on cpu0's rq (root cause: `select_run_queue` current-CPU bias, `run_queue.rs:195`). cpus 1-7 are running the idle task; on their first `idle_pull_once` each finds cpu0's `load` high and steals exactly one Ready task, then `continue`s and runs it. Convergence to one-thread-per-core in microseconds. Because the threads never block, they stay put → sustained ~2046. Affinity is respected automatically: the `t.cpumask().get(to_cpu)` predicate means a `{4-7}`-masked task is never stolen by an A55.

### First PR scope (this is the concrete first PR)

One PR, feature-gated, zero default-build change:
- `axsched`: `len()`/`is_empty()`/`pick_stealable_task` on fifo/rr/cfs + tests (from prototype).
- `linked_list_r4l`: `len` field + `len()` (from prototype).
- `axtask`: online-gate, `load()`, `pull_task`, `idle_pull_once`, `run_idle` hook; `sched-loadbalance = ["smp"]` feature in `axtask/Cargo.toml`.
- Feature forwarding: `axfeat`, `axstd`, `os/StarryOS/kernel/Cargo.toml` (re-do this hunk by hand — the prototype's version conflicts).
- **Do NOT** touch `select_run_queue`/`select_wake_run_queue` in this PR.

---

## PHASE 2 — big.LITTLE placement (prefer A76 for compute)

**Goal / rung:** an unpinned single compute task lands on an A76 (`cpuprobe` residency `landed=4..7`), ~2.2× single-A55 throughput (~351 vs ~160 ev/s @800MHz). A `{4-7}`-masked task lands on the *best* A76, not always cpu4.

### Capacity model (greenfield — nothing distinguishes cores today)

Add **`ax_hal::dtb::cpu_capacities() -> &'static [u16; MAX_CPU_NUM]`** (prototype hunk for `os/arceos/modules/axhal/src/dtb.rs`), built once from the cached `get_fdt()`:
- Per `cpu@*` node (in device-tree order = logical `cpu_id`, matching someboot's `.enumerate()` mapping — **do not key by `reg`/MPIDR**), read `capacity-dmips-mhz` (A76=1024, A55=530); fall back to `compatible` (`cortex-a76`→1024, `cortex-a55`→530); final fallback `DEFAULT_CPU_CAPACITY=1024` (all-equal → homogeneous/QEMU degrades to plain load-spreading).
- **Naming**: keep this concept *distinct* from `MAX_CPU_NUM` (the "16" the scout mislabeled "CPU_CAPACITY" — it is a max CPU *count*, `axconfig/.../driver_dyn_config.rs`, not a per-core weight).
- MIDR fallback (`axcpu ... read_midr_el1`, A55=`0xD05`/A76=`0xD0B`) is available as a last-resort per-core classifier for DTBs lacking `capacity-dmips-mhz`; wire only if needed.

**Ship it read-only first**: log the table at boot, confirm `idx0-3=530 / idx4-7=1024` on-board with zero behavior change, then wire into placement.

### Placement policy

Add `cpu_capacity(cpu)`, `effective_load(cpu, load) = load*1024/capacity(cpu)`, and `select_least_loaded(cpumask, prefer)` (prototype hunks). **Fix the one real gap in the prototype's picker**: its tie-break biases only toward `prefer` (cache locality), so with all cores idle (`load=0 ⇒ effective_load=0` everywhere) it returns the *lowest-indexed* eligible CPU = an **A55**, which fails the "lone task → A76" goal. The picker's comparison must be, in order:
1. **min `effective_load`**, then
2. **max raw capacity** (empty A76 beats empty A55) ← the missing rule,
3. then `prefer` (last/current CPU) for cache warmth,
4. then lowest index.

With this, while any A76 has headroom its `effective_load` undercuts a loaded A55, so compute flows to A76 first and *spills* to A55 only once each big core holds ~1 runnable — Linux EAS-in-miniature, and `cpumask` stays authoritative (the loop only ever visits mask bits).

### Wire into placement — spawn path only (safe: fresh tasks are never `on_cpu`)

- **`select_run_queue`** (`run_queue.rs:177-206`): under the feature, replace the current-CPU bias with `select_least_loaded(task.cpumask(), current_cpu)`.
- **`set_current_affinity`** (`api.rs:262-289`): today it only migrates when the current CPU leaves the mask, landing via `migrate_entry`→`select_run_queue`; with the fixed picker, a `{4-7}` pin now lands on the best A76 instead of round-robin cpu4.
- **`idle_pull_once`**: switch its "busiest" selection to `effective_load` weighting so pulls prefer draining little cores onto big ones.
- **Leave `select_wake_run_queue` untouched** (deferred to Phase 3 — see Gates).

---

## PHASE 3 (optional) — capacity-aware balancing / EAS-lite

Two independent, individually-gated additions:

1. **Periodic push-balance** — `try_push_balance()` (prototype hunk) hooked in `scheduler_timer_tick` (`run_queue.rs:360-367`, per-CPU, irq+preempt already off). Handles imbalance when *no* core is idle (e.g. 10 threads / 8 cores). **Must** keep the prototype's guards: rate-limit (`balance_counter % 16`), hysteresis (only push if `effective_load(self) ≥ dest_eff + 2`), and the same one-lock-at-a-time / `pick_stealable_task` predicate; IPI-kick the dest. This is the only mechanism that touches the global tick path, so it stays behind a sub-flag and gets its own board soak test.

2. **Wake placement (`select_wake_run_queue` → least-loaded)** — the barrier/producer-consumer locality win. **Blocked on a prerequisite**: first port the upstream deferred-wake handoff (#1495) to this branch (replace the `on_cpu` busy-spin at `run_queue.rs:633` with the stash/take-wake protocol), because routing wakes cross-core drives that spin far more often (§Gates). Until then, do not enable wake-redirect on the board.

3. **Per-task demand EWMA** (further out): an `AtomicU8` on `TaskInner` updated in `task_tick` to infer heavy/light and refine big-vs-little placement beyond the affinity-mask hint. Optional.

---

## CORRECTNESS GATES (the rules migration must obey)

**The `on_cpu` handoff — the one real invariant on this branch.** `on_cpu` is set true in `switch_to` *before* the arch switch (`run_queue.rs:692`) and cleared in `clear_prev_task_on_cpu` *after* (`:811`), so `on_cpu==true` ⇒ the task's registers may not be saved. **Never make a task pickable/enqueued while `on_cpu()==true.`** All Phase-1/2 mechanisms are safe because they act only on *Ready* tasks already sitting in a scheduler ready queue — a Ready task was removed by `pick_next_task` and is by construction not the running task and not `on_cpu`. The predicate keeps `!t.on_cpu()` as belt-and-suspenders anyway.

**The live deadlock this branch has (and must not amplify).** `put_task_with_state` (`run_queue.rs:633`) busy-spins `while task.on_cpu()` **with IRQs off**. Two cores that concurrently wake each other's currently-running task (mutual cross-core wake) spin forever = whole-board freeze. This is the unfixed #1495 hazard. Therefore:
- Idle-pull and spawn-placement (Phases 1-2) are safe — they use `remove_task`/`pick_stealable_task` + `put_prev_task`, never the `Blocked→Ready` `put_task_with_state` spin path.
- **Wake-redirect (Phase 3 (d′)) is unsafe until the deferred-wake fix is ported** — it makes remote wakes the common case, turning the latent mutual-wake deadlock probable.
- **Never busy-spin on a remote `on_cpu` with IRQs off.** If a steal candidate is `on_cpu`, *skip it* (the predicate does), never wait.

**Affinity stays authoritative.** Enforcement is enqueue-time only — the `BaseScheduler` impls are cpumask-blind (`fifo.rs:53`, `round_robin.rs:101` just pop the head). So *every* placement/steal decision must route through `task.cpumask().get(dest)`. The steal predicate `t.cpumask().get(to_cpu)` guarantees a task is only ever enqueued on an rq whose `cpu_id` is in its mask. This also transparently protects the pinned `gc` task (`one_shot(cpu_id)`, added at `run_queue.rs:596`); the idle task is additionally excluded by `!t.is_idle()` (and is never in the ready queue anyway).

**Lock order / deadlock-freedom.** `AxRunQueue.scheduler` is `SpinRaw` (no irq/preempt save of its own — safe *only* under the `NoPreemptIrqSave` guard held by `AxRunQueueRef`/`CurrentRunQueueRef`). Rules: (1) acquire the remote scheduler lock only under that guard; (2) **hold at most one scheduler lock at a time** — `pull_task`/`try_push_balance` lock source, remove, *drop*, then lock dest (never AB/BA); (3) `load()` locks-and-unlocks each rq independently during a scan, so a scan never nests locks; (4) move the *current* task only via the existing `migrate_current`/`migrate_entry` trampoline (`run_queue.rs:397,791`), never a direct cross-core enqueue (its context/`on_cpu` isn't saved until `switch_to` completes).

**Ordering/visibility.** `set_cpu_id` (Release, `task.rs:565`) *before* the task becomes pickable on the dest, so `select_wake_run_queue`'s `last_cpu` reads the new home. `on_cpu` is SeqCst-free here (plain Acquire/Release load/store, `task.rs:576-585`) — sufficient because we never race it; we only ever *read* `on_cpu` in the predicate to *exclude*, never to synchronize a handoff.

**Blast radius.** Everything is behind `sched-loadbalance` (off by default) → default StarryOS/ArceOS/Axvisor builds and QEMU CI are byte-identical. The FDT capacity table's all-equal fallback makes homogeneous/QEMU boards degrade to plain load-spreading, so no non-RK3588 board regresses.

---

## VERIFICATION per phase (via `apps/OScope-harness/harness.py` on OrangePi-5-Plus)

Each phase = a snapshot rung; build the board kernel with `--features sched-loadbalance` and compare against the default-build baseline in the same session.

- **Phase 1 rung:** unpinned `sysbench --cpu --threads=8` → **~160 → ~2046 ev/s** @800MHz; unpinned `--threads=4` masked `{4-7}` → **351 → ~1400** (4× A76). `cpuprobe` residency histogram should show tasks spread across 8 (resp. 4) cores instead of collapsed on one. Confirm no default-build behavior change (feature off = identical binary).
- **Phase 2 rung:** a single unpinned compute task → `cpuprobe` `landed=4..7` (an A76), **~2.2×** single-A55 throughput. Boot log shows the capacity table `idx0-3=530 / idx4-7=1024`. A `{4-7}`-masked task lands on the least-loaded A76.
- **Phase 3 rung:** 10-thread / 8-core workload stays balanced (push-balance); after deferred-wake port, a producer-consumer/barrier workload keeps its spread instead of re-collapsing onto the waker.

**Board-only caveat (must state in every PR):** the mutual cross-core-wake deadlock cannot be reproduced at `max_cpu_num=1` (QEMU CI config), so **CI green is necessary but not sufficient** — validate the balancer under real 8-way SMP on the board, ideally with a wake-heavy soak (many `join`/mutex churn) to stress the `on_cpu` path.

---

## Key file:line anchors (this branch)

- Placement selectors: `os/arceos/modules/axtask/src/run_queue.rs` — `select_run_queue` 177-206, `select_wake_run_queue` 215-244, `select_run_queue_index` 109-126, `get_run_queue` 144-148, `kick_remote_cpu` 150-158.
- Safety machinery: `AxRunQueue` struct 247-254, `put_task_with_state` 610-646 (**`on_cpu` spin 633**), `resched` 648-666 (`None`-branch 655), `switch_to` 668-745 (`set_on_cpu(true)` 692), `migrate_entry` 791-801, `clear_prev_task_on_cpu` 805-812, `init`/`init_secondary` 813-864, `scheduler_timer_tick` 360-367.
- `os/arceos/modules/axtask/src/api.rs` — `spawn_task` 203-208, `set_current_affinity` 262-289, `run_idle` 474-481.
- `os/arceos/modules/axtask/src/task.rs` — `on_cpu()`/`set_on_cpu()` 576-585 (plain Acquire/Release), `cpumask()`/`set_cpumask()` 261-272, `set_cpu_id` 564, `is_idle` 446.
- `components/axsched/src/lib.rs:21-61` (`BaseScheduler`, no `len()`); `fifo.rs:49`/`round_robin.rs:97` `remove_task = List::remove`; `cfs.rs` BTreeMap.
- `os/arceos/modules/axhal/src/dtb.rs:34` `get_fdt`; `axhal/src/lib.rs:122` `cpu_num`.
- Reference prototype (cherry-pick, does not `git apply` cleanly): `.claude/sched-loadbalance.patch`. Prior spec: `scripts/profile/SCHEDULER_LOADBALANCE_SPEC.md`.
