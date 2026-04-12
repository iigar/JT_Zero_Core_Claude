# Architecture

**Analysis Date:** 2026-04-12

## Pattern Overview

**Overall:** 4-layer hard-realtime pipeline — C++ embedded runtime → pybind11 FFI → FastAPI async server → React dashboard

**Key Characteristics:**
- Hard-realtime C++ layer with no dynamic allocation on hot paths; all buffers pre-allocated at startup
- 8 dedicated threads with fixed CPU affinity and RT priorities (SCHED_FIFO on Linux)
- Python layer is a thin adapter — all computation stays in C++; Python only serialises and routes
- Simulator fallback activates automatically when no hardware is detected (same code path, no branching in callers)
- Lock-free data structures throughout the hot path: SPSC ring buffers + CAS-based memory pool

---

## Layers

**Layer 1 — C++ Runtime (`jt-zero/`):**
- Purpose: All realtime control, sensor fusion, visual odometry, MAVLink I/O
- Location: `jt-zero/core/`, `jt-zero/camera/`, `jt-zero/sensors/`, `jt-zero/mavlink/`, `jt-zero/drivers/`
- Contains: 8-thread scheduler, `SystemState` struct, VO pipeline, sensor drivers, engine classes
- Depends on: Linux pthreads, standard C++17, NEON SIMD intrinsics (ARM)
- Compiled to: `jt-zero/build/libjtzero_core.a` (static lib) + `jt-zero/build/jtzero_native.cpython-311-aarch64-linux-gnu.so` (pybind11 module)

**Layer 2 — pybind11 Bridge (`jt-zero/api/python_bindings.cpp`):**
- Purpose: Exposes `Runtime` class and all getter methods to Python as a native module
- Location: `jt-zero/api/python_bindings.cpp`
- Contains: `state_to_dict()`, `imu_to_dict()`, `vo_to_dict()` helpers; pybind11 module definition
- Note: compiled WITH exceptions and RTTI (required by pybind11); core lib uses neither

**Layer 3 — FastAPI Server (`backend/`):**
- Purpose: REST + WebSocket API for the dashboard; runtime lifecycle management
- Location: `backend/server.py`
- Contains: HTTP endpoints, WebSocket telemetry stream, VO fallback monitor background task, flight logger
- Runtime selection: tries `NativeRuntime` (pybind11) first; falls back to `JTZeroSimulator` (pure Python)
- Adapters: `backend/native_bridge.py` (wraps C++ with simulator-compatible interface), `backend/simulator.py` (pure Python fallback)

**Layer 4 — React Dashboard (`frontend/`):**
- Purpose: Ground station UI; real-time telemetry display
- Location: `frontend/src/`
- Contains: Component panels per subsystem, WebSocket hook `useApi.js`, Tailwind CSS styling
- Consumes: WebSocket stream at `ws://host:8001/ws/telemetry`, REST endpoints at `/api/*`

---

## 8-Thread Model

Defined in `jt-zero/include/jt_zero/common.h` (`THREAD_CONFIGS[]`) and implemented in `jt-zero/core/runtime.cpp`.

| ID | Name | Rate | RT Priority | CPU Core | Role |
|----|------|------|-------------|----------|------|
| T0 | `T0_Supervisor` | 10 Hz | 90 | 0 | Battery, uptime, physics sim, heartbeat, telemetry recording |
| T1 | `T1_Sensors` | 200 Hz | 95 | 1 | IMU/Baro/GPS/Range/Flow poll; complementary filter for roll/pitch; gyro pre-integration for VO |
| T2 | `T2_Events` | 200 Hz | 85 | 2 | Drain `EventEngine` ring buffer; forward to `ReflexEngine`; record in `MemoryEngine` |
| T3 | `T3_Reflex` | 200 Hz | **98** (highest) | 2 | Time-based reflexes (obstacle proximity); emergency rules |
| T4 | `T4_Rules` | 20 Hz | 70 | 3 | `RuleEngine.evaluate()` — battery warnings, altitude limits, flight-mode transitions |
| T5 | `T5_MAVLink` | 50 Hz | 80 | 1 | Serial/UDP MAVLink; receive FC telemetry; send `VISION_POSITION_ESTIMATE`, `OPTICAL_FLOW_RAD` |
| T6 | `T6_Camera` | 15 Hz | 60 | 3 | Capture frame → FAST → LK → Kalman EKF → `VOResult`; multi-camera management |
| T7 | `T7_API` | 30 Hz | 50 | any | Pack `SystemState` + VO stats into Python-readable form; WebSocket push cadence |

All threads use `rate_sleep()` (sleep_until with a fixed `next_wake` time point) for deterministic scheduling.

Thread stats (actual Hz, CPU%, max latency) stored as atomics in `Runtime::thread_stats_[]` — readable lock-free by T7/API.

---

## SystemState Struct

Defined in `jt-zero/include/jt_zero/common.h`, held as `Runtime::state_` (single instance, shared across threads).

```cpp
struct SystemState {
    FlightMode flight_mode;          // IDLE/ARMED/TAKEOFF/HOVER/NAVIGATE/LAND/RTL/EMERGENCY
    bool  armed;
    float battery_voltage, battery_percent;
    float cpu_usage, ram_usage_mb, cpu_temp;
    uint32_t uptime_sec, event_count, error_count;

    IMUData         imu;             // latest raw IMU
    BarometerData   baro;
    GPSData         gps;
    RangefinderData range;
    OpticalFlowData flow;

    float roll, pitch, yaw;          // degrees, complementary-filter output
    float altitude_agl;              // m above ground
    float vx, vy, vz;               // m/s body frame
    float pos_n, pos_e, pos_d;      // NED, relative to home (m)
    float target_altitude;

    float motor[4];                  // 0-1 PWM output
};
```

Access pattern: T1 writes sensor fields at 200 Hz; T0 writes battery/uptime at 10 Hz; T4 writes flight_mode; T5 reads for MAVLink; T7 reads everything for serialisation. No mutex — races tolerated on read-only snapshot fields (all float/bool atomics not needed because reads are one-shot snapshots, not control signals).

---

## Lock-Free Data Structures

Both defined in `jt-zero/include/jt_zero/common.h` (`namespace jtzero`).

**`RingBuffer<T, Capacity>`** — SPSC lock-free queue:
- Cache-line-aligned `head_` and `tail_` atomics (64-byte padding) to prevent false sharing
- `push()` uses `memory_order_release`; `pop()` uses `memory_order_acquire`
- Capacity must be power of 2; index masking via `& MASK`
- Used by `EventEngine` (capacity 1024 events)

**`MemoryPool<T, PoolSize>`** — O(1) lock-free free-list allocator:
- Atomic CAS on `free_head_` for thread-safe allocate/deallocate
- Used for fixed-size object pools where heap allocation is forbidden on realtime paths

**`MemoryEngine`** — Dual ring buffers for telemetry and event history:
- `TELEMETRY_HISTORY = 2048` records (~100 s at 20 Hz)
- `EVENT_HISTORY = 512` records
- Write index is atomic; readers take a snapshot

---

## Visual Odometry Pipeline

Implemented in `jt-zero/camera/camera_pipeline.cpp`, orchestrated by `CameraPipeline::tick()` called from T6 at 15 Hz.

```
FrameBuffer (grayscale, 320x240 default)
       |
       v
[FAST-9 Corner Detector]          -- FASTDetector::detect()
  threshold from AdaptiveVOParams  -- adapts per altitude zone
       |
       v
[Lucas-Kanade Sparse Tracker]     -- LKTracker::track()
  window_size, iterations from VO mode
  initial flow hint from IMU pre-integration (accumulate_gyro(), 200 Hz)
       |
       v
[Outlier Rejection — MAD filter]  -- compute_median() / compute_mad()
  removes features with displacement > 3*MAD
       |
       v
[Kalman EKF — per-axis 1D]        -- kf_vx_, kf_vy_ + variance
  predict: from IMU accelerations (imu_ax_, imu_ay_)
  update: from median optical flow displacement
       |
       v
[Hover Yaw Correction]            -- update_hover_state()
  estimates gyro_z bias during confirmed hover
  corrects accumulated yaw drift
       |
       v
VOResult { dx, dy, dz, vx, vy, vz, confidence, position_uncertainty }
       |
       v
[MAVLink T5]  send VISION_POSITION_ESTIMATE  (msg id 102)
              send OPTICAL_FLOW_RAD          (msg id 106)
```

**Altitude-Adaptive Parameters** (4 zones in `camera.h`):
- `LOW` (0–10 m): aggressive FAST threshold (low), small LK window
- `MEDIUM` (10–50 m): balanced defaults
- `HIGH` (50–200 m): conservative, larger windows, more inliers required
- `CRUISE` (200 m+): maximum stability settings

**Thermal fallback** (for low-contrast CSI frames): Shi-Tomasi structure tensor detector (5×5 window, minimum eigenvalue gate) replaces FAST-9 when processing USB thermal camera frames.

---

## Multi-Camera Architecture

Defined in `jt-zero/include/jt_zero/camera.h` (`CameraSlot`, `CameraSource`).

```
CameraSlot::PRIMARY   = CSI forward camera (PiCSICamera / V4L2 via rpicam-vid pipe)
                         → always active → feeds VisualOdometry
CameraSlot::SECONDARY = USB thermal camera (USBCamera / V4L2 MMAP)
                         → downward-facing → on-demand capture
                         → VO fallback when CSI confidence drops below 0.30
```

**Priority order at init (`initialize_multicam()`):**
1. CSI detected → PRIMARY(VO=CSI), scan USB → SECONDARY(thermal)
2. Only USB → PRIMARY(VO=USB), no SECONDARY
3. Only CSI → PRIMARY(VO=CSI), no SECONDARY
4. Neither → PRIMARY(VO=SimulatedCamera)

**VO Fallback logic** (managed in `backend/native_bridge.py` `vo_fallback_tick()` at 10 Hz):
- Rolling average of `vo_confidence` falls below `conf_drop_thresh = 0.30` → switch to THERMAL_FALLBACK
- CSI probed every `csi_probe_interval_s = 2.0 s`; recover if confidence > `conf_recover_thresh = 0.40`
- `VOFallbackState` tracks reason, switch count, fallback duration

**CSI sensor auto-detection**: `PiCSICamera::detect_sensor()` runs `rpicam-hello --list-cameras`, parses chip ID string, maps to `CSISensorType` enum (OV5647, IMX219, IMX477, IMX708, OV9281, IMX296, OV64A40, IMX290).

---

## Simulator Fallback

When no Raspberry Pi hardware is detected (or `JT_ZERO_SIMULATE=1` is set):

**C++ level** (`jt-zero/core/runtime.cpp`):
- `Runtime::simulator_mode_ = true` (default)
- Each sensor class (`IMUSensor`, `BarometerSensor`, etc.) uses internal simulation when `simulated_ = true`
- `CameraPipeline` opens `SimulatedCamera` (generates moving checkerboard test patterns for VO)
- `MAVLinkInterface` uses `MAVTransport::SIMULATED` (in-memory, no socket)

**Python level** (`backend/server.py`):
- If `jtzero_native` `.so` not found or `NATIVE_AVAILABLE = False` → instantiates `JTZeroSimulator` from `backend/simulator.py`
- `JTZeroSimulator` is a full Python reimplementation matching `NativeRuntime` interface exactly
- Both expose identical method signatures (`get_state()`, `get_camera_stats()`, `send_command()`, etc.)

**Standalone simulator** (`jt-zero/simulator/simulator.py`): alternate simulation script, separate from `backend/simulator.py`.

---

## Data Flow: Telemetry to Dashboard

1. T1 (200 Hz) updates `state_.imu`, `state_.roll/pitch/yaw`
2. T0 (10 Hz) writes `state_.uptime_sec`, battery, calls `memory_engine_.record_telemetry(state_)`
3. T6 (15 Hz) completes VO, stores `VOResult` in `CameraPipeline` internal buffer
4. T7 (30 Hz) `api_bridge_loop()` — reads `state_` + VO stats, marks them ready for Python poll
5. FastAPI WebSocket handler reads runtime at ~10 Hz, serialises to JSON via `native_bridge.py`
6. React `useApi.js` hook receives WebSocket JSON, distributes to panel components via state

---

## Engine Classes

| Engine | File | Purpose |
|--------|------|---------|
| `EventEngine` | `jt-zero/include/jt_zero/event_engine.h` | Lock-free SPSC queue (1024 events); emit/poll interface |
| `ReflexEngine` | `jt-zero/include/jt_zero/reflex_engine.h` | Up to 32 condition→action rules; <5 ms latency guarantee |
| `RuleEngine` | `jt-zero/include/jt_zero/rule_engine.h` | Slower behaviour rules evaluated at 20 Hz |
| `MemoryEngine` | `jt-zero/include/jt_zero/memory_engine.h` | Ring-buffer telemetry + event history; fixed memory |
| `OutputEngine` | `jt-zero/include/jt_zero/output_engine.h` | Queued output commands (GPIO, MAVLink cmds, log) |

---

## Error Handling

**C++ layer:**
- No exceptions in realtime paths (compile flag); functions return `bool` success/fail
- Sensor init failures are non-fatal for non-critical sensors (camera logs warning, continues)
- Hardware probe failure → falls back to simulated mode silently

**Python layer:**
- `try/except` wraps all native module calls; errors logged to stdout
- `server.py` WebSocket handler catches all exceptions per client; disconnections handled gracefully
- VO fallback monitor catches per-tick exceptions without crashing the background task

---

## MAVLink Transport

Defined in `jt-zero/include/jt_zero/mavlink_interface.h`:
- `MAVTransport::SERIAL` — `/dev/ttyAMA0` or `/dev/serial0` (hardware UART on Pi)
- `MAVTransport::UDP` — UDP socket, default port 14550 (SITL / network flight controller)
- `MAVTransport::SIMULATED` — in-memory, no I/O (development/CI)

Messages sent to FC: `VISION_POSITION_ESTIMATE` (id 102), `ODOMETRY` (id 331), `OPTICAL_FLOW_RAD` (id 106)
Messages received: `HEARTBEAT` (0), `ATTITUDE` (30), `GLOBAL_POSITION_INT` (33), `GPS_RAW_INT` (24), `SCALED_IMU` (26)

---

*Architecture analysis: 2026-04-12*
