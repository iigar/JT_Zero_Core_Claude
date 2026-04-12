# Phase 01: Thread Safety — Research

**Researched:** 2026-04-12
**Domain:** C++ lock-free concurrency, ARM Cortex-A53 memory model, real-time thread safety
**Confidence:** HIGH (based on direct source reading + ARM architecture docs)

---

## Summary

The JT-Zero runtime has two confirmed data races with flight-safety consequences. Both were identified by direct source audit, not speculation.

**Race 1 — SystemState:** Eight threads share a single `Runtime::state_` (plain struct, no synchronization). At 200 Hz, T1 writes IMU/attitude fields; at 50 Hz, T5 writes FC-derived fields; at 10 Hz, T0 writes battery/uptime and passes `state_` by value into `memory_engine_.record_telemetry(state_)`. Meanwhile T3 reads `state_.range.distance` and `state_.armed` for obstacle detection; T4 reads the entire struct for rule evaluation; T6 reads `state_.range`, `state_.altitude_agl`, `state_.yaw`, `state_.imu` as inputs to the camera pipeline; T7 writes `state_.ram_usage_mb` and `state_.cpu_usage`. This is a genuine MPMC (multi-producer, multi-consumer) pattern across all 8 threads — not tolerable even on ARM where 32-bit aligned stores are atomic, because struct fields are not isolated and compound reads are not atomic.

**Race 2 — MemoryPool ABA:** The CAS-based free-list in `MemoryPool<T,N>::allocate()` has a verified ABA vulnerability. The implementation reads `free_head_`, loads `pool_[head].next`, then CAS-es `free_head_` back. Between the load and the CAS, another thread can pop the same block, use it, and push it back — the 32-bit index has no generation counter, so the CAS succeeds against a stale `next`. Two allocations of the same block follow.

**Primary recommendation:** Fix SystemState with a write-side per-domain `std::mutex` + snapshot pattern for readers. Fix MemoryPool ABA with a 64-bit tagged pointer packing index + generation counter into one `std::atomic<uint64_t>`. Both changes are surgical and leave hot-path read performance unchanged for the common case.

---

## Thread Access Map (from source audit)

This is the ground-truth access pattern derived from reading `runtime.cpp` directly. Every field listed maps to actual code lines.

| Field Group | Writer(s) | Reader(s) | Race Severity |
|-------------|-----------|-----------|---------------|
| `imu`, `roll`, `pitch`, `yaw` | T1 (200 Hz, lines 314–362) | T6 (15 Hz, line 837), T5 (50 Hz, line 756), T7 snapshot | CRITICAL — T1 writes, T6 reads simultaneously |
| `baro`, `altitude_agl` | T1 (50 Hz, lines 370–373), T5 (50 Hz, lines 763–766), T0 sim (line 559/663) | T6 (line 827), T4 (rule: altitude_agl > 2.0f), T3 reflex | HIGH |
| `gps` | T1 (10 Hz, lines 376–379), T5 (50 Hz, lines 771–779) | T4 (rule: gps.valid), T7 snapshot | HIGH |
| `range` | T1 (50 Hz, lines 383–385), T0 sim (lines 667–668) | T3 (200 Hz, line 449), T6 (line 827) | CRITICAL — T3 reads at 200 Hz while T1/T0 write |
| `flight_mode`, `armed` | T0 sim (multiple), T5 (lines 796–801), `send_command()` (API thread) | T3 reflex (line 500), T4 rules (line 678–693), T0 reflex lambdas | CRITICAL — armed/flight_mode are control signals |
| `battery_voltage`, `battery_percent` | T0 (lines 272–274), T5 (lines 789–792) | T4 (rule: battery_percent < 10%), reflex (battery_percent < 20%) | HIGH |
| `motor[4]` | T0 sim (lines 558/572/580/589/601/628) | T7 snapshot, Python API | MEDIUM — 4-element array, non-atomic update |
| `ram_usage_mb`, `cpu_usage` | T7 (lines 881/888) | Python API via `state()` ref | LOW — T7 is sole writer |
| `uptime_sec`, `event_count` | T0 (lines 268–269) | T7 snapshot | LOW |
| `pos_n`, `pos_e`, `pos_d`, `vx`, `vy`, `vz` | T0 sim (lines 657–660), T5 (line 784–785) | T4, T7 | MEDIUM |

**Key insight from source:** `send_command()` at lines 188–208 writes `state_.armed` and `state_.flight_mode` directly from the Python API call path — this is a 9th write source outside the named threads, callable at any time from T7 or the pybind11 bridge.

---

## MemoryPool ABA Analysis (from source)

Location: `jt-zero/include/jt_zero/common.h`, lines 120–131.

```cpp
// CURRENT — vulnerable
T* allocate() noexcept {
    int32_t head = free_head_.load(std::memory_order_acquire);  // (A) load head
    while (head >= 0) {
        int32_t next = pool_[head].next.load(std::memory_order_relaxed); // (B) load next
        if (free_head_.compare_exchange_weak(head, next,           // (C) CAS
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            alloc_count_.fetch_add(1, std::memory_order_relaxed);
            return &pool_[head].data;
        }
    }
    return nullptr;
}
```

ABA window: Thread A loads `head=5, next=3` at (A)+(B). Thread B pops block 5 (head becomes 3), uses it, pushes it back (head becomes 5 again, but `pool_[5].next` may now be 7, not 3). Thread A's CAS at (C) sees `free_head_==5`, succeeds — but writes `next=3` as new head, losing block 7 from the list. More critically, Thread B already holds a pointer to block 5 and Thread A now also returns block 5. Same pointer returned twice.

The `deallocate()` path (lines 144–148) has a symmetric issue: it reads `old_head`, sets `pool_[idx].next = old_head`, then CAS-es. If the free list changed between read and CAS, the retry loop (`do { ... } while (!)`) is correct here — the CAS failure causes `old_head` to be reloaded via the CAS failure path of `compare_exchange_weak`. The deallocate side is actually safe. Only allocate has the ABA problem.

**Why the current CAS loop does not fix ABA:** When the CAS on `free_head_` fails, `head` is updated by `compare_exchange_weak` to the current value of `free_head_`. But `pool_[head].next` is re-loaded at line 123 from the block — if the block was recycled and its `next` field was overwritten by a new chain, the re-loaded `next` is correct. The true ABA failure is the case where CAS *succeeds* against a recycled head whose `next` has changed — the CAS passes, but `next` was already stale at step (B). The fix is ensuring `next` cannot go stale between (B) and (C): a generation counter makes the CAS fail in this case.

---

## Recommended Fix: SystemState

### Strategy: Per-Domain Mutex + Snapshot for Slow Readers

The hot path constraint (T1/T3 at 200 Hz, T3 priority 98) eliminates `std::shared_mutex` wrapping the entire struct — a shared_mutex `lock_shared()` on ARM has ~100–400 ns overhead even uncontended, and T3 runs every 5 ms. But T3 only reads two fields: `state_.range.distance` and `state_.range.valid` (line 449). These are 5 bytes total, written by T1 (50 Hz) and T0 sim. A targeted atomic copy of just these fields is the right tool.

**Decompose SystemState access into three zones:**

**Zone A — Safety-critical control fields (read at 200 Hz by T3):**
`range.distance`, `range.valid`, `armed`, `flight_mode`

These are read by T3 at highest RT priority (98). They must be accessible lock-free. Solution: promote these to `std::atomic` wrappers or copy them into a dedicated `std::atomic<SafetySnapshot>` that T1/T5/T0 update atomically after writing the main struct.

```cpp
// Add to Runtime (not SystemState — keeps struct layout clean)
struct SafetySnapshot {
    float     range_distance;
    bool      range_valid;
    bool      armed;
    FlightMode flight_mode;
    uint8_t   _pad[1];  // align to 8 bytes
};
static_assert(sizeof(SafetySnapshot) <= 8, "Must fit in atomic");
// Use std::atomic<uint64_t> with bit-packing, or std::atomic<SafetySnapshot>
// On ARM64 with 8-byte alignment, atomic<SafetySnapshot> is lock-free.
alignas(8) std::atomic<SafetySnapshot> safety_snapshot_{};
```

T3 reads from `safety_snapshot_` only. T1 writes main struct then calls `update_safety_snapshot()` which stores atomically. Zero locks on T3's critical path.

**Zone B — Sensor data written by one thread, read by others (T1 writes, T6/T5/T7 read):**
`imu`, `baro`, `gps`, `roll`, `pitch`, `yaw`, `altitude_agl`, `vx`, `vy`, `vz`

These are large structs (IMUData = 32 bytes). On 64-bit ARM, stores larger than 8 bytes are never atomic. Use a `std::mutex sensor_mutex_` that T1 holds while writing, and readers (T6, T5) take briefly when they need a snapshot. T6 runs at 15 Hz — locking once per 66 ms to copy ~150 bytes is negligible. T5 at 50 Hz similarly. This is the simplest correct approach.

Alternatively, a seqlock gives readers a wait-free fast path with no lock acquisition:

```cpp
// Seqlock — readers spin only if a write is in progress
std::atomic<uint32_t> seq_{0};  // odd = write in progress

// Writer (T1):
void write_imu(const IMUData& d) {
    seq_.fetch_add(1, std::memory_order_release);   // seq becomes odd
    state_.imu = d;
    state_.roll = ...; state_.pitch = ...; state_.yaw = ...;
    seq_.fetch_add(1, std::memory_order_release);   // seq becomes even
}

// Reader (T6, T7):
IMUSnapshot read_imu_snapshot() {
    IMUSnapshot snap;
    uint32_t s1, s2;
    do {
        s1 = seq_.load(std::memory_order_acquire);
        snap = { state_.imu, state_.roll, state_.pitch, state_.yaw };
        s2 = seq_.load(std::memory_order_acquire);
    } while (s1 != s2 || (s1 & 1));
    return snap;
}
```

A seqlock is correct here because T1 is the sole writer of the IMU zone. The spin loop retries only when a write is actually happening (5 µs window at 200 Hz, ~0.1% of T6's 66 ms budget).

**Zone C — Slow-path metrics written by T0/T7 (battery, uptime, cpu_usage, ram_usage_mb):**
These are written at 10–30 Hz and read only by T7/Python. A single `std::mutex slow_mutex_` is acceptable here. Alternatively, since T7 is both writer (cpu_usage, ram_usage_mb) and the Python API reader, these two fields need no locking at all — sole writer.

**Summary of recommended locks:**

| Zone | Fields | Mechanism | Writer(s) | Reader(s) | Lock cost on hot path |
|------|--------|-----------|-----------|-----------|----------------------|
| Safety | range, armed, flight_mode | `atomic<SafetySnapshot>` (8B) | T1, T0, T5, send_command | T3 (200 Hz) | Zero — atomic load |
| Sensors | imu, baro, gps, attitude | seqlock (`atomic<uint32_t> seq_`) | T1 (sole writer of IMU zone) | T6, T5, T7 | ~2 atomic loads per read, no spin under normal conditions |
| FC telemetry | imu/baro/gps when fc_active | `std::mutex sensor_mutex_` | T5 (FC path) | T1 (skipped when fc_active), T6, T7 | <1% of T5's 20 ms budget |
| Slow metrics | battery, uptime, cpu, ram | `std::mutex slow_mutex_` | T0, T7 | T7, Python | Irrelevant — 10–30 Hz |
| Motors | motor[4] | `std::mutex motor_mutex_` or atomic array | T0 sim | T7, Python | Only in sim mode; negligible |

---

## Recommended Fix: MemoryPool ABA

### Strategy: 64-bit Tagged Pointer (Index + Generation Counter)

On ARM64 (Cortex-A53), `std::atomic<uint64_t>` is lock-free and uses a single `stxr`/`ldaxr` pair. Pack the 32-bit index and a 32-bit generation counter into one 64-bit word:

```cpp
// Replace:
alignas(64) std::atomic<int32_t> free_head_{0};

// With:
struct TaggedHead {
    uint32_t idx : 32;
    uint32_t gen : 32;
};
static_assert(sizeof(TaggedHead) == 8);
alignas(64) std::atomic<uint64_t> free_head_{0}; // encodes TaggedHead

static uint64_t pack(uint32_t idx, uint32_t gen) {
    return (static_cast<uint64_t>(gen) << 32) | idx;
}
static TaggedHead unpack(uint64_t v) {
    return { static_cast<uint32_t>(v), static_cast<uint32_t>(v >> 32) };
}
```

Updated `allocate()`:

```cpp
T* allocate() noexcept {
    uint64_t raw = free_head_.load(std::memory_order_acquire);
    while (true) {
        TaggedHead h = unpack(raw);
        if (static_cast<int32_t>(h.idx) < 0) return nullptr; // exhausted
        int32_t next_idx = pool_[h.idx].next.load(std::memory_order_relaxed);
        uint64_t new_raw = pack(static_cast<uint32_t>(next_idx), h.gen + 1);
        if (free_head_.compare_exchange_weak(raw, new_raw,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            alloc_count_.fetch_add(1, std::memory_order_relaxed);
            return &pool_[h.idx].data;
        }
        // raw updated by CAS failure — retry with fresh value
    }
}
```

The generation counter (`h.gen + 1`) is incremented on every successful allocation. The ABA scenario now fails: when Thread A's stale `(idx=5, gen=3)` is compared against the current `(idx=5, gen=5)`, the CAS fails because `gen` differs. Thread A retries with the current value and sees the correct `next`.

**Why 32-bit gen is sufficient:** The counter wraps at 2^32 ≈ 4 billion allocations. At the max allocation rate conceivable for this system (8 threads, 200 Hz event bursts), wrap-around requires ~170 million seconds of continuous operation. Practically infinite.

**Pool's Block.next field:** The per-block `next` is currently `std::atomic<int32_t>`. This is correct and unchanged — writers (deallocate) set it before publishing via the CAS on `free_head_`, so the relaxed load in allocate is safe (protected by the acquire on `free_head_`).

---

## Existing Lock-Free Patterns to Extend

The codebase already uses these patterns correctly:

1. **RingBuffer<T, Capacity>** (`common.h` lines 42–96): Correct SPSC with cache-line-padded atomics, `memory_order_release` on push, `memory_order_acquire` on pop. No changes needed.

2. **InternalThreadStats** (`runtime.h` lines 140–147): All fields are individual atomics. Used correctly with relaxed ordering for stats (non-critical). T7 reads these lock-free at line 886.

3. **EventEngine** (SPSC ring buffer): Already lock-free and correct. T1 is the sole emitter on sensor paths; T2 drains. Multiple emitters (`send_command`, reflexes) indicate this may not be truly SPSC — worth auditing separately, but out of scope for this phase.

The seqlock and safety-snapshot patterns proposed above are direct extensions of the atomic discipline already in the codebase.

---

## pybind11 / T7 Considerations

`python_bindings.cpp` exposes `state()` as a const reference and calls `state_to_dict()` from the GIL. T7 calls into Python from the `api_bridge_loop()` at 30 Hz. With the proposed changes:

- T7 must acquire the seqlock reader path before passing state to Python — a snapshot copy is safer than exposing the live reference.
- `send_command()` called from Python currently writes `state_.armed` and `state_.flight_mode` directly (lines 188–208) without any lock. This is a write path from the GIL context. After the fix, `send_command()` must update `safety_snapshot_` atomically after modifying the struct fields.
- The Python binding must not hold a raw `const SystemState&` across a GIL release — it should copy to a local struct first.

---

## Files and Lines Requiring Changes

| File | Location | Change Required |
|------|----------|-----------------|
| `jt-zero/include/jt_zero/common.h` | Lines 120–131 (`MemoryPool::allocate`) | Replace `int32_t free_head_` with `uint64_t` tagged pointer; update allocate() |
| `jt-zero/include/jt_zero/common.h` | Line 108 (`MemoryPool::free_head_`) | Change field type from `atomic<int32_t>` to `atomic<uint64_t>` |
| `jt-zero/include/jt_zero/runtime.h` | After line 125 (`state_` member) | Add `atomic<uint64_t> safety_snapshot_`, `atomic<uint32_t> sensor_seq_`, `mutex slow_mutex_` |
| `jt-zero/core/runtime.cpp` | Lines 314–385 (sensor_loop, all `state_.*=` writes) | Wrap IMU zone writes in seqlock; call `update_safety_snapshot()` after range/armed/flight_mode writes |
| `jt-zero/core/runtime.cpp` | Lines 447–457 (reflex_loop) | Replace `state_.range.distance` and `state_.range.valid` with `safety_snapshot_.load()` reads |
| `jt-zero/core/runtime.cpp` | Lines 476–478 (rule_loop) | Replace direct `state_` read with `rule_engine_.evaluate(get_sensor_snapshot())` |
| `jt-zero/core/runtime.cpp` | Lines 736–801 (mavlink_loop FC path) | Lock `sensor_mutex_` for the FC data writes into state fields |
| `jt-zero/core/runtime.cpp` | Lines 188–208 (`send_command`) | Add `update_safety_snapshot()` call after flight_mode/armed writes |
| `jt-zero/core/runtime.cpp` | Lines 824–840 (camera_loop) | Replace `state_.range`, `state_.imu`, `state_.altitude_agl`, `state_.yaw` reads with snapshot reads |
| `jt-zero/core/runtime.cpp` | Lines 877–888 (api_bridge_loop) | T7 writes `state_.ram_usage_mb`, `state_.cpu_usage` — these become T7-owned, no locking needed since T7 is sole writer; expose via accessor, not direct state ref |

---

## Common Pitfalls

### Pitfall 1: Protecting the Struct But Not the Snapshot
**What goes wrong:** Developer adds `sensor_mutex_` and locks it in T1, but forgets that `send_command()` also writes `flight_mode` and `armed` without holding any lock.
**Why it happens:** `send_command()` is called from Python (T7 context) and looks like a command dispatcher, not a state writer.
**How to avoid:** After the fix, `update_safety_snapshot()` must be called from every write site of safety-critical fields. Add a helper that encapsulates both the struct write and the atomic snapshot update. Never write `flight_mode` or `armed` without calling it.
**Warning signs:** TSan reports a data race on `state_.armed` from a thread that isn't T0/T1/T5.

### Pitfall 2: Seqlock With Multiple Writers
**What goes wrong:** The seqlock proposed for Zone B assumes T1 is the sole writer of the IMU zone. If T5 (FC path) also writes `state_.imu` while T1 is writing — both increment `seq_` — readers see `seq_` go from 2→3→4→5` in sequence and miss the torn state between T1 and T5 writes.
**Why it happens:** FC mode (lines 736–801) writes `imu`, `baro`, `gps`, `roll`, `pitch`, `yaw` — the same Zone B fields as T1.
**How to avoid:** In FC mode (`fc_active = true`), T1 skips the sensor write entirely (line 309). The two writers are mutually exclusive in practice: either T1 writes (sim mode) or T5 writes (FC mode). Verify this holds by checking the `fc_active` guard. If it can ever be false while T5 is also writing, use `sensor_mutex_` instead of the seqlock.

### Pitfall 3: False Sharing on MemoryPool Block.next
**What goes wrong:** Each `Block` has a `data` field (size depends on T) followed by `alignas(64) std::atomic<int32_t> next`. This means each block is padded to 64 bytes. For small T (e.g., `Event` at ~88 bytes), the total block size is 152 bytes. 128 blocks = ~19 KB — fits in L2. But if T is large (e.g., `FrameBuffer`), the pool may exceed L2 cache, causing cache pressure during allocation/deallocation.
**How to avoid:** The current design is already cache-line-aligned on `next`. No change needed. But when instantiating `MemoryPool` for new types, check that `PoolSize * sizeof(Block)` fits comfortably in L2 (256 KB on Cortex-A53).

### Pitfall 4: ARM Memory Model — Don't Trust "32-bit is atomic"
**What goes wrong:** Developer reads that ARM guarantees aligned 32-bit loads/stores are atomic and concludes that `float battery_percent` is safe without locking.
**Why it happens:** True for single 32-bit fields in isolation. But `battery_voltage` and `battery_percent` are adjacent in the struct — a 64-bit load optimized by the compiler reads both at once. Also, the *compiler* can reorder stores/loads without `std::atomic`, so the race is undefined behavior regardless of hardware guarantees.
**How to avoid:** Only use raw struct fields for intra-thread access. All cross-thread access must go through `std::atomic` or synchronized paths. The C++ memory model, not ARM hardware, is the correct abstraction level.

### Pitfall 5: Reflex Lambda Captures `SystemState&` by Reference
**What goes wrong:** Reflex lambdas in `setup_default_reflexes()` (lines 498–504) take `SystemState& state` by reference and write to it: `state.flight_mode = FlightMode::EMERGENCY; state.armed = false;`. After the fix, these lambdas must call `update_safety_snapshot()` or the safety snapshot will be stale after an emergency stop.
**How to avoid:** Change reflex lambda signatures to accept a writer proxy that encapsulates both the struct write and the snapshot update. Or document clearly that any lambda that writes `flight_mode`/`armed` must also update the snapshot.

---

## CPU Budget Analysis

Target: CPU ≤55%, alert at 70%. Pi Zero 2W: 4× Cortex-A53 @ 1 GHz.

**Cost of proposed additions:**

| Addition | Thread(s) | Estimated cost | Notes |
|----------|-----------|----------------|-------|
| `atomic<uint64_t>` store of SafetySnapshot | T1, T0, T5, send_command | ~5 ns per store | Single `stlr` on ARM64 |
| `atomic<uint64_t>` load in T3 | T3, 200 Hz | ~3 ns × 200/s = 0.06% CPU | Negligible |
| Seqlock `seq_` store×2 per T1 cycle | T1, 200 Hz | ~10 ns per cycle = 0.2% CPU | Acceptable |
| Seqlock `seq_` read×2 in T6 | T6, 15 Hz | ~6 ns × 15/s = 0% CPU | Rounding error |
| `sensor_mutex_` in mavlink_loop FC path | T5, 50 Hz | ~200–500 ns per lock, 50/s = 0.001–0.025% CPU | Negligible |
| `slow_mutex_` in T0 | T0, 10 Hz | ~200 ns × 10/s = 0% CPU | Negligible |
| Tagged-pointer CAS in MemoryPool | Allocation sites | Same as before + 1 wider CAS | `cmpxchg` on 64-bit word vs 32-bit: same latency on ARM64 |

**Total estimated CPU overhead: <0.5% across all threads.** Well within budget. No hot-path mutex is introduced.

---

## Don't Hand-Roll

| Problem | Don't Build | Use Instead |
|---------|-------------|-------------|
| ABA-safe lock-free stack | Custom tagged-pointer implementation from scratch | The tagged-pointer pattern documented here — it is 15 lines and directly replaces the existing CAS loop |
| Full read-write lock for SystemState | Custom RW spinlock | `std::mutex` for slow paths (T0, T5 FC, T4) is correct and cheaper than a homegrown solution on 4-core ARM |
| Wait-free snapshot mechanism | Complex epoch-based reclamation | Seqlock is correct for the single-writer IMU zone and is 10 lines |
| Per-field atomics for entire SystemState | `std::atomic<float>` for every field | This would bloat SystemState, destroy locality, and require 20+ atomic stores per T1 cycle. Use seqlock instead. |

---

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| `std::mutex` for MemoryPool | Lock-free CAS free-list | Unknown (documented as Bug Fix #2) | O(1) allocation, but introduced ABA vulnerability |
| No IMU/gyro synchronization | `std::mutex preint_mtx_` for PreIntState | 2026-04-03 (Fix #41) | Correct but adds latency variance to T1 |
| (planned) | SafetySnapshot atomic + seqlock for sensor zone | This phase | Eliminates races without hot-path locks |

---

## Open Questions

1. **Is EventEngine truly SPSC?**
   - What we know: `RingBuffer` is documented as SPSC. Multiple threads call `event_engine_.emit()`: T0, T1, T3 reflexes, T4 rules, T5, T6, `send_command()` (Python thread).
   - What's unclear: SPSC with multiple producers is a data race on `head_`. The `EventEngine` wrapper may add its own synchronization — the header was not read in this audit.
   - Recommendation: Read `jt-zero/include/jt_zero/event_engine.h` before the planner finalizes tasks. If EventEngine uses a plain `RingBuffer` with multiple producers, that is a third race condition.

2. **Does `memory_engine_.record_telemetry(state_)` copy the struct safely?**
   - What we know: T0 calls `memory_engine_.record_telemetry(state_)` at 10 Hz (line 286), passing `state_` by const reference. At the same time, T1 is writing IMU fields at 200 Hz.
   - What's unclear: If `record_telemetry` does a memcpy of the struct, it will produce a torn snapshot. This is a read-time race, not a write-time race.
   - Recommendation: After the seqlock is in place, T0 should call `get_sensor_snapshot()` first, then pass the snapshot to `record_telemetry`. The telemetry record will then be internally consistent.

3. **T3 and T2 share CPU core 2 — does T3 preempt T2 mid-write to EventEngine?**
   - What we know: T3 (priority 98) and T2 (priority 85) are both pinned to CPU core 2. SCHED_FIFO means T3 can preempt T2 at any point. T2 drains the EventEngine ring buffer; T3 emits to it.
   - What's unclear: If T2 is mid-drain and T3 preempts and emits — is the SPSC ring buffer safe with this interleaving? SPSC is safe only if producer and consumer never run concurrently. On a single core, they cannot run truly concurrently, but T3 preempting T2 mid-drain means T3 becomes a second concurrent producer after T2 has already produced.
   - Recommendation: Needs architectural review. Short-term: verify EventEngine has its own mutex on emit side, or pin T3 and T2 to different cores if feasible.

---

## Sources

### Primary (HIGH confidence)
- Direct source read: `jt-zero/core/runtime.cpp` — all thread loop implementations
- Direct source read: `jt-zero/include/jt_zero/common.h` — MemoryPool and SystemState definitions
- Direct source read: `jt-zero/include/jt_zero/runtime.h` — Runtime class layout
- Direct source read: `.planning/codebase/CONCERNS.md` — confirmed race descriptions with line numbers
- Direct source read: `.planning/codebase/ARCHITECTURE.md` — thread model and priorities

### Secondary (MEDIUM confidence)
- ARM Cortex-A53 TRM: 32-bit aligned loads/stores are single-copy atomic on ARM; multi-word operations are not
- C++ standard (N4860): Plain struct field access from multiple threads without synchronization is undefined behavior regardless of hardware atomicity
- Seqlock pattern: Widely documented in Linux kernel and C++ concurrency literature; correct for single-writer multiple-reader with spin-retry on contention

### Tertiary (LOW confidence — not independently verified for this exact GCC/ARM64 toolchain)
- `std::atomic<uint64_t>` is lock-free on ARM64: expected true for Cortex-A53 with GCC -march=armv8-a, but should be verified with `std::atomic<uint64_t>::is_always_lock_free` at compile time
- Tagged pointer generation counter wrap safety at 2^32: theoretical analysis, not load-tested

---

## Metadata

**Confidence breakdown:**
- Thread access map: HIGH — derived directly from source, not inferred
- MemoryPool ABA analysis: HIGH — code path traced line by line
- Recommended fix (SafetySnapshot + seqlock): HIGH — established patterns, CPU cost estimated from ARM instruction timing
- CPU budget estimate: MEDIUM — based on published ARM64 instruction latencies, not measured on target hardware
- Open questions: identified from source, need EventEngine header read to close #1 and #3

**Research date:** 2026-04-12
**Valid until:** 2026-06-01 (stable domain — C++ concurrency patterns do not change; ARM architecture is fixed)
