# External Integrations

**Analysis Date:** 2026-04-12

## pybind11 C++↔Python Bridge

**Module:** `jtzero_native` (shared library: `backend/jtzero_native.cpython-311-aarch64-linux-gnu.so`)

**Binding source:** `jt-zero/api/python_bindings.cpp`

**Build:** `pybind11_add_module(jtzero_native ...)` in `jt-zero/CMakeLists.txt`; installed to `backend/` via `cmake --install`.

**What is exposed to Python:**

| C++ Class/Function | Python name | Notes |
|--------------------|-------------|-------|
| `jtzero::Runtime` | `jtzero_native.Runtime` | Full lifecycle: `initialize()`, `start()`, `stop()` |
| `Runtime::state()` | `get_state()` → dict | `SystemState` struct serialized: flight_mode, armed, battery, IMU, baro, GPS, rangefinder, optical_flow, VO position |
| Thread stats | `get_thread_stats(i)` → list[dict] | 8 threads: name, target_hz, actual_hz, cpu_percent, loop_count, max_latency_us |
| Engine stats | `get_engine_stats()` → dict | events, reflexes, rules, memory, output engine counters |
| `camera.get_stats()` | `get_camera_stats()` → dict | camera_type, fps, VO metrics (features, tracking quality, confidence, distance) |
| Frame data | `get_frame_data()` → bytes | Raw grayscale pixel buffer (width×height bytes) |
| MAVLink stats | `get_mavlink_stats()` → dict | state, transport, fc_type, fc_firmware, message counts |
| Sensor modes | `get_sensor_modes()` → dict | hardware vs simulated per sensor |
| Commands | `send_command(cmd, p1, p2)` → bool | Routes flight commands to C++ runtime |
| Build info | `get_build_info()` → dict | Compiler version, build date |

**Bridge wrapper:** `backend/native_bridge.py` — `NativeRuntime` class wraps `jtzero_native.Runtime` with the same interface as `JTZeroSimulator`. Server imports one or the other transparently.

**VO Fallback (Python-side):** `NativeRuntime.vo_fallback_tick()` monitors CSI brightness using a rolling window; if brightness drops below threshold (`_VO_BRIGHT_DROP = 20`), switches secondary USB thermal camera to PRIMARY for VO. Uses Pillow for JPEG→grayscale decoding (numpy explicitly excluded for Pi Zero memory budget).

**Simulator mode toggle:** `_native.Runtime.set_simulator_mode(bool)` — C++ runtime switches between real hardware and in-memory simulation.

---

## FastAPI Backend Endpoints

**Server:** `backend/server.py`
**Base URL on Pi:** `http://jtzero.local:8001`
**CORS origins (default):** `localhost:3000`, `localhost:8001`, `jtzero.local:8001` (override via `JTZERO_ALLOWED_ORIGINS`)

### REST Routes

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/health` | Runtime status, mode (native/simulator), build info, uptime |
| GET | `/api/state` | Full `SystemState` dict from runtime |
| GET | `/api/hardware` | Hardware detection: I2C, SPI, UART, sensor detection results |
| GET | `/api/diagnostics` | Cached hardware scan + live MAVLink status |
| POST | `/api/diagnostics/scan` | Trigger fresh hardware diagnostics scan |
| GET | `/api/sensors` | Per-sensor mode: hardware vs simulated |
| GET | `/api/events?count=N` | Filtered, deduplicated event log (max N entries) |
| GET | `/api/telemetry` | state + thread stats + engine stats |
| GET | `/api/telemetry/history?count=N` | Historical telemetry ring buffer |
| GET | `/api/threads` | Thread stats for all 8 runtime threads |
| GET | `/api/engines` | Engine counters (events, reflexes, rules, memory, output) |
| GET | `/api/camera` | Primary camera stats (type, fps, VO metrics) |
| GET | `/api/cameras` | All camera slots (primary + secondary) |
| GET | `/api/camera/frame` | Latest primary camera frame as PNG (stdlib zlib encoder, no Pillow required) |
| GET | `/api/camera/secondary/stats` | Secondary (thermal) camera stats |
| GET | `/api/camera/secondary/frame` | Secondary camera frame as JPEG or PNG |
| POST | `/api/camera/secondary/capture` | On-demand capture from thermal camera |
| GET | `/api/camera/features` | Current VO feature point positions |
| GET | `/api/vo/profiles` | Available VO hardware profiles (Light / Balanced / Performance) |
| POST | `/api/vo/profile/{id}` | Activate a VO profile |
| GET | `/api/vo/trail` | VO position trail for 3D dashboard visualization |
| GET | `/api/mavlink` | MAVLink connection state, FC type/firmware, message counts |
| GET | `/api/performance` | Engine perf + system metrics (CPU, RAM, temp) |
| GET | `/api/simulator/config` | Current simulator parameters |
| POST | `/api/simulator/config` | Set simulator wind, noise, battery drain, physics params |
| POST | `/api/command` | Send command to runtime (`command`, `param1`, `param2`) |
| GET | `/api/logs/status` | Flight log recording state, session info |
| POST | `/api/logs/password` | Set/update flight log password (min 12 chars) |
| POST | `/api/logs/start` | Start recording flight log (password required) |
| POST | `/api/logs/stop` | Stop recording flight log |
| POST | `/api/logs/sessions` | List all sessions (password required) |
| POST | `/api/logs/read` | Decrypt and return a specific session (password + filename) |

### WebSocket Routes

| Path | Push rate | Payload |
|------|-----------|---------|
| `/api/ws/telemetry` | 10 Hz | Full telemetry snapshot: state, threads, engines, events, camera, MAVLink, features, sensor_modes, system_metrics, cameras |
| `/api/ws/events` | On change (~5 Hz) | New event list when event count changes |

### Static File Serving

Built React frontend is served from `backend/static/`. When `backend/static/index.html` exists, a catch-all route serves the React SPA; all non-API paths return `index.html`.

---

## MAVLink v2 — ArduPilot Flight Controller

**Implementation:** `jt-zero/mavlink_interface.cpp` + `jt-zero/include/jt_zero/mavlink_interface.h`

**Protocol:** MAVLink v2 framing (raw byte-level implementation — no external MAVLink library linked; CRC-extra table embedded)

**Transport auto-detection (priority order):**
1. Serial UART: `/dev/ttyAMA0` → `/dev/serial0` → `/dev/ttyS0` (Pi hardware UART, auto-baud)
2. UDP: `127.0.0.1:14550` (SITL / MissionPlanner / QGroundControl)
3. Simulated: in-memory (development, no FC attached)

**Messages sent to FC (outbound):**
- `VISION_POSITION_ESTIMATE` (msg_id 102) — VO-derived position estimate
- `ODOMETRY` (msg_id 331) — full odometry with velocity
- `OPTICAL_FLOW_RAD` (msg_id 106) — optical flow measurements

**Messages received from FC (inbound):**
- `HEARTBEAT` (msg_id 0) — connection keepalive, FC type/firmware detection
- `ATTITUDE` (msg_id 30) — roll/pitch/yaw
- `GLOBAL_POSITION_INT` (msg_id 33) — GPS position
- `GPS_RAW_INT` (msg_id 24) — raw GPS data
- `SCALED_IMU` (msg_id 26) — IMU data from FC
- `RC_CHANNELS` (msg_id 65) — RC input
- `VFR_HUD` (msg_id 74) — airspeed, groundspeed, alt
- `COMMAND_LONG` (msg_id 76) — commands from GCS
- `STATUSTEXT` (msg_id 253) — FC status messages

**FC detection:** FC type and firmware version parsed from `HEARTBEAT` autopilot/type fields; exposed via `/api/mavlink`.

---

## Camera Integrations

### Primary — Raspberry Pi CSI Camera

**Driver:** `jt-zero/camera/camera_drivers.cpp`, class `PiCSICamera`

**Interface:** MIPI CSI via libcamera stack (`rpicam-vid` subprocess)

**Detection:** `rpicam-hello --list-cameras` — parses output for 8 known sensor chip IDs:
`OV5647`, `IMX219`, `IMX477`, `IMX708`, `OV9281`, `IMX296`, `OV64A40`, `IMX290` — plus GENERIC fallback for any libcamera-compatible sensor

**Capture:** `rpicam-vid` spawned as subprocess, YUV420 output → grayscale 320×240 (Pi Zero 2W profile)

**Pi Zero 2W profile:** 320×240 @ 15 fps, focal length 554.0 px

**Purpose:** PRIMARY camera — forward-facing Visual Odometry (VO)

### Secondary — USB Thermal Camera (MS210x / Caddx 256)

**Driver (C++):** `jt-zero/camera/camera_drivers.cpp`, class `USBCamera` — V4L2 MMAP streaming with 4 buffers, `select()` timeout, YUYV → grayscale conversion

**Driver (Python):** `backend/usb_camera.py`, class `USBCameraCapture` — `v4l2-ctl` subprocess-based batch capture; MJPEG format; reopens device each capture to force fresh analog frame from MS210x

**Detection:** `v4l2-ctl --list-devices` — finds first USB device at `/dev/video*`

**Format:** MJPEG preferred; YUYV fallback

**Resolution:** 640×480 if supported, otherwise largest MJPEG resolution available

**Purpose:** SECONDARY camera — thermal scanning downward; VO fallback when CSI is dark/blocked

**VO Fallback logic (`backend/native_bridge.py`):**
- Background monitor runs at ~10 Hz measuring CSI frame brightness
- Rolling window average below `_VO_BRIGHT_DROP = 20` triggers switch to thermal as PRIMARY
- Cooldown period prevents thrashing; returns to CSI after brightness recovers

### Frame Pipeline (C++ side)

**Algorithms implemented natively (no OpenCV):**
- FAST-9 corner detector — primary feature detector
- Shi-Tomasi grid corner detector with Sobel gradients — fallback for low-contrast scenes (activates when FAST finds < 15 features)
- Lucas-Kanade sparse optical flow — feature tracking between frames
- Visual Odometry estimator — computes drone displacement from feature motion
- NEON SIMD acceleration — `jt-zero/camera/neon_accel.h`; 16-pixel-wide vectorized brightness and Sobel operations (`arm_neon.h`); scalar fallback for x86 dev

---

## Flight Log Storage

**Implementation:** `backend/flight_log.py`

**Storage:** Local SD card at `~/jt-zero/logs/` — one `.jtzlog` file per session

**Format:** Encrypted JSON lines (~3–5 KB/s, ~10–18 MB/hour)

**Encryption:** AES-256 via Fernet (`cryptography` library); key derived from user password via PBKDF2-HMAC-SHA256 (100k iterations); password stored as SHA-256 hash in `~/jt-zero/config.json`

**Contents per record:** telemetry snapshot (state, sensors, VO position), point cloud (VO feature positions), system metrics

**API access:** All session operations require password (`/api/logs/*`)

---

## Diagnostics Scanner

**Implementation:** `backend/diagnostics.py`

**What it scans:**
- CSI camera: `rpicam-hello --list-cameras`
- USB cameras: `/dev/video*` enumeration
- I2C bus: `i2cdetect -y 1` — resolves addresses to known sensors (IMU at 0x68, baro at 0x76/0x77, compass at 0x1E, rangefinder at 0x29, etc.)
- UART ports: checks `/dev/ttyAMA0`, `/dev/serial0`, `/dev/ttyS0`
- GPIO: checks `/sys/class/gpio`
- MAVLink: live status from runtime

**Caching:** Results cached for 60s to avoid repeated hardware scans; MAVLink status always refreshed live.

---

## GitHub Actions CI/CD

**Workflow:** `.github/workflows/build-frontend.yml`

**Trigger:** Push to any branch when files under `frontend/src/**`, `frontend/public/**`, `frontend/package.json`, or frontend config files change

**Runner:** `ubuntu-latest`

**Steps:**
1. `actions/checkout@v4`
2. `actions/setup-node@v4` — Node.js 20, npm cache keyed on `frontend/package-lock.json`
3. `npm install --no-audit --no-fund` + `npm run build` (CRA production build, `REACT_APP_BACKEND_URL=""`)
4. Copy `frontend/build/` → `backend/static/`
5. Git commit and push `backend/static/` with message `ci: rebuild frontend [skip ci]`

**Effect:** Built static assets are committed to the repo so the Pi can pull them without a Node.js install. The Pi serves the static bundle directly from `backend/static/` via FastAPI.

---

## External APIs & Services

The full `backend/requirements.txt` includes SDKs for OpenAI, Google Generative AI, LiteLLM, Stripe, and boto3 (AWS). None of these appear in any runtime code path (`server.py`, `native_bridge.py`, `simulator.py`, `flight_log.py`, `diagnostics.py`). They are present in the dev environment but are not part of the deployed drone system.

**No cloud telemetry or remote monitoring integrations are active in the runtime.**

---

*Integration audit: 2026-04-12*
