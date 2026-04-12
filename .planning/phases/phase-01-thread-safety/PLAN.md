# Phase 01: Thread Safety — Execution Plan

**Phase goal:** Eliminate all data races on `SystemState` and `MemoryPool::allocate()` in the
8-thread C++ drone runtime without breaking real-time performance (CPU ≤55% on Pi Zero 2W).

**Confirmed races (from source audit):**
1. `MemoryPool::allocate()` — ABA vulnerability in CAS free-list (common.h:120–131)
2. `SystemState` — 9 concurrent writers (T0–T7 + send_command) with zero synchronization
3. `EventEngine::emit()` — SPSC `RingBuffer` used by multiple concurrent producers (T0, T1, T3, T4, T5, T6, send_command/T7)

**Out of scope (deferred):** T3/T2 CPU-core preemption analysis (open question #3 in RESEARCH.md)

---

## Dependency Order

```
Task 1  ─────────────────────────────────────── ISOLATED (common.h only)
Task 2  ─────────────────────────────────────── ISOLATED (common.h only)
Task 3  ─────────────────────────────────────── ISOLATED (event_engine.h/.cpp only)
Task 4  ─────────────────────────────────────── depends on Task 3 (uses sensor_mutex_, sensor_seq_)
Task 5  ─────────────────────────────────────── depends on Task 3 (uses slow_mutex_)
Task 6  ─────────────────────────────────────── depends on Task 3 (uses safety_snapshot_)
Task 7  ─────────────────────────────────────── depends on Task 3 (uses sensor_seq_ reader)
Task 8  ─────────────────────────────────────── depends on Task 3 (uses safety_snapshot_ reader)
Task 9   ── depends on Tasks 3–8 (reads safety_snapshot_ filled by tasks above)
Task 10  ── depends on Tasks 3–8 (rule_engine reads need sensor_mutex_ in place)
Task 10a ── depends on Task 2 (needs sensor_mutex_ declared in runtime.h)
Task 11  ── depends on Tasks 1–10a (ThreadSanitizer build — all sources changed)
Task 12  ── depends on Task 11 (hardware verification after TSan passes)
```

Tasks 1, 2, and 3 are all independent of each other (Wave 1 — can run in parallel).
Tasks 4–10 all depend on Task 2's declarations being in place first (Wave 2, sequential).

---

## Task 1 — Fix MemoryPool ABA: replace int32_t free_head_ with 64-bit tagged pointer

**Complexity:** S

**File:** `jt-zero/include/jt_zero/common.h`

**Lines affected:** 108 (field declaration), 120–131 (allocate body)

**What to change:**

Replace the `atomic<int32_t> free_head_` field and the `allocate()` body. Do not touch
`deallocate()` — the research audit confirmed it is already correct.

```cpp
// REMOVE this field (line 108):
alignas(64) std::atomic<int32_t> free_head_{0};

// ADD these in its place:
struct TaggedHead {
    uint32_t idx;  // free-list head index (0xFFFFFFFF = exhausted)
    uint32_t gen;  // generation counter — incremented on every successful pop
};
static_assert(sizeof(TaggedHead) == 8, "TaggedHead must be 8 bytes");

static constexpr uint32_t POOL_EMPTY = 0xFFFFFFFFu;

static uint64_t th_pack(uint32_t idx, uint32_t gen) noexcept {
    return (static_cast<uint64_t>(gen) << 32) | idx;
}
static TaggedHead th_unpack(uint64_t v) noexcept {
    return { static_cast<uint32_t>(v), static_cast<uint32_t>(v >> 32) };
}

alignas(64) std::atomic<uint64_t> free_head_{0}; // initialized by constructor
```

Update the constructor to initialize `free_head_` using the tagged form:
```cpp
// In MemoryPool() constructor, replace:
//   (nothing to add here — just keep the pool_[i].next initialization as-is)
// free_head_ initial value encodes idx=0, gen=0:
free_head_.store(th_pack(0, 0), std::memory_order_relaxed);
```

Replace `allocate()` entirely:
```cpp
T* allocate() noexcept {
    uint64_t raw = free_head_.load(std::memory_order_acquire);
    while (true) {
        TaggedHead h = th_unpack(raw);
        if (h.idx == POOL_EMPTY || static_cast<int32_t>(h.idx) < 0) {
            return nullptr;  // pool exhausted
        }
        int32_t next_idx = pool_[h.idx].next.load(std::memory_order_relaxed);
        uint32_t next_u = (next_idx < 0) ? POOL_EMPTY : static_cast<uint32_t>(next_idx);
        uint64_t new_raw = th_pack(next_u, h.gen + 1);
        if (free_head_.compare_exchange_weak(raw, new_raw,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            alloc_count_.fetch_add(1, std::memory_order_relaxed);
            return &pool_[h.idx].data;
        }
        // raw reloaded by CAS failure — retry
    }
}
```

Add a compile-time lock-free check immediately after the field declaration:
```cpp
static_assert(std::atomic<uint64_t>::is_always_lock_free,
    "atomic<uint64_t> must be lock-free on this platform (ARM64 requirement)");
```

**Why:** The current CAS loop succeeds when a block is recycled back to the same index between
the `next` load and the CAS. The generation counter makes the CAS fail in that case because
`gen` has advanced. See RESEARCH.md "MemoryPool ABA Analysis" for the traced failure path.

**Rollback:** `git revert HEAD` on this commit. The allocate race is probabilistic and silent
under low load — the system will run in a degraded state until reverted.

**Acceptance criteria:**
- `static_assert(std::atomic<uint64_t>::is_always_lock_free, ...)` compiles without error
  on ARM64 (Cortex-A53 with `-march=armv8-a`)
- `deallocate()` is unchanged
- `MemoryPool()` constructor initializes `free_head_` to `th_pack(0, 0)`
- Allocate/deallocate cycle returns correct non-null pointers when pool is not exhausted
- Returns nullptr when pool is exhausted

---

## Task 2 — Declare SafetySnapshot type and synchronization members in runtime.h

**Complexity:** S

**File:** `jt-zero/include/jt_zero/runtime.h`

**What to change:**

Add to the `private:` section, directly after `SystemState state_;` (line 123):

```cpp
// ── Thread-Safety Synchronization ───────────────────────────────────────
//
// Three zones (see .planning/phases/phase-01-thread-safety/RESEARCH.md):
//   Zone A: Safety-critical control fields (T3 reads at 200 Hz, lock-free)
//   Zone B: Sensor data (T1 sole writer in sim; T5 sole writer in FC mode)
//   Zone C: Slow metrics (T0/T7 writers, 10–30 Hz)

// Zone A — SafetySnapshot (8 bytes, lock-free atomic load/store on ARM64)
// Fields: range_distance, range_valid, armed, flight_mode
// Invariant: updated atomically by EVERY site that writes these four fields.
//   Writers: T1 sensor_loop, T0 update_flight_physics, T5 mavlink_loop,
//            send_command(), reflex lambdas (emergency_stop).
//   Reader:  T3 reflex_loop (200 Hz, priority 98).
struct SafetySnapshot {
    float      range_distance{0.0f};
    bool       range_valid{false};
    bool       armed{false};
    FlightMode flight_mode{FlightMode::IDLE};
    uint8_t    _pad{0};  // pad to 8 bytes
};
static_assert(sizeof(SafetySnapshot) == 8,
    "SafetySnapshot must be 8 bytes to fit in atomic<uint64_t>");

// Encode/decode SafetySnapshot as uint64_t for lock-free atomic operations.
// Layout (little-endian): [range_distance 32b][range_valid 8b][armed 8b]
//                          [flight_mode 8b][_pad 8b]
static uint64_t ss_encode(const SafetySnapshot& s) noexcept {
    uint64_t out = 0;
    std::memcpy(&out, &s, 8);
    return out;
}
static SafetySnapshot ss_decode(uint64_t v) noexcept {
    SafetySnapshot s;
    std::memcpy(&s, &v, 8);
    return s;
}

alignas(8) std::atomic<uint64_t> safety_snapshot_{0};

// Helper: write safety fields AND update the atomic snapshot atomically.
// MUST be called from every site that writes range, armed, or flight_mode.
void update_safety_snapshot() noexcept {
    SafetySnapshot snap;
    snap.range_distance = state_.range.distance;
    snap.range_valid    = state_.range.valid;
    snap.armed          = state_.armed;
    snap.flight_mode    = state_.flight_mode;
    safety_snapshot_.store(ss_encode(snap), std::memory_order_release);
}

// Zone B — Seqlock for IMU/attitude/baro/gps sensor zone
// T1 is sole writer in sim mode; T5 is sole writer in FC mode.
// fc_active guard ensures they are mutually exclusive (verified in sensor_loop:309).
// Readers: T6 (15 Hz), T7 (snapshot for Python API).
alignas(64) std::atomic<uint32_t> sensor_seq_{0};  // odd = write in progress

// Zone B FC path — mutex guards T5 writes into imu/baro/gps/attitude fields
// so they do not race with T6/T7 reads that happen without checking fc_active.
std::mutex sensor_mutex_;

// Zone C — Slow metrics mutex: battery, uptime, cpu_temp, event_count, error_count
// Writers: T0 (10 Hz). Readers: T7 (30 Hz), Python API.
// T7-owned fields (cpu_usage, ram_usage_mb) are excluded — T7 is their sole writer.
std::mutex slow_mutex_;

// Zone D — Motor array mutex
// Writer: T0 update_flight_physics (sim mode). Readers: T7 snapshot, Python API.
std::mutex motor_mutex_;
```

Also add `#include <mutex>` to the includes at the top of `runtime.h` if not already present.

**Why:** All downstream tasks (3–9) reference these members. Declaring them first prevents
compiler errors and establishes the single authoritative contract for every write site.

**Rollback:** These are additive declarations. Removing them restores the prior state.
No runtime behavior changes in this task.

**Acceptance criteria:**
- `runtime.h` compiles cleanly (`g++ -std=c++17 -c jt-zero/include/jt_zero/runtime.h`)
- `static_assert(sizeof(SafetySnapshot) == 8)` passes at compile time
- `static_assert(std::atomic<uint64_t>::is_always_lock_free)` passes (added in Task 1)
- All five members visible in `Runtime` private section:
  `safety_snapshot_`, `sensor_seq_`, `sensor_mutex_`, `slow_mutex_`, `motor_mutex_`

---

## Task 3 — Fix EventEngine: add mutex on producer side of SPSC RingBuffer

**Complexity:** S

**Files:**
- `jt-zero/include/jt_zero/event_engine.h`
- `jt-zero/core/event_engine.cpp`

**Root cause:** `EventEngine` uses `RingBuffer<Event, 1024>` which is a pure SPSC structure
(single `head_` atomic, single `tail_` atomic, no producer-side lock). But `emit()` is called
from T0, T1, T3 reflexes, T4 rules, T5, T6, and `send_command()` (T7/Python context) —
at minimum 7 concurrent producers. Two concurrent `push()` calls both read the same `head_`,
both see the same `next`, both write to `buffer_[head_]`, then race on `head_.store()`. One
event is silently overwritten.

**What to change in `event_engine.h`:**

Add `#include <mutex>` at the top. Add a single `std::mutex emit_mutex_` to the private
section. The consumer (`poll`) remains lock-free — `tail_` is only advanced by T2.

```cpp
private:
    std::mutex emit_mutex_;  // serializes concurrent producers on the SPSC RingBuffer
    RingBuffer<Event, QUEUE_SIZE> queue_;
    std::atomic<uint64_t> total_events_{0};
    std::atomic<uint64_t> dropped_events_{0};
```

**What to change in `event_engine.cpp`:**

Lock `emit_mutex_` in `emit(const Event&)`:

```cpp
bool EventEngine::emit(const Event& event) {
    std::lock_guard<std::mutex> lk(emit_mutex_);
    if (queue_.push(event)) {
        total_events_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    dropped_events_.fetch_add(1, std::memory_order_relaxed);
    return false;
}
```

The `emit(EventType, uint8_t, const char*)` convenience overload calls `emit(const Event&)`
so it inherits the lock automatically — no change needed there.

**CPU cost analysis:** At 200 Hz (T1, heaviest emitter), uncontended `std::mutex` lock/unlock
on ARM Cortex-A53 costs ~200–400 ns. That is 0.04–0.08% of T1's 5 ms budget. Negligible.
T3 emits only on obstacle detection (rare). T0/T4/T5/T6 emit at ≤50 Hz. Total added cost
across all threads: <0.2% CPU. Well within the 55% target.

**Why not MPMC RingBuffer:** The existing SPSC `RingBuffer` is correct for T2 as sole
consumer. Adding a mutex on the producer side is the minimal surgical fix — no data structure
replacement required.

**Rollback:** Revert the mutex addition. Race is silent under normal load.

**Acceptance criteria:**
- `emit_mutex_` declared in `EventEngine` private section
- `emit(const Event&)` acquires `lock_guard<mutex>` before `queue_.push()`
- `poll()` unchanged (no lock added to consumer path)
- Compiles cleanly
- No deadlock possible: lock is never held while calling code that would re-enter `emit()`
  (verify by inspection — reflex lambdas called from `reflex_engine_` which is driven by
  `event_loop` T2, not inside `emit_mutex_`)

---

## Task 4 — Protect sensor_loop (T1) writes with seqlock + call update_safety_snapshot()

**Complexity:** M

**File:** `jt-zero/core/runtime.cpp`

**Lines affected:** 308–406 (`sensor_loop` body, the `!fc_active` block + rangefinder writes)

**What to change:**

Wrap the Zone B writes (IMU/attitude/baro/gps block, lines 311–379) in a seqlock writer:

```cpp
if (!fc_active) {
    // --- Seqlock writer: begin ---
    sensor_seq_.fetch_add(1, std::memory_order_release);  // seq becomes odd
    // barrier: all subsequent stores happen after seq is odd

    imu_.update();
    state_.imu = imu_.data();
    // ... all the complementary filter code (unchanged) ...
    state_.roll  = cf_roll_rad  * RAD2DEG;
    state_.pitch = cf_pitch_rad * RAD2DEG;
    state_.yaw  += (gz - gyro_z_bias) * dt_imu * RAD2DEG;
    // ... yaw wrap ...

    if (cycle % 4 == 0) {
        baro_.update();
        state_.baro = baro_.data();
        state_.altitude_agl = state_.baro.altitude;
    }
    if (cycle % 20 == 0) {
        gps_sensor_.update();
        state_.gps = gps_sensor_.data();
    }

    sensor_seq_.fetch_add(1, std::memory_order_release);  // seq becomes even
    // --- Seqlock writer: end ---
}

// Rangefinder (outside fc_active guard — always T1 writes this):
if (cycle % 4 == 1) {
    range_.update();
    state_.range = range_.data();
    update_safety_snapshot();  // MUST follow every range write
}
```

The `camera_.accumulate_gyro()` call (line 366) is already thread-safe per the comment
("mutex inside VisualOdometry") — leave it in place after the seqlock end.

**Why seqlock here and not mutex:** T6 reads `state_.imu`, `state_.altitude_agl`, `state_.yaw`
at 15 Hz. A mutex lock in T6 shared with T1 at 200 Hz would create lock contention in the
hot path. The seqlock gives T6 a wait-free read path (just two `seq_` loads) and spins only
if T1 is mid-write (~5 µs window = 0.008% of T6's 66 ms budget).

**The fc_active mutual-exclusion invariant:** The seqlock is valid because T1 skips the entire
`!fc_active` block when `fc_active = true`. T5 (mavlink_loop) writes IMU/baro/gps only when
`fc_active = true`. They are mutually exclusive by design (line 309). If this invariant ever
breaks, replace the seqlock with `sensor_mutex_` — Task 5 already adds that mutex.

**Rollback:** Remove the two `sensor_seq_.fetch_add` calls and the `update_safety_snapshot()`
call after range write. The seqlock degrades to no synchronization — identical to the current
state.

**Acceptance criteria:**
- `sensor_seq_.fetch_add(1, release)` called before and after the `!fc_active` write block
- `update_safety_snapshot()` called after every `state_.range = range_.data()` write
- `camera_.accumulate_gyro()` call position unchanged
- T1's cycle budget not visibly increased (two atomic stores = ~10 ns total)

---

## Task 5 — Protect mavlink_loop (T5) FC writes with sensor_mutex_ + update_safety_snapshot()

**Complexity:** M

**File:** `jt-zero/core/runtime.cpp`

**Lines affected:** 737–801 (`mavlink_loop` FC telemetry block)

**What to change:**

Wrap the entire FC telemetry write block in `sensor_mutex_`. Also call
`update_safety_snapshot()` after writing `state_.armed` and `state_.flight_mode`.

```cpp
if (mavlink_.has_fc_data() && !simulator_mode_) {
    FCTelemetry fc = mavlink_.get_fc_telemetry();

    std::lock_guard<std::mutex> lk(sensor_mutex_);  // ADD THIS LINE

    // Attitude from FC (existing code unchanged)
    if (fc.attitude_valid) {
        state_.roll  = fc.roll  * 57.2957795f;
        state_.pitch = fc.pitch * 57.2957795f;
        state_.yaw   = fc.yaw   * 57.2957795f;
    }
    // IMU from FC (existing code unchanged)
    if (fc.imu_valid) { ... }
    // Barometer from FC (existing code unchanged)
    if (fc.baro_valid) { ... }
    // GPS from FC (existing code unchanged)
    if (fc.gps_valid) { ... }
    // VFR HUD (existing code unchanged)
    if (fc.hud_valid) { ... }
    // Battery from FC
    if (fc.status_valid) { ... }

    // Armed state — MUST call update_safety_snapshot() after writing
    state_.armed = fc.armed;
    if (fc.armed && state_.flight_mode == FlightMode::IDLE) {
        state_.flight_mode = FlightMode::ARMED;
    } else if (!fc.armed && state_.flight_mode == FlightMode::ARMED) {
        state_.flight_mode = FlightMode::IDLE;
    }
    update_safety_snapshot();  // ADD THIS CALL (inside lock_guard scope)

}  // lock_guard released here
```

**Why mutex here and not seqlock:** T5 writes the same Zone B fields (imu, baro, gps, roll,
pitch, yaw) as T1. Seqlocks only work with a single writer. When fc_active=true, T1 skips
writing, so T5 is the sole Zone B writer — but T6 and T7 still read these fields. The mutex
ensures T6's reads do not see torn state from T5's multi-field writes.

**CPU cost:** `sensor_mutex_` is uncontended in practice (T5 at 50 Hz; T6 locks it at 15 Hz
only to take a snapshot — Task 7 adds that lock). Uncontended mutex: ~200 ns. T5 budget = 20 ms.
Cost = 0.001%.

**Rollback:** Remove the `lock_guard` line and the `update_safety_snapshot()` call.

**Acceptance criteria:**
- `lock_guard<mutex> lk(sensor_mutex_)` is the first line inside the `if (has_fc_data...)` block
- `update_safety_snapshot()` called after the armed/flight_mode writes, still inside the lock scope
- T5 holds `sensor_mutex_` for the entire FC telemetry write (coarse-grained is fine at 50 Hz)

---

## Task 6 — Fix T0 supervisor_loop: protect slow-path writes with slow_mutex_ and motor_mutex_

**Complexity:** S

**File:** `jt-zero/core/runtime.cpp`

**Lines affected:** 259–297 (`supervisor_loop`) and `update_flight_physics` (line 541+)

**What to change in supervisor_loop:**

Wrap the Zone C writes (battery, uptime, event_count, cpu_temp) in `slow_mutex_`.

```cpp
{
    std::lock_guard<std::mutex> lk(slow_mutex_);
    state_.uptime_sec    = static_cast<uint32_t>(now_sec());
    state_.event_count   = static_cast<uint32_t>(event_engine_.total_events());
    state_.battery_voltage -= 0.00001f * sim_config_.battery_drain;
    if (state_.battery_voltage < 10.0f) state_.battery_voltage = 10.0f;
    state_.battery_percent = (state_.battery_voltage - 10.0f) / 2.6f * 100.0f;
    state_.cpu_temp = 42.0f + static_cast<float>(rand() % 100) / 100.0f;
}
```

`update_flight_physics` writes `state_.motor[]` and position/velocity fields. Wrap motor
writes in `motor_mutex_`. Position/velocity fields (pos_n, pos_e, pos_d, vx, vy, vz) are
only read by T4 (rule evaluation) — protect those with a separate lock or include them in
`slow_mutex_`. Use `slow_mutex_` for everything in `update_flight_physics` for simplicity;
T0 runs at 10 Hz and this function is already guarded by `if (!simulator_mode_) return`.

```cpp
// In update_flight_physics, wrap all state_ writes:
{
    std::lock_guard<std::mutex> lk(slow_mutex_);
    // ... existing physics writes to state_.vx, vy, vz, pos_n/e/d, motor[], altitude_agl ...
}
// After the lock: if armed/flight_mode were changed by physics, update snapshot:
update_safety_snapshot();
```

Also add `update_safety_snapshot()` after any place in `update_flight_physics` that modifies
`state_.armed` or `state_.flight_mode` (grep for these assignments in the function body and
add the call after each one, outside the slow_mutex_ lock to avoid lock ordering issues —
`update_safety_snapshot()` only writes the atomic, no mutex involved).

**Why T7's cpu_usage/ram_usage_mb need no lock:** T7 is the sole writer of those two fields
(api_bridge_loop lines 881, 888). No other thread writes them. The Python API reads them via
`state()` accessor, also from T7 context. No race.

**Rollback:** Remove the `lock_guard` wrappers.

**Acceptance criteria:**
- `slow_mutex_` locked before all Zone C writes in `supervisor_loop`
- `update_flight_physics` writes to `state_` wrapped in `slow_mutex_`
- Any write to `state_.armed` or `state_.flight_mode` inside `update_flight_physics` is
  followed by `update_safety_snapshot()` (check function body for these assignments)
- Any write to `state_.range` inside `update_flight_physics` (lines 667–668, sim mode) is
  followed by `update_safety_snapshot()` — T3 reads range data from the atomic snapshot
  at 200 Hz; stale snapshot after T0 sim writes is a live race
- Grep `supervisor_loop` body (lines 259–297) directly for any writes to `state_.armed`
  or `state_.flight_mode` outside `update_flight_physics` — wrap them in `slow_mutex_` +
  `update_safety_snapshot()`
- `motor_mutex_` locked before `state_.motor[]` assignments if they occur outside
  `slow_mutex_` scope

---

## Task 7 — Fix T6 camera_loop reads: seqlock reader + sensor_mutex_ for FC path

**Complexity:** S

**File:** `jt-zero/core/runtime.cpp`

**Lines affected:** 819–857 (`camera_loop`)

**What to change:**

T6 reads `state_.range.distance` (line 827), `state_.altitude_agl`, `state_.yaw` (lines 831–832),
and `state_.imu.acc_x/y`, `state_.imu.gyro_z` (line 837). These are Zone A and Zone B fields.

For Zone A (`state_.range.distance`), read from `safety_snapshot_` instead:
```cpp
// REPLACE (line 827):
//   float ground_dist = state_.range.valid ? state_.range.distance : 1.0f;
// WITH:
SafetySnapshot snap_a = ss_decode(safety_snapshot_.load(std::memory_order_acquire));
float ground_dist = snap_a.range_valid ? snap_a.range_distance : 1.0f;
```

For Zone B fields (`altitude_agl`, `yaw`, `imu`), use `sensor_mutex_` (NOT seqlock):

```cpp
// CHOSEN approach — mutex (simpler, safe in both sim and FC modes):
float cam_altitude, cam_yaw, cam_acc_x, cam_acc_y, cam_gyro_z;
{
    std::lock_guard<std::mutex> lk(sensor_mutex_);
    cam_altitude = state_.altitude_agl;
    cam_yaw      = state_.yaw;
    cam_acc_x    = state_.imu.acc_x;
    cam_acc_y    = state_.imu.acc_y;
    cam_gyro_z   = state_.imu.gyro_z;
}

// Use snapshot values instead of state_ direct reads:
camera_.set_altitude(cam_altitude);
camera_.set_yaw_hint(cam_yaw * 0.0174533f);
camera_.set_imu_hint(cam_acc_x, cam_acc_y, cam_gyro_z);
```

T6 at 15 Hz has a 66 ms budget — 300 ns mutex cost is 0.0005%. Seqlock was considered
but rejected: in FC mode T5 does not increment `sensor_seq_`, making seqlock unsafe there.

**Rollback:** Revert to direct `state_.*` reads.

**Acceptance criteria:**
- T6 reads `range_distance`/`range_valid` exclusively from `safety_snapshot_` (Zone A)
- T6 reads `altitude_agl`, `yaw`, `imu.*` fields under `sensor_mutex_` (Zone B)
- No direct `state_.*` reads in camera_loop for these fields
- T6's 15 Hz timing unaffected (verify: `thread_stats_[6].actual_hz` stays ~15)

---

## Task 8 — Fix send_command() and reflex lambdas: call update_safety_snapshot() after every armed/flight_mode write

**Complexity:** S

**File:** `jt-zero/core/runtime.cpp`

**Lines affected:** 186–209 (`send_command`), 501–504 (`setup_default_reflexes` emergency_stop lambda)

**What to change in send_command():**

`send_command()` writes `state_.armed` and `state_.flight_mode` directly (lines 188–209)
from the Python/T7 call context, with no lock and no snapshot update. This is the 9th write
source confirmed by the research audit.

Add `slow_mutex_` lock before the if/else chain and call `update_safety_snapshot()` after:

```cpp
bool Runtime::send_command(const char* cmd, float param1, float param2) {
    Event e;
    e.timestamp_us = now_us();
    e.source_id = 7;
    e.priority = 200;
    e.data[0] = param1;
    e.data[1] = param2;
    e.set_message(cmd);

    {
        std::lock_guard<std::mutex> lk(slow_mutex_);  // ADD
        if (std::strcmp(cmd, "arm") == 0) {
            e.type = EventType::FLIGHT_ARM;
            state_.armed = true;
            state_.flight_mode = FlightMode::ARMED;
        } else if (std::strcmp(cmd, "disarm") == 0) {
            e.type = EventType::FLIGHT_DISARM;
            state_.armed = false;
            state_.flight_mode = FlightMode::IDLE;
        } else if (std::strcmp(cmd, "takeoff") == 0) {
            e.type = EventType::FLIGHT_TAKEOFF;
            state_.flight_mode = FlightMode::TAKEOFF;
        } else if (std::strcmp(cmd, "land") == 0) {
            e.type = EventType::FLIGHT_LAND;
            state_.flight_mode = FlightMode::LAND;
        } else if (std::strcmp(cmd, "rtl") == 0) {
            e.type = EventType::FLIGHT_RTL;
            state_.flight_mode = FlightMode::RTL;
        } else if (std::strcmp(cmd, "hold") == 0) {
            e.type = EventType::FLIGHT_HOLD;
            state_.flight_mode = FlightMode::HOVER;
        } else if (std::strcmp(cmd, "vo_reset") == 0) {
            camera_.reset_vo();
            e.set_message("VO origin reset (SET HOMEPOINT)");
        }
        update_safety_snapshot();  // ADD — covers all flight_mode + armed writes above
    }  // lock released here

    return event_engine_.emit(e);
}
```

**What to change in emergency_stop reflex lambda (line 501–504):**

The lambda writes `state_.flight_mode` and `state_.armed` but is called from within the
ReflexEngine dispatch path (T2 event_loop context). It must call `update_safety_snapshot()`
after these writes:

```cpp
emergency_stop.action = [](const Event&, SystemState& state, EventEngine& events) {
    state.flight_mode = FlightMode::EMERGENCY;
    state.armed = false;
    // Safety snapshot must be updated here — T3 reads it at 200 Hz.
    // NOTE: ReflexEngine dispatch is on T2; update_safety_snapshot() needs access to
    // the Runtime instance. Options:
    //   (a) Capture `this` in the lambda
    //   (b) Add update_snapshot callback to the lambda signature
    //   (c) Promote emergency fields to std::atomic directly
    // Use option (a): capture [this] in the lambda at setup_default_reflexes() time.
    events.emit(EventType::FLIGHT_DISARM, 255, "EMERGENCY STOP");
};
```

Concretely: change `setup_default_reflexes()` to be a member function (it already is) and
change the lambda captures from `[]` to `[this]`:

```cpp
emergency_stop.action = [this](const Event&, SystemState& state, EventEngine& events) {
    state.flight_mode = FlightMode::EMERGENCY;
    state.armed = false;
    update_safety_snapshot();  // T3 will see EMERGENCY immediately
    events.emit(EventType::FLIGHT_DISARM, 255, "EMERGENCY STOP");
};
```

Check the `ReflexRule` struct's `action` field type — confirm it stores `std::function` (not
a raw function pointer) so it can capture `this`. If it stores a raw pointer, change to
`std::function<void(const Event&, SystemState&, EventEngine&)>`.

**Why slow_mutex_ for send_command:** `send_command` writes `armed` and `flight_mode` which
are also written by T5 (mavlink_loop, under `sensor_mutex_`) and T0 (under `slow_mutex_`).
Using `slow_mutex_` in `send_command` makes it consistent with T0 and avoids introducing a
new lock ordering. T5's `sensor_mutex_` guard does not conflict because T5 only writes these
fields when `fc_active = true` and `send_command` is a human API command — logically distinct.
In the worst case both locks could be held simultaneously on different threads but not in a
cycle (T5 never calls `send_command`).

**Rollback:** Remove the `lock_guard` and the `update_safety_snapshot()` call in
`send_command`, revert the lambda capture from `[this]` to `[]`.

**Acceptance criteria:**
- `send_command()` holds `slow_mutex_` during all `state_.armed` / `state_.flight_mode` writes
- `update_safety_snapshot()` called after the if/else chain, inside the lock scope
- `emergency_stop.action` lambda captures `[this]` and calls `update_safety_snapshot()`
- Check `ReflexRule::action` field type supports `std::function` (i.e., can capture `this`)
- **Compile-time check:** after changing lambda capture to `[this]`, compile `runtime.cpp`;
  if it fails due to capture, change `ReflexRule::action` field type to
  `std::function<void(const Event&, SystemState&, EventEngine&)>` and verify compile succeeds

---

## Task 9 — Fix T3 reflex_loop: read safety fields exclusively from safety_snapshot_

**Complexity:** S

**File:** `jt-zero/core/runtime.cpp`

**Lines affected:** 437–465 (`reflex_loop`)

**What to change:**

T3 reads `state_.range.valid` and `state_.range.distance` (line 449) directly. Replace with
atomic load from `safety_snapshot_`:

```cpp
void Runtime::reflex_loop() {
    constexpr int HZ = 200;
    thread_stats_[3].running.store(true);
    auto next_wake = SteadyClock::now();

    while (running_.load(std::memory_order_acquire)) {
        auto start = SteadyClock::now();

        // Read safety-critical fields from the lock-free snapshot (Zone A).
        // Writers: T1 (sensor_loop after range write), T0 (update_flight_physics),
        //          T5 (mavlink_loop), send_command(), emergency_stop reflex.
        // This path must NEVER block on any mutex — T3 is priority 98.
        SafetySnapshot snap = ss_decode(safety_snapshot_.load(std::memory_order_acquire));

        if (snap.range_valid && snap.range_distance < 0.5f) {
            Event e;
            e.timestamp_us = now_us();
            e.type = EventType::FLIGHT_OBSTACLE_DETECTED;
            e.priority = 250;
            e.data[0] = snap.range_distance;
            e.set_message("Obstacle proximity alert!");
            event_engine_.emit(e);
        }

        auto end = SteadyClock::now();
        update_thread_stats(3, start, end, HZ);
        rate_sleep(next_wake, HZ);
    }

    thread_stats_[3].running.store(false);
}
```

`ss_decode` and `safety_snapshot_` are accessible because `reflex_loop` is a member function
of `Runtime`. No includes needed beyond what `runtime.h` already provides.

**Why this is the critical path fix:** T3 at priority 98 is the highest-priority thread in
the system. Any block on a mutex in T3 would preempt T0–T2 and T4–T7 indefinitely (SCHED_FIFO).
The atomic load of `safety_snapshot_` is guaranteed non-blocking on ARM64 — it is a single
`ldar` instruction.

**Rollback:** Revert to `state_.range.valid` and `state_.range.distance` direct reads.

**Acceptance criteria:**
- `reflex_loop` contains zero direct reads from `state_.*` (grep confirms)
- All field reads go through `ss_decode(safety_snapshot_.load(acquire))`
- The `event_engine_.emit(e)` call is unchanged
- T3 loop latency stays under 100 µs (max_latency_us stays reasonable)

---

## Task 10 — T4 rule_loop and T0 record_telemetry: protect with slow_mutex_ snapshot

**Complexity:** S

**File:** `jt-zero/core/runtime.cpp`

**Lines affected:** 467–487 (`rule_loop`), 284–286 (`supervisor_loop` record_telemetry call)

**What to change in rule_loop:**

T4 calls `rule_engine_.evaluate(state_)` passing the entire struct by value (line 476). This
is a multi-word read while T1/T5 may be writing Zone B fields. Take a snapshot under the
appropriate locks:

```cpp
void Runtime::rule_loop() {
    constexpr int HZ = 20;
    thread_stats_[4].running.store(true);
    auto next_wake = SteadyClock::now();

    while (running_.load(std::memory_order_acquire)) {
        auto start = SteadyClock::now();

        // Build a consistent snapshot of state_ for rule evaluation.
        // T4 runs at 20 Hz — locking briefly is acceptable.
        SystemState snap;
        {
            // Zone B fields first (sensor_mutex_)
            std::lock_guard<std::mutex> lk_b(sensor_mutex_);
            snap = state_;  // copy entire struct under lock
        }
        // Zone A is atomic — overwrite the safety-critical fields from snapshot:
        {
            SafetySnapshot sa = ss_decode(safety_snapshot_.load(std::memory_order_acquire));
            snap.range.distance = sa.range_distance;
            snap.range.valid    = sa.range_valid;
            snap.armed          = sa.armed;
            snap.flight_mode    = sa.flight_mode;
        }

        auto result = rule_engine_.evaluate(snap);
        if (result.action != RuleAction::NONE) {
            // rule_engine_.execute may write back to state_ — wrap if needed
            std::lock_guard<std::mutex> lk(slow_mutex_);
            rule_engine_.execute(result, state_, event_engine_);
            update_safety_snapshot();  // if execute() changes armed/flight_mode
        }

        auto end = SteadyClock::now();
        update_thread_stats(4, start, end, HZ);
        rate_sleep(next_wake, HZ);
    }

    thread_stats_[4].running.store(false);
}
```

Note: copying the entire `SystemState` struct (~600 bytes) under `sensor_mutex_` at 20 Hz
takes roughly 1–2 µs including lock acquisition. T4's budget = 50 ms. Cost: 0.004%.

**What to change in supervisor_loop record_telemetry:**

Line 286: `memory_engine_.record_telemetry(state_)` reads the entire struct while T1 may be
writing. Take a sensor snapshot first:

```cpp
// REPLACE (line 286):
//   memory_engine_.record_telemetry(state_);
// WITH:
SystemState telem_snap;
{
    std::lock_guard<std::mutex> lk(sensor_mutex_);
    telem_snap = state_;
}
memory_engine_.record_telemetry(telem_snap);
```

**Rollback:** Revert to `rule_engine_.evaluate(state_)` and `record_telemetry(state_)` direct calls.

**Acceptance criteria:**
- `rule_loop` reads state only through a `SystemState snap` copy taken under `sensor_mutex_`
- `rule_engine_.execute()` result writes back to `state_` under `slow_mutex_`
- **Deadlock check:** inspect `rule_engine_.execute()` body — confirm it does NOT acquire
  `sensor_mutex_` or `slow_mutex_` directly or transitively (called under `slow_mutex_`)
- `record_telemetry` receives a snapshot, not a live `state_` reference
- T4 at 20 Hz shows no timing regression

---

## Task 10a — Fix python_bindings.cpp: state() must return a copy, not a live reference

**Complexity:** S

**File:** `jt-zero/python_bindings.cpp` (or equivalent pybind11 binding file)

**Lines affected:** `state()` accessor + `api_bridge_loop` snapshot path

**Race confirmed by RESEARCH.md:** The Python `state()` accessor returns `const SystemState&` — a live pointer into the struct. T7/Python reads occur at 30 Hz while T1, T5, T0 continue writing without holding any lock that Python obeys. This is a live data race between pybind11 GIL release and ongoing C++ writes.

**What to change:**

In `python_bindings.cpp`, change the `state()` binding to return by value (snapshot), taken under `sensor_mutex_`:

```cpp
// BEFORE (unsafe — live reference):
.def("state", [](Runtime& r) -> const SystemState& {
    return r.state();
}, py::return_value_policy::reference)

// AFTER (safe — snapshot copy):
.def("state", [](Runtime& r) -> SystemState {
    std::lock_guard<std::mutex> lk(r.sensor_mutex());
    return r.state();  // copy constructor
})
```

Also update `api_bridge_loop` in `runtime.cpp` to snapshot state before passing to Python:

```cpp
// In api_bridge_loop (lines 877–888), replace direct state_ access:
SystemState py_snap;
{
    std::lock_guard<std::mutex> lk(sensor_mutex_);
    py_snap = state_;
}
// Pass py_snap (not state_) to Python bridge calls
```

**Note:** `sensor_mutex_` must be exposed via a public `sensor_mutex()` accessor on Runtime, or the lambda can be a friend/member. Prefer a public accessor.

**Why:** The Python GIL protects Python objects but does NOT prevent concurrent C++ writes to `state_`. Returning a reference means Python reads a live struct that T1 writes at 200 Hz.

**Rollback:** Revert to `return_value_policy::reference`. Identical to current (unsafe) state.

**Acceptance criteria:**
- `state()` Python binding returns `SystemState` by value (copy), NOT a reference
- `sensor_mutex_` held during the copy
- `api_bridge_loop` uses a local `py_snap` copy, not `state_` directly, for Python-facing calls
- TSan build (Task 11) shows zero races on the T7/Python read path

---

## Task 11 — Build with ThreadSanitizer and verify zero races

**Complexity:** M

**Files:** CMakeLists.txt (or Makefile), no source changes

**What to do:**

Add a TSan build configuration. On ARM64 (Pi Zero 2W cross-compile or native build), GCC's
TSan (`-fsanitize=thread`) instruments memory accesses and reports data races at runtime.

```bash
# In the build directory, configure with TSan:
cmake .. -DCMAKE_BUILD_TYPE=Debug \
         -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -g" \
         -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"

# Build:
cmake --build . -j4

# Run (requires all 8 threads to actually start):
TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1" ./jtzero_runtime --sim --duration=30
```

A 30-second sim run with all threads active will exercise the race windows at 200 Hz.
TSan is sensitive to all C++ data races regardless of whether they manifest as visible bugs.

**Expected result after Tasks 1–10:** Zero TSan reports. Any remaining report indicates a
write site that was missed in Tasks 4–10.

**If TSan is not available on the target (Pi Zero 2W has limited RAM):**
Cross-compile on x86-64 with `-fsanitize=thread` and run on the host with simulated sensors
(`--sim` flag). The race patterns are identical; TSan on x86 will catch them.

**Rollback:** N/A — this task makes no source changes. Remove the TSan flags to restore
normal build.

**Acceptance criteria:**
- TSan build compiles cleanly
- 30-second sim run produces zero `DATA RACE` reports in TSan output
- 30-second sim run produces zero deadlock reports
- Process exits cleanly (no crash, no hang)

---

## Task 12 — Verify on hardware: CPU budget and timing regression

**Complexity:** M

**Files:** None (measurement task)

**What to do:**

Run the release build on Pi Zero 2W hardware (or as close to it as available). Measure:

1. **CPU usage** — the `cpu_usage` field in `SystemState` aggregates per-thread estimates.
   Target: aggregate ≤55%. Alert threshold: ≥70%.

   ```bash
   # While jtzero_runtime is running, sample /proc/stat every 2 seconds:
   while true; do grep '^cpu ' /proc/stat; sleep 2; done
   # Also watch the internal cpu_usage via Python API or telemetry log
   ```

2. **Thread latency** — call `runtime.get_thread_stats(N)` for each thread via the Python API.
   Check `max_latency_us`:
   - T1 (200 Hz): max_latency_us should stay under 4000 µs (80% of 5 ms budget)
   - T3 (200 Hz): max_latency_us should stay under 3000 µs (priority 98, should be fast)
   - T5 (50 Hz): max_latency_us should stay under 15000 µs
   - T6 (15 Hz): max_latency_us should stay under 50000 µs

3. **EventEngine drop rate** — call `runtime.events().dropped_events()` after 30 seconds.
   Should be 0 under normal sim load. If non-zero, the `emit_mutex_` is introducing contention
   that slows producers. Investigate which thread is holding the lock longest.

4. **MemoryPool allocation** — call `runtime.memory().used()` on each pool. Verify allocations
   are not exhausting pools (would indicate the ABA fix broke something).

**Pass criteria:**
- Aggregate CPU ≤55% over a 60-second run
- No thread exceeds its latency budget
- EventEngine drop count = 0
- MemoryPool used() < capacity() for all pools throughout the run

**Rollback:** If CPU exceeds 55%, profile which lock is hot (`perf stat -e lock:*` or
`perf record -g ./jtzero_runtime`). The most likely culprit is `sensor_mutex_` contention
between T5 (50 Hz) and T6 (15 Hz). Fix: use `std::shared_mutex` for sensor reads to allow
concurrent T6 reads, with T5 taking an exclusive lock. This trades simplicity for throughput
but keeps the same race-free guarantee.

---

## Completion Checklist

| Task | Race Eliminated | File Changed | Test |
|------|-----------------|--------------|------|
| 1 | MemoryPool ABA | common.h | compile + static_assert |
| 2 | (declarations) | runtime.h | compile |
| 3 | EventEngine MPMC | event_engine.h/.cpp | compile |
| 4 | T1 sensor writes (Zone B) | runtime.cpp | seqlock covers T1→T6 |
| 5 | T5 FC writes (Zone B) | runtime.cpp | mutex covers T5→T6/T7 |
| 6 | T0 slow writes (Zone C) | runtime.cpp | mutex covers T0→T7 |
| 7 | T6 reads (Zone A+B) | runtime.cpp | reads via snapshot/atomic |
| 8 | send_command + reflex lambda | runtime.cpp | snapshot updated on write |
| 9 | T3 reads (Zone A) | runtime.cpp | atomic load only, no blocking |
| 10  | T4 reads + telemetry | runtime.cpp | snapshot copy under lock |
| 10a | pybind11 state() live ref | python_bindings.cpp + runtime.cpp | snapshot copy under sensor_mutex_ |
| 11  | All races | (build flags) | TSan: 0 reports in 30s run |
| 12  | — | (measurement) | CPU ≤55%, latency in budget |

---

## Lock Ordering (to prevent deadlocks)

Multiple locks exist: `slow_mutex_`, `sensor_mutex_`, `motor_mutex_`, `EventEngine::emit_mutex_`.

**Canonical order (always acquire in this sequence when holding multiple):**
1. `sensor_mutex_` (Zone B — most contested)
2. `slow_mutex_` (Zone C — rarely held with sensor_mutex_)
3. `motor_mutex_` (Zone D — only in sim mode)
4. `emit_mutex_` (inside EventEngine — always last, never held when acquiring above)

**Invariant:** No function acquires a lock with a lower number while holding a higher number.
`send_command` uses `slow_mutex_` only. `rule_loop` acquire order: `sensor_mutex_` → `slow_mutex_`.
This matches the canonical order. No deadlock possible.

`update_safety_snapshot()` writes only to `safety_snapshot_` (atomic store) — it acquires
no mutex. It may be called while any mutex is held. No ordering issue.
