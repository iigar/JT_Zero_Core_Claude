# Coding Conventions

**Analysis Date:** 2026-04-12

---

## Mandatory Rules (from CLAUDE.md)

These rules apply to ALL agents and ALL sessions without exception:

1. **Read `Worklog.md` first** — before any work, every session.
2. **Ukrainian language** — all responses to user must be in Ukrainian.
3. **Frontend build command** — `export REACT_APP_BACKEND_URL="" && yarn build` (or `REACT_APP_BACKEND_URL="" npm run build`). Never build with an Emergent Preview URL — the Pi dashboard will break.
4. **Venv-only Python** — packages MUST be installed via `backend/venv/bin/pip install`. System Python (`apt install python3-*`) is invisible to the service (`backend/venv/bin/uvicorn`).
5. **No numpy** — prohibited on Pi Zero 2W (too heavy). Use Pillow or standard library only. Fallback chain: Pillow → pure Python.
6. **Document all bug fixes** — append every fix to "Key Bug Fixes" in CLAUDE.md with root cause + file:line. No vague descriptions.
7. **Hardware testing** — Emergent environment hits simulator only. Real bugs require `journalctl -u jtzero` logs from physical Pi.

---

## C++ Conventions

### Namespace

All C++ runtime code lives in the `jtzero` namespace:
```cpp
namespace jtzero {
    class Runtime { ... };
    class EventEngine { ... };
}
```

### Class Naming

PascalCase for classes, descriptive noun phrases:
- `Runtime`, `EventEngine`, `ReflexEngine`, `RuleEngine`, `MemoryEngine`, `OutputEngine`
- `CameraPipeline`, `MAVLinkInterface`, `IMUSensor`, `BarometerSensor`, `GPSSensor`
- `RingBuffer<T, N>`, `MemoryPool<T, N>` (templates are PascalCase)
- `ScopedSpinLock` — RAII wrapper, destructor guarantees release

### Member Variable Naming

Private members use `snake_case` with trailing underscore `_`:
```cpp
class Runtime {
    EventEngine   event_engine_;
    CameraPipeline camera_;
    SystemState   state_;
    bool          simulator_mode_;
    std::atomic<bool> running_;
};
```

### Method Naming

`snake_case` for methods, verb phrases for actions, noun phrases for accessors:
```cpp
bool initialize();
void start();
void stop();
bool is_running() const;
bool send_command(const char* cmd, float param1 = 0, float param2 = 0);
const SystemState& state() const;
EventEngine& events();
```

### Free Function Naming

`snake_case`:
```cpp
inline uint64_t now_us();
inline double now_sec();
const char* flight_mode_str(FlightMode mode);
```

### Thread Naming Convention

Threads are named `T{N}_{PascalCaseName}` — matching the 8-thread model table in CLAUDE.md:
- `T0_Supervisor`, `T1_Sensors`, `T2_Events`, `T3_Reflex`
- `T4_Rules`, `T5_MAVLink`, `T6_Camera`, `T7_API_Bridge`

Thread stats expose this name via `ThreadStats::name` field (const char*).

### Constants and Enums

UPPER_SNAKE_CASE for constants and enum values:
```cpp
static constexpr size_t MASK = Capacity - 1;
constexpr float BRIGHT_DROP = 20.0f;
constexpr float BRIGHT_RECOVER = 30.0f;

enum class FlightMode { IDLE, ARMED, TAKEOFF, HOVER, LAND, RTL, EMERGENCY };
```

### File Naming

`snake_case` for `.cpp` and `.h` files:
- `camera_pipeline.cpp`, `camera_drivers.cpp`, `mavlink_interface.cpp`
- `event_engine.h`, `reflex_engine.h`, `common.h`
- Exception: `neon_accel.h` (lowercase, underscore)

### Header Guards

`#pragma once` — used consistently across all headers instead of include guards.

### Compiler Flags (C++17, no exceptions, no RTTI)

```cmake
set(CMAKE_CXX_STANDARD 17)
add_compile_options(-fno-exceptions -fno-rtti -O2 -Wall -Wextra -Wpedantic)
```
Exception: `python_bindings.cpp` is compiled WITH exceptions and RTTI (required by pybind11).

### Memory Discipline

- No dynamic allocation in realtime paths (`camera_loop`, `sensor_loop`, `reflex_engine`)
- Use `RingBuffer<T, N>` (SPSC, stack-allocated) for inter-thread data
- Use `MemoryPool<T, N>` (lock-free CAS free-list) for fixed-size object pools
- `alignas(64)` on hot atomic members to prevent false sharing

### Error Handling (C++)

- No exceptions — all errors are returned as `bool` or status codes
- Check return values; log via `std::printf("[JT-Zero] ...")` with subsystem prefix
- Use `noexcept` on all lock-free data structure methods

---

## Python Conventions

### Module Structure

Each backend Python file has a single clear responsibility:
- `backend/server.py` — FastAPI app, all REST endpoints, WebSocket handlers
- `backend/native_bridge.py` — wraps C++ runtime, VO fallback monitor, RC handling
- `backend/simulator.py` — pure-Python fallback simulator (same interface as NativeRuntime)
- `backend/usb_camera.py` — USB camera via v4l2-ctl subprocess (architecture-independent)
- `backend/flight_log.py` — AES-256 encrypted telemetry log
- `backend/system_metrics.py` — psutil-based OS metrics
- `backend/diagnostics.py` — hardware diagnostics

### Class Naming (Python)

PascalCase:
- `NativeRuntime`, `JTZeroSimulator`, `USBCameraCapture`, `FlightLogger`
- Pydantic models: `CommandRequest`, `LogPasswordRequest`, `CommandResponse`

### Variable and Function Naming (Python)

`snake_case` for all variables and functions:
```python
def get_camera_stats(): ...
def vo_fallback_tick(): ...
async def _vo_fallback_monitor(): ...
flight_logger = FlightLogger()
RUNTIME_MODE = "simulator"  # module-level constants are UPPER_SNAKE_CASE
```

Private helpers use leading underscore:
```python
def _decode_jpeg_to_gray(jpeg_bytes): ...
def _vo_fallback_monitor(): ...
```

### FastAPI Route Patterns

All routes use `/api/` prefix. Pattern: `@app.get("/api/{resource}")` or `@app.post("/api/{resource}/{id}")`.

```python
@app.get("/api/health")
async def health(): ...

@app.get("/api/camera")
async def camera_stats(): ...

@app.post("/api/vo/profile/{profile_id}")
async def set_vo_profile(profile_id: int): ...

@app.post("/api/camera/secondary/capture")
async def capture_secondary(): ...
```

WebSocket routes:
```python
@app.websocket("/api/ws/telemetry")
async def websocket_telemetry(websocket: WebSocket): ...

@app.websocket("/api/ws/events")
async def websocket_events(websocket: WebSocket): ...
```

Response shape for commands:
```python
return {"success": True, "message": "...", "command": cmd}
```

### pybind11 Binding Style

Bindings live in `jt-zero/api/python_bindings.cpp`. Pattern: static helper functions convert C++ structs to `py::dict`, then the module method calls them:

```cpp
namespace py = pybind11;
using namespace pybind11::literals;  // enables "key"_a syntax

static py::dict imu_to_dict(const jtzero::IMUData& d) {
    return py::dict(
        "gyro_x"_a = d.gyro_x,
        "gyro_y"_a = d.gyro_y,
        "valid"_a = d.valid
    );
}
```

All data crossing the C++/Python boundary is returned as `py::dict` for direct JSON serialization. No shared state between threads via Python — all cross-thread data goes through C++ lock-free structures.

### Error Handling (Python)

- Never use bare `except Exception: pass` — it silences critical errors
- Log all exceptions to stderr with subsystem prefix:
  ```python
  sys.stderr.write(f"[VO PyDetect] error: {e}\n")
  ```
- Use one-time dedup flag for repeated error messages:
  ```python
  if not _error_logged:
      sys.stderr.write(f"[VO] error: {e}")
      _error_logged = True
  ```
- Endpoints that parse complex data wrap in try/except and return safe fallbacks:
  ```python
  try:
      return runtime.get_features()
  except Exception as e:
      sys.stderr.write(f"[API] features error: {e}\n")
      return []
  ```

### Import Order

1. Standard library (`os`, `sys`, `time`, `threading`, `io`, `subprocess`)
2. Third-party (`fastapi`, `pydantic`, `PIL`)
3. Local modules (`native_bridge`, `simulator`, `flight_log`, `system_metrics`, `diagnostics`)

Conditional imports with availability flags:
```python
try:
    from PIL import Image
    PIL_AVAILABLE = True
except ImportError:
    PIL_AVAILABLE = False

NUMPY_AVAILABLE = False  # Explicitly prohibited — CLAUDE.md rule 5
```

### CORS Policy

Restrict to explicit origins — no wildcards:
```python
_ALLOWED_ORIGINS = os.environ.get(
    "JTZERO_ALLOWED_ORIGINS",
    "http://localhost:3000,http://localhost:8001,http://jtzero.local:8001"
).split(",")
app.add_middleware(CORSMiddleware,
    allow_origins=_ALLOWED_ORIGINS,
    allow_credentials=False,
    allow_methods=["GET", "POST"],
    allow_headers=["Content-Type"],
)
```

---

## React Conventions

### Component Naming

PascalCase files and default exports, `{Name}.js` filename matches export name:
- `CameraPanel.js` → `export default function CameraPanel({ camera, features })`
- `ThermalPanel.js` → `export default function ThermalPanel({ ... })`
- `MAVLinkDiagPanel.js`, `Drone3DPanel.js`, `CommandPanel.js`

### Component Structure Pattern

```js
import React, { useEffect, useRef, useState } from 'react';
import { IconName } from 'lucide-react';

const API = process.env.REACT_APP_BACKEND_URL || '';  // always relative for Pi build

export default function ComponentName({ prop1, prop2 = defaultValue }) {
  // 1. Refs
  const canvasRef = useRef(null);
  // 2. State
  const [localState, setLocalState] = useState(false);
  // 3. Effects (useEffect with cleanup)
  useEffect(() => {
    let active = true;
    // ... setup
    return () => { active = false; };  // cleanup
  }, [dependency]);
  // 4. Handlers
  // 5. Render
  return ( ... );
}
```

### Backend URL Pattern

Always use `process.env.REACT_APP_BACKEND_URL || ''` at module level. Empty string `""` means relative URLs — required for Pi production build. The CI/CD build explicitly sets `REACT_APP_BACKEND_URL=""`.

```js
const API = process.env.REACT_APP_BACKEND_URL || '';
const frameUrl = `${API}/api/camera/frame`;
```

### State Management

No Redux or global state library. Data flows top-down from `App.js` through props:
- `App.js` owns all top-level state: `state`, `camera`, `mavlink`, `threads`, `events`, `features`, `cameras`, `voTrail`
- WebSocket data received in `useWebSocket` hook → dispatched to App state
- Components receive data as props, send commands via `fetch` calls to API

Throttled updates: accumulate data in refs, flush to state at ~5Hz to avoid excessive re-renders:
```js
const pendingRef = useRef(null);
// update pendingRef on every WS message
// setInterval → flush pendingRef to state at 5Hz
```

### Angle Normalization (important pattern)

Yaw lerp MUST normalize delta to `[-PI, PI]` to prevent full-rotation glitches (Bug Fix #25):
```js
const PI = Math.PI;
let delta = target - current;
delta -= Math.round(delta / (2 * PI)) * 2 * PI;  // normalize
rotation.y += delta * 0.06;  // lerp on normalized delta
```

### Hooks

Custom hooks live in `frontend/src/hooks/`:
- `useApi.js` — `useWebSocket` hook for telemetry and events

### Styling

Tailwind CSS. No inline style objects except for canvas/3D positioning.

---

## Git Commit Style

Conventional Commits format: `type: description`

Types observed in log:
- `fix:` — bug fixes with specific description of what was wrong
- `ci:` — CI/CD changes (`ci: rebuild frontend [skip ci]`)
- `Docs:` — documentation updates (note: capitalized D)
- `VO+IMU:` — feature area prefix for grouped changes
- `Security fixes:` — security-related changes
- `Frontend design overhaul:` — large UI changes

Auto-generated CI commits: `ci: rebuild frontend [skip ci]` — added by GitHub Actions to prevent re-trigger loops.

**Rule:** Commit messages must describe the fix specifically. "fixed bug" is explicitly prohibited by CLAUDE.md rule 2.

---

## Security Constraints

- Path traversal protection: validate `os.path.realpath(filepath)` is within `LOG_DIR` before reading (`flight_log.py`)
- Passwords: derive Fernet key via PBKDF2 100k iterations; store only key bytes, not raw password
- Random salt: per-installation 16-byte random salt stored in `config.json` (not hardcoded)
- Minimum password length: 12 characters
- CORS: explicit origin whitelist, no wildcard, `allow_credentials=False`

---

*Convention analysis: 2026-04-12*
