# Codebase Structure

**Analysis Date:** 2026-04-12

## Directory Layout

```
JT_Zero_Core/
├── jt-zero/                    # C++ runtime (embedded layer)
│   ├── include/jt_zero/        # Public headers (all interfaces)
│   ├── core/                   # Runtime orchestrator + engine implementations
│   ├── camera/                 # Camera drivers + VO pipeline
│   ├── sensors/                # Sensor abstraction layer
│   ├── mavlink/                # MAVLink protocol implementation
│   ├── drivers/                # Low-level I2C/UART/SPI bus drivers + hardware sensor drivers
│   ├── api/                    # pybind11 Python bindings
│   ├── simulator/              # Standalone Python simulator (alternate entrypoint)
│   └── build/                  # CMake build outputs (compiled artifacts)
├── backend/                    # FastAPI server (Python layer)
│   ├── tests/                  # Backend test suite
│   └── static/                 # Compiled React bundle (served by FastAPI)
├── frontend/                   # React dashboard (UI layer)
│   ├── src/
│   │   ├── components/         # React panel components (one per subsystem)
│   │   └── hooks/              # Data-fetching hooks
│   ├── build/                  # Compiled React output (→ copied to backend/static/)
│   └── public/                 # Static assets
├── memory/                     # Project notes / memory files for AI context
├── .planning/                  # GSD planning documents
│   └── codebase/               # Codebase analysis docs (this file)
├── .emergent/                  # Emergent logs / session artefacts
├── .github/                    # CI/CD workflows
├── test_reports/               # Test output artefacts
├── CLAUDE.md                   # Primary project rules and constraints
├── ABOUT_PROJECT.md            # Project overview
├── README.md                   # Setup and usage guide
├── Worklog.md                  # Chronological change log
├── commands_reminder.md        # Common dev commands
├── setup.sh                    # Full environment setup script
├── update.sh                   # Incremental update/deploy script
├── backend_test.py             # Integration test runner
└── design_guidelines.json      # UI design tokens and visual rules
```

---

## Directory Purposes

**`jt-zero/include/jt_zero/`:**
- Purpose: All C++ public interfaces; include these headers from any module
- Key files:
  - `common.h` — `RingBuffer<>`, `MemoryPool<>`, `SystemState`, `Event`, `FlightMode`, `ThreadConfig`, all sensor data structs
  - `runtime.h` — `Runtime` class declaration (main orchestrator)
  - `camera.h` — `CameraPipeline`, `VisualOdometry`, `FASTDetector`, `LKTracker`, `CameraSource` hierarchy, `VOResult`, `VOFallbackState`, all camera enums
  - `sensors.h` — `SensorBase`, `IMUSensor`, `BarometerSensor`, `GPSSensor`, `RangefinderSensor`, `OpticalFlowSensor`, `HardwareInfo`
  - `mavlink_interface.h` — `MAVLinkInterface`, `MAVTransport`, `MAVLinkState`, message structs
  - `event_engine.h` — `EventEngine` (lock-free 1024-event queue)
  - `reflex_engine.h` — `ReflexEngine`, `ReflexRule`
  - `rule_engine.h` — `RuleEngine`
  - `memory_engine.h` — `MemoryEngine`, `TelemetryRecord`, `EventRecord`
  - `output_engine.h` — `OutputEngine`, `OutputCommand`

**`jt-zero/core/`:**
- Purpose: Runtime orchestrator and all engine implementations
- Key files:
  - `runtime.cpp` — `Runtime` implementation: `initialize()`, `start()`, `stop()`, all 8 thread loop functions (`supervisor_loop()` through `api_bridge_loop()`), flight physics, default reflex/rule setup
  - `event_engine.cpp` — `EventEngine` emit/poll implementation
  - `reflex_engine.cpp` — `ReflexEngine` rule processing
  - `rule_engine.cpp` — `RuleEngine` evaluation
  - `memory_engine.cpp` — Telemetry/event ring buffer writes and reads
  - `output_engine.cpp` — Output command queuing

**`jt-zero/camera/`:**
- Purpose: All camera and visual odometry code
- Key files:
  - `camera_pipeline.cpp` — `SimulatedCamera`, `PiCSICamera`, `USBCamera`, `FASTDetector`, `LKTracker`, `VisualOdometry`, `CameraPipeline`; the complete VO pipeline (FAST → LK → MAD filter → Kalman EKF → hover correction)
  - `camera_drivers.cpp` — Camera source open/capture/close implementations for CSI and USB V4L2
  - `neon_accel.h` — ARM NEON SIMD helpers: `structure_tensor_5x5()` (Shi-Tomasi), gradient computation

**`jt-zero/sensors/`:**
- Purpose: Sensor abstraction (simulated + real hardware)
- Key files:
  - `sensors.cpp` — All sensor `initialize()`, `update()`, `try_hardware()` implementations; MPU6050, BMP280, NMEA GPS drivers; hardware auto-detection (`detect_hardware()`)

**`jt-zero/mavlink/`:**
- Purpose: MAVLink protocol (send/receive, transport abstraction)
- Key files:
  - `mavlink_interface.cpp` — `MAVLinkInterface` implementation; serial/UDP/simulated transports; `send_vision_position_estimate()`, `send_optical_flow_rad()`, heartbeat receive

**`jt-zero/drivers/`:**
- Purpose: Low-level hardware bus access
- Key files:
  - `bus.h` / `bus.cpp` — `I2CBus`, `UARTBus`, `SPIBus` classes; raw read/write to `/dev/i2c-1`, `/dev/ttyS0`
  - `sensor_drivers.h` / `sensor_drivers.cpp` — `MPU6050Driver`, `BMP280Driver`, `NMEAParser`; chip-level register access

**`jt-zero/api/`:**
- Purpose: pybind11 bindings that expose `Runtime` to Python
- Key files:
  - `python_bindings.cpp` — Module `jtzero_native`; `Runtime` class binding; all `*_to_dict()` helper converters; exposed methods: `get_state()`, `get_camera_stats()`, `get_mavlink_stats()`, `get_thread_stats()`, `send_command()`, `get_build_info()`

**`jt-zero/simulator/`:**
- Purpose: Standalone Python simulator (alternate, not the main simulator path)
- Key files:
  - `simulator.py` — Python-native drone physics simulator; mirrors C++ Runtime behaviour

**`jt-zero/build/`:**
- Purpose: CMake build output directory (do not edit)
- Key outputs:
  - `jtzero_native.cpython-311-aarch64-linux-gnu.so` — pybind11 shared library loaded by Python
  - `libjtzero_core.a` — Static library (linked into the `.so`)
  - `jt-zero` — Standalone C++ binary (debug/test entrypoint)
  - `CMakeCache.txt` — Build configuration (CMake 3.25, GCC 12, Release mode, Python 3.11)

**`backend/`:**
- Purpose: FastAPI server, Python runtime adapter, flight logging, diagnostics
- Key files:
  - `server.py` — FastAPI app; lifespan (init runtime + VO monitor); REST endpoints at `/api/*`; WebSocket at `/ws/telemetry`
  - `native_bridge.py` — `NativeRuntime` class wrapping `jtzero_native`; VO fallback logic (`vo_fallback_tick()`); implements same interface as `simulator.py`
  - `simulator.py` — `JTZeroSimulator` pure-Python fallback; full state simulation with threading
  - `flight_log.py` — `FlightLogger`; encrypted flight log recording, playback, password management
  - `diagnostics.py` — Hardware scan (`run_diagnostics()`); I2C probe, camera detection, MAVLink status
  - `system_metrics.py` — `get_system_metrics()`; CPU/RAM/temperature via `/proc` and `psutil`
  - `usb_camera.py` — USB thermal camera capture helper used from native_bridge VO fallback
  - `requirements.txt` — Full dependency list (FastAPI, uvicorn, pybind11, Pillow, etc.)
  - `requirements-pi.txt` — Minimal Pi-specific requirements (no numpy — too heavy for Pi Zero)
  - `jtzero_native.cpython-311-aarch64-linux-gnu.so` — Copy of the compiled C++ module (deployed here alongside server.py)

**`backend/tests/`:**
- Purpose: Backend unit and integration tests

**`backend/static/`:**
- Purpose: Compiled React build served by FastAPI at `/`; populated by `npm run build` → copy

**`frontend/src/`:**
- Purpose: React dashboard source
- Key files:
  - `App.js` — Root component; layout, routing between tabs
  - `index.js` — React entry point
  - `hooks/useApi.js` — WebSocket connection management; distributes telemetry to components
- Components (`frontend/src/components/`):
  - `DronePanel.js` — Flight state, armed status, attitude
  - `SensorPanels.js` — IMU, baro, GPS, rangefinder, optical flow
  - `CameraPanel.js` — VO metrics, feature count, confidence, altitude zone, platform info
  - `ThermalPanel.js` — USB thermal camera display + VO fallback status
  - `MAVLinkPanel.js` — MAVLink connection state, message rates
  - `MAVLinkDiagPanel.js` — Detailed MAVLink diagnostics
  - `SimulatorPanel.js` — Simulator config controls (wind, noise, battery drain)
  - `CommandPanel.js` — Arm/disarm/takeoff/land/RTL command buttons
  - `EventLog.js` — Live event stream from `EventEngine`
  - `TelemetryCharts.js` — Time-series charts for sensor data
  - `PerformancePanel.js` — Thread stats (Hz, CPU%, max latency per thread)
  - `DiagnosticsPanel.js` — Hardware scan results
  - `FlightLogPanel.js` — Flight log playback and export
  - `Drone3DPanel.js` — 3D attitude visualisation
  - `Header.js` — Status bar, runtime mode badge (NATIVE/SIM)
  - `Sidebar.js` — Navigation
  - `SettingsTab.js` — VO mode selector, platform info
  - `DocumentationTab.js` — In-app documentation
  - `SafeChart.js` — Error-boundary wrapper for recharts

---

## Key Source Files at a Glance

| File | One-line Description |
|------|----------------------|
| `jt-zero/include/jt_zero/common.h` | All shared types: `RingBuffer`, `MemoryPool`, `SystemState`, `Event`, sensor data structs, `ThreadConfig` |
| `jt-zero/include/jt_zero/runtime.h` | `Runtime` class interface — 8 threads, all engines, sensor + camera + MAVLink accessors |
| `jt-zero/include/jt_zero/camera.h` | Full camera subsystem interface: VO pipeline classes, multi-camera slots, fallback config, FAST/LK/Kalman structs |
| `jt-zero/core/runtime.cpp` | Runtime implementation: all 8 thread loops, hardware init sequence, flight physics simulation |
| `jt-zero/camera/camera_pipeline.cpp` | VO pipeline implementation: FAST detector, Lucas-Kanade tracker, Kalman EKF, hover yaw correction, Shi-Tomasi (thermal) |
| `jt-zero/mavlink/mavlink_interface.cpp` | MAVLink send/receive, transport abstraction (serial / UDP / simulated) |
| `jt-zero/api/python_bindings.cpp` | pybind11 module `jtzero_native` — exposes `Runtime` to Python |
| `backend/server.py` | FastAPI app: REST + WebSocket, VO fallback monitor, flight logger integration |
| `backend/native_bridge.py` | `NativeRuntime` adapter wrapping `jtzero_native`; VO fallback tick logic |
| `backend/simulator.py` | Pure-Python drone simulator matching `NativeRuntime` interface exactly |
| `backend/flight_log.py` | Encrypted flight log: record, playback, password management |
| `frontend/src/hooks/useApi.js` | WebSocket hook distributing telemetry to all React components |

---

## Build Outputs Location

```
jt-zero/build/
├── jtzero_native.cpython-311-aarch64-linux-gnu.so   # Python extension module
├── libjtzero_core.a                                  # Static library
└── jt-zero                                           # Standalone C++ binary

backend/
└── jtzero_native.cpython-311-aarch64-linux-gnu.so   # Deployed copy (server.py imports from here)

frontend/build/                                       # Compiled React bundle
backend/static/                                       # React bundle copied here for FastAPI to serve
```

Build command (from `jt-zero/`):
```bash
mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)
```

---

## Config Files Location

| File | Purpose |
|------|---------|
| `jt-zero/build/CMakeCache.txt` | CMake build configuration (compiler, pybind11 path, build type) |
| `backend/requirements.txt` | Python dependencies (full list for development/desktop) |
| `backend/requirements-pi.txt` | Minimal Pi Zero 2W dependencies (no numpy) |
| `frontend/package.json` | Node.js dependencies (React, recharts, Tailwind) |
| `frontend/tailwind.config.js` | Tailwind CSS configuration |
| `frontend/postcss.config.js` | PostCSS configuration |
| `design_guidelines.json` | UI design tokens: colors, typography, spacing |
| `setup.sh` | Full environment bootstrap (system packages, Python venv, npm build, cmake) |
| `update.sh` | Incremental rebuild and restart |

Environment variable overrides:
- `JT_ZERO_SIMULATE=1` — Force simulator mode even on Raspberry Pi hardware
- `JTZERO_ALLOWED_ORIGINS` — Override CORS allowed origins (comma-separated URLs)

---

## Where to Add New Code

**New sensor type:**
- Interface: `jt-zero/include/jt_zero/sensors.h` — add class inheriting `SensorBase`
- Implementation: `jt-zero/sensors/sensors.cpp`
- Wire up: `jt-zero/core/runtime.cpp` `initialize()` and `sensor_loop()`
- Expose to Python: `jt-zero/api/python_bindings.cpp` — add dict converter + binding

**New MAVLink message:**
- Add `MAVMsgType` enum value in `jt-zero/include/jt_zero/mavlink_interface.h`
- Implement send/receive in `jt-zero/mavlink/mavlink_interface.cpp`

**New REST endpoint:**
- Add route in `backend/server.py`
- If reading C++ data: add getter in `backend/native_bridge.py` `NativeRuntime` and mirror in `backend/simulator.py` `JTZeroSimulator`

**New dashboard panel:**
- Add component file to `frontend/src/components/`
- Register in `frontend/src/App.js`
- Consume data from `useApi.js` hook (already receives full telemetry payload)

**New C++ engine:**
- Header in `jt-zero/include/jt_zero/`
- Implementation in `jt-zero/core/`
- Add instance to `Runtime` class in `jt-zero/include/jt_zero/runtime.h`
- Wire in `jt-zero/core/runtime.cpp`

---

## Special Directories

**`.planning/`:**
- Purpose: GSD planning documents (phases, codebase analysis)
- Generated: No (hand-managed by GSD commands)
- Committed: Yes

**`jt-zero/build/`:**
- Purpose: CMake build artefacts
- Generated: Yes (by cmake + make)
- Committed: No (`.gitignore` should exclude; `.so` present as pre-built artifact)

**`frontend/build/`** and **`backend/static/`:**
- Purpose: Compiled React bundle
- Generated: Yes (by `npm run build`)
- Committed: `backend/static/` is committed as pre-built static content

**`.emergent/`:**
- Purpose: AI session artefacts and emergent logs
- Committed: Yes (per project convention)

**`memory/`:**
- Purpose: AI memory files (project facts, user profile) loaded by Claude at session start
- Committed: Yes

---

*Structure analysis: 2026-04-12*
