# CLAUDE.md — JT-Zero Runtime Technical Reference

> ## MANDATORY RULES FOR ALL AGENTS
> 0. **Read Worklog.md first.** At the start of every session read `Worklog.md` — it contains what was done, why, and what is pending. Do NOT start working without reading it.
> 1. **User language: Ukrainian.** Always respond in Ukrainian.
> 2. **Bug fixes → document here.** Every bug fix MUST be appended to "Key Bug Fixes" section with: root cause, WHY it failed, WHAT was changed, file:line. Format: `N. **Title** — explanation.` Do NOT write vague descriptions like "fixed bug". Write exactly what was wrong and why the fix works.
> 3. **Frontend build rule:** `export REACT_APP_BACKEND_URL="" && yarn build`. If built with Emergent Preview URL, the Pi dashboard will break.
> 4. **Venv:** Service runs via `backend/venv/bin/uvicorn`. Python packages MUST be installed into venv (`venv/bin/pip install`), NOT system Python. `apt install python3-*` does NOT work for the service.
> 5. **No numpy.** Too heavy for Pi Zero. Use Pillow or standard library only.
> 6. **Update PRD.md + CHANGELOG.md** on every finish. Update CLAUDE.md on significant changes.
> 7. **Hardware testing:** Code in Emergent hits the simulator only. Real bugs only appear on the user's physical Pi. Ask for `journalctl -u jtzero` logs when debugging hardware issues.

## Project Overview
JT-Zero is a real-time robotics runtime for lightweight drone autonomy on Raspberry Pi Zero 2 W.

**Architecture:** Multi-threaded C++ core → pybind11 → FastAPI backend → React dashboard

**Runtime Mode:** Native C++ (primary) or Python Simulator (fallback)

**Current Status (March 2026):** Full VO pipeline verified on hardware. MAVLink connected to ArduPilot FC. VISION_POSITION_ESTIMATE delivered at 25Hz. EKF3 ExternalNav integration active. **Multi-camera architecture** — CSI forward (VO) + USB thermal (downward). **8 known CSI sensors + GENERIC fallback**. **Real USB camera capture** via v4l2-ctl subprocess (architecture-independent). **USB thermal live streaming** — MJPEG batch capture (~5fps), frame cache invalidation fixed. **GitHub Actions CI/CD** auto-builds frontend — Pi Zero needs no Node.js. **VO Fallback** — automatic switch to USB thermal camera when CSI goes dark (brightness < 20). Uses brightness-only trigger (NOT confidence — FAST detector tracks noise in darkness). Feature overlay on ThermalPanel during fallback. Brightness-based auto-recovery (brightness >= 30 OR confidence >= 0.20). **SET HOMEPOINT** — VO origin reset via Commands panel button, RC channel (ch8 PWM >= 1700), or API. **3D Trail** — VO position history visualization in Dashboard 3D View (cyan trail line + amber HomeMarker). **ARM NEON SIMD** — frame brightness, Sobel gradients, Shi-Tomasi structure tensor, SAD 8x8 accelerated via NEON intrinsics (auto-fallback to scalar on x86). **MAVLink Diagnostics** — RC Channels (18ch PWM bars), FC Telemetry, Message Types, Link Stats panel. **RC_CHANNELS parsing** (msg 65). **MAVLink STATUSTEXT** — sends critical events (VO FALLBACK, CSI RECOVERED, SET HOMEPOINT) to Mission Planner at any radio range. **Encrypted Flight Log** — AES-256 Fernet, PBKDF2 100k iterations, records telemetry + point cloud (camera pose + features for 3D landscape reconstruction). Password-protected API access. **System Constraints** — CPU ≤55% (alert 70%), RAM ≤180MB (alert 250MB) for Pi Zero 2W thermal stability.

---

## Thread Model (8 threads)

| Thread | Name        | Hz    | Function                                           |
|--------|-------------|-------|-----------------------------------------------------|
| T0     | Supervisor  | 10    | System health, battery, failsafe, mode transitions  |
| T1     | Sensors     | 200   | IMU, Baro, GPS, Rangefinder, Optical Flow           |
| T2     | Events      | 200   | Event queue processing, prioritization              |
| T3     | Reflex      | 200   | Reflexes — instant reactions (obstacle avoidance)   |
| T4     | Rules       | 20    | Rules engine — condition-based state transitions    |
| T5     | MAVLink     | 50    | MAVLink v2 communication with flight controller     |
| T6     | Camera      | 15    | Frame capture + Visual Odometry (FAST + LK)        |
| T7     | API Bridge  | 30    | pybind11 ↔ Python data sync                        |

**Thread communication:** Lock-free SPSC ring buffers between threads.  
**Memory:** Lock-free O(1) MemoryPool using CAS (Compare-And-Swap).

---

## Sensor Hardware Auto-Detection

On startup, the runtime probes hardware interfaces:

| Sensor          | Bus     | Address | Auto-detect Method          |
|-----------------|---------|---------|------------------------------|
| MPU6050 (IMU)   | I2C-1   | 0x68    | i2cdetect probe              |
| BMP280 (Baro)   | I2C-1   | 0x76    | i2cdetect probe              |
| NMEA GPS        | UART    | 9600    | /dev/ttyS0 availability      |
| Rangefinder     | I2C/UART| varies  | Bus scan                     |
| PMW3901 (Flow)  | SPI0    | CS0     | SPI device probe             |

**Fallback:** If no hardware detected → automatic simulation mode. No manual config needed.

---

## Camera Pipeline

### Multi-Camera Architecture (Variant B)

**Priority logic:** CSI has priority, USB is fallback.

| CSI | USB | Result |
|-----|-----|--------|
| Found | Found | CSI=PRIMARY(VO), USB=SECONDARY(thermal/aux) |
| Found | None | CSI=PRIMARY(VO), no SECONDARY |
| None | Found | USB=PRIMARY(VO fallback for testing) |
| None | None | SIMULATED=PRIMARY(VO) |

**Detection:** `initialize_multicam()` in `camera_pipeline.cpp`:
1. `PiCSICamera::detect_sensor()` — parses `rpicam-hello --list-cameras`, identifies sensor chip ID
2. `CameraPipeline::find_usb_device()` — scans `/dev/video0..9`, checks V4L2 capabilities for `uvcvideo` driver
3. Assigns slots based on priority table above

**Supported CSI Sensors (auto-detected):**

| Sensor | Camera | Resolution | FOV | AF | Global Shutter |
|--------|--------|-----------|------|-----|----------------|
| OV5647 | Pi Camera v1 | 5MP | 62° | No | No |
| IMX219 | Pi Camera v2 | 8MP | 62° | No | No |
| IMX477 | Pi HQ Camera | 12.3MP | lens-dependent | No | No |
| IMX708 | Pi Camera v3 | 12MP | 66° | Yes | No |
| OV9281 | OV9281 | 1MP | 80° | No | Yes (ideal for VO) |
| IMX296 | Pi GS Camera | 1.6MP | 49° | No | Yes |
| OV64A40 | Arducam 64MP | 64MP | 84° | Yes | No |
| IMX290 | IMX290 STARVIS | 2MP (1920x1080) | 82° | No | No (excellent low-light) |

**GENERIC fallback:** If `rpicam-hello` detects a CSI camera not in the list above, JT-Zero assigns it as `GENERIC` and still uses it for VO with default focal length (from PlatformConfig). The raw sensor name from rpicam-hello is stored and displayed in the dashboard.

**Camera streams:**

| Camera | Interface | Role | Stream Mode | Pi Load |
|--------|-----------|------|-------------|---------|
| CSI (Forward) | CSI → GPU/ISP via libcamera | Visual Odometry | Continuous (15fps) | Low (GPU ISP) |
| USB Thermal (Down) | USB 2.0, v4l2-ctl subprocess | Thermal + VO fallback | Continuous (background thread) | Low (256x192) |
| Analog FPV | Analog VTX | Pilot view | Bypasses Pi | None |

**USB Camera Implementation:**
- `backend/usb_camera.py` — Pure Python, uses `v4l2-ctl --stream-mmap` subprocess (no raw ioctl — works on arm32/arm64/x86)
- `find_usb_camera()` — parses `v4l2-ctl --list-devices` output (Python `ioctl(VIDIOC_QUERYCAP)` fails on aarch64 Pi due to struct size mismatch)
- `USBCameraCapture` — **batch capture** (`v4l2-ctl --stream-count=2`): each call reopens device, forcing fresh analog-to-digital conversion. MS210x capture card repeats same frame when device stays open (persistent stream), batch reopen fixes this.
- Frame format: MJPEG only (YUYV returns all-zero data on MS210x hardware)
- Resolution: 640x480. FPS: ~5fps with batch capture (0.2s per grab)
- Both `native_bridge.py` and `simulator.py` auto-detect USB cameras at init via `usb_camera.py`
- If no USB camera found — gracefully degrades to "not connected" (no fake data)
- **Frame cache bug (FIXED)**: `get_secondary_frame_data()` must update `frame_count` in camera state dict, otherwise server cache never invalidates

**API endpoints for multi-camera:**

| Method | Path | Description |
|--------|------|-------------|
| GET | /api/cameras | List all camera slots (PRIMARY, SECONDARY) |
| GET | /api/camera | Primary (VO) camera stats |
| GET | /api/camera/frame | Primary camera frame (PNG) |
| GET | /api/camera/secondary/stats | Thermal camera stats |
| POST | /api/camera/secondary/capture | Trigger on-demand thermal capture |
| GET | /api/camera/secondary/frame | Thermal camera frame (JPEG or PNG) |

---

## VO Fallback — Automatic Camera Switching

When the CSI camera loses tracking (darkness, fog, featureless surface), the system automatically switches Visual Odometry to the USB thermal camera.

### State Machine

```
CSI_PRIMARY (normal) ──[rolling avg brightness < 20]──> THERMAL_FALLBACK
                                                        │
THERMAL_FALLBACK ──[CSI probe brightness OK]──────────> CSI_PRIMARY
```

### Configuration

| Parameter | Value | Description |
|-----------|-------|-------------|
| `BRIGHT_DROP` | 20 | Switch to fallback when rolling avg brightness drops below this |
| `BRIGHT_RECOVER` | 30 | Return to CSI when probe brightness exceeds this |
| `CONF_RECOVER` | 0.20 | Secondary recovery: confidence above this (lowered for dim environments) |
| `WINDOW_SIZE` | 10 | Rolling average window for brightness samples |
| `MIN_SAMPLES` | 8 | Minimum brightness samples before switching |
| `MIN_FALLBACK_S` | 3.0 | Minimum seconds in fallback before checking recovery |
| `PROBES_TO_RECOVER` | 1 | Consecutive good CSI probes needed to recover |
| `COOLDOWN_S` | 5.0 | Cooldown after recovery before re-triggering |

### CRITICAL: Brightness-Only Trigger

**DO NOT use confidence for the fallback trigger.** On the Pi Camera v2, the FAST detector tracks sensor noise in pitch darkness — causing confidence to read as HIGH (~70%) when the camera is completely blocked, but LOW during normal scene transitions. Only brightness is reliable:
- `avg_brightness < 20` → camera is dark/blocked → trigger fallback
- Confidence is only used for CSI recovery probes (scene quality, not darkness detection)

### Switch Logic (Hybrid Python/C++ Architecture)

The VO Fallback uses a **hybrid** approach because the USB thermal camera hardware (MS210x capture card) only works with MJPEG via v4l2-ctl subprocess (managed by Python `usb_camera.py`), while the C++ `USBCamera` class uses YUYV which returns all-zero frames on this hardware.

**Python side** (`native_bridge.py`):
1. `vo_fallback_tick()` called at 10Hz from background asyncio task (independent of WebSocket)
2. Monitors `frame_brightness` (NOT confidence!) from C++ camera stats
3. Maintains rolling average brightness over 10 samples
4. When avg brightness < 20: calls `activate_fallback()` + starts injection thread
5. Injection thread: captures JPEG from `usb_camera.py` → decodes to grayscale via Pillow → calls `inject_frame()` at ~5fps
6. Monitors CSI recovery probes from C++ → calls `deactivate_fallback()` when 3 consecutive probes show good quality

**C++ side** (`camera_pipeline.cpp`):
1. `activate_fallback(reason)` — sets fallback state, resets VO with thermal focal length (180px)
2. `tick()` in fallback mode: reads injected frame via atomic SPSC buffer → runs full VO pipeline (FAST/Shi-Tomasi + LK + Kalman)
3. Periodic CSI probe (every 3s): captures one CSI frame, calculates avg brightness, reports probe quality (brightness-normalized)
4. `deactivate_fallback()` — restores CSI camera, resets VO with CSI focal length
5. `inject_frame(data, w, h)` — thread-safe SPSC: Python writes when state=0, T6 reads when state=2

### Feature Overlay on Thermal Panel

During VO Fallback, the React `ThermalPanel.js` displays **real** VO feature positions:
- Orange tracked squares + yellow detected circles, scaled from 320x240 VO resolution to 640x480 canvas
- VO displacement vector (orange arrow from center)
- VO stats bar: DET, TRK, CONF, PTS (real feature count), FALLBACK indicator
- Features redraw on JPEG frame load AND when WebSocket data updates (dual trigger)

**Thread-safe feature snapshot** (`features_snapshot_` in CameraPipeline):
- After each `vo_.process()` in `tick()`, features are copied to `features_snapshot_[]` with `std::memory_order_release`
- Python `get_features()` reads via `features_snapshot_count()` with `std::memory_order_acquire`
- This guarantees cross-thread visibility on ARM64 (the original `vo_.features()` had no memory barrier, causing empty reads from the Python thread)

### Hardware Constraints (Pi Zero 2 W)

- USB bus shared between thermal camera and WiFi → thermal capture adds ~3ms CPU per frame
- Both cameras cannot do simultaneous VO (CPU overload on 4× Cortex-A53 @ 1GHz)
- Fallback design: **one active VO source at a time**, other camera idle or probe-only
- USB thermal FOV is narrower (typical 25-50°) → fewer trackable features, lower VO confidence expected

---

## How Visual Odometry Works — Full Flight Cycle

### What is Visual Odometry (VO)?

VO measures drone displacement by comparing consecutive camera frames. The CSI camera faces **forward** and tracks features in the landscape (buildings, trees, terrain) — if features shift left in the frame, the drone moved right. The USB thermal camera faces **downward** and can serve as VO fallback when the CSI camera loses tracking (night, fog, featureless sky).

### Full Flight Cycle

```
TAKEOFF → CLIMB → ROUTE → RETURN (RTL) → LANDING
```

#### Step 1: Takeoff

The system initializes:
1. C++ Runtime starts 8 threads
2. Camera Thread (T6) opens CSI camera (rpicam-vid)
3. MAVLink Thread (T5) connects to flight controller (Matek H743)
4. VO initializes: first frame stored as "previous"
5. Position = (0, 0, 0) — launch point = reference origin

#### Step 2: VO Computation (every 66ms, 15fps)

```
Frame N-1 (previous)          Frame N (current)
┌─────────────────────┐       ┌─────────────────────┐
│    ·  rock           │       │         ·  rock      │
│  ·  bush             │  →→→  │       ·  bush        │
│        · trail       │       │             · trail   │
└─────────────────────┘       └─────────────────────┘
                                 
  Rock moved by (dx, dy) pixels →
  Drone moved by (-dx, -dy) × scale
```

**Algorithm steps:**

**a) Feature Detection (FAST-9 + Shi-Tomasi)**
- FAST-9: checks 16 pixels in a circle around each point, fast
- Shi-Tomasi: fallback for low-contrast (thermal) — computes structure tensor eigenvalues

**b) Feature Tracking (Lucas-Kanade Optical Flow)**
- Takes points from frame N-1, finds them in frame N
- Uses Sobel 3x3 gradients and iterative sub-pixel refinement with bilinear interpolation

**c) Outlier Filtering (Median + MAD)**
- Computes median displacement of all tracked points
- MAD (Median Absolute Deviation) — rejects outliers (>2σ from median)
- Remaining = "inliers" — reliable points

**d) Real-World Displacement**
```
dx_meters = dx_pixels × altitude / focal_length
dy_meters = dy_pixels × altitude / focal_length
```

**e) IMU Pre-integration for LK Hints**
- T1 (200Hz) accumulates gyro rotation between T6 frames (thread-safe mutex)
- ~13 IMU samples per camera frame (66ms @ 200Hz)
- `shift_x = focal * dgz` (yaw → lateral), `shift_y = -focal * dgy` (pitch → vertical)
- LK tracker initializes flow search at predicted position instead of (0,0)
- Activates only when |shift| > 0.3px to avoid polluting stationary tracking

**f) Kalman Filter (simplified EKF)**
- Predict step uses IMU: `kf_vx += imu_ax * dt` (physics-based prediction)
- Update step uses VO measurement (corrects IMU prediction)
- Adaptive noise: process noise Q from `kalman_q`, measurement noise R from `kalman_r_base / inlier_ratio`
- Position uncertainty: `sqrt(pose_var_x + pose_var_y)` from accumulated KF covariance (not ad-hoc)

**g) IMU Consistency Validation**
- Compares `ΔV_VO = raw_vx - kf_vx_prev` with `ΔV_IMU = imu_ax * dt`
- Large discrepancy → lower `imu_consistency` → lower confidence
- Gate: only runs when `imu_hint_valid_` (set by `camera_.set_imu_hint()` in T6)

**f) Sending to ArduPilot**
```
VISION_POSITION_ESTIMATE @ 25Hz → MAVLink → Matek H743 → EKF3
```
- ArduPilot EKF3 fuses: GPS + VO + IMU + Baro + Compass
- VO provides position estimate with covariance (lower confidence → higher covariance)

#### Step 3: Navigation

ArduPilot knows current position (from EKF3) and target waypoint — the difference = movement vector → motor commands. **VO helps when GPS is degraded** (urban canyons, forests, interference).

#### Step 4: Return To Launch (RTL)

EKF3 knows full position from combined GPS + VO + IMU + Baro → computes vector home.

**Drift:** VO accumulates error over time (~±5m in 10min flight). GPS corrects absolute position, VO provides precise inter-frame displacement, EKF3 optimally fuses both.

#### Step 5: Landing

- Rangefinder provides precise altitude <2m
- VO becomes very accurate near ground (more features, smaller scale)
- Precision landing: VO can track landing pad markers

---

## Why VO When GPS Exists?

| Situation | GPS | VO |
|-----------|-----|-----|
| Open field | Works | Works |
| Between buildings | Signal reflection (±10m) | Works perfectly |
| Under bridge/canopy | Disappears | Works |
| Dense forest | Poor (±5m) | Works |
| Indoor (warehouse) | No signal | **Only navigation source** |
| GPS jamming (warfare) | Disabled | **Only navigation source** |

---

## Use Cases

1. **Precision Agriculture** — thermal camera down, VO for precise field positioning, thermal map shows moisture/disease/irrigation zones
2. **Search & Rescue (SAR)** — night flight with thermal, VO navigation without GPS (forest, mountains, caves), autonomous grid search
3. **Infrastructure Inspection** — bridges, power lines, towers: VO holds stable position for photos; thermal finds overheated contacts, heat leaks; solar panels: finds defective cells
4. **Indoor Navigation** — warehouses, mines: GPS=0, VO is the only position source; autonomous building mapping; warehouse inventory
5. **Cartography & 3D Modeling** — series of images with precise VO coordinates → photogrammetry → 3D model
6. **Security & Patrol** — autonomous perimeter route; thermal detects people/animals; VO for stable flight between buildings

---

## MAVLink Interface

**Transport cascade:** Serial → UDP → Simulation

| Transport  | Config                    | Use Case                    |
|-----------|----------------------------|-----------------------------|
| Serial    | auto-detect port + baud   | Direct FC UART connection   |
| UDP       | 127.0.0.1:14550           | SITL / QGC / MissionPlanner |
| Simulated | In-memory                 | Development & testing       |

**Auto-baud detection (CRC-validated):**
Probes 115200 → 921600 → 57600 → 230400 → 460800. For each rate, reads ~1.5s of data and searches for complete MAVLink frames with valid CRC-16/MCRF4XX. Only frames with **known CRC extra** are counted — eliminates false positives from garbage bytes at wrong baud rates. Falls back to 115200 (ArduPilot default) if no valid frames found.

**Messages sent to FC:**
- `VISION_POSITION_ESTIMATE` (#102) @ 25Hz — accumulated VO pose, NED frame
- `ODOMETRY` (#331) @ 25Hz — full 6DOF with quaternion
- `OPTICAL_FLOW_RAD` (#106) — integrated flow + gyro
- `HEARTBEAT` (#0) @ 1Hz — companion computer heartbeat (MAV_TYPE_ONBOARD_CONTROLLER)

**MAVLink parser features:**
- CRC-16/MCRF4XX validation on all received frames (rejects corrupt/garbled data)
- MAVLink v1 (0xFE) and v2 (0xFD) support
- MAVLink v2 signing detection (incompat_flags bit 0 → +13 bytes signature)
- MAVLink v2 zero truncation handling (heartbeat min payload 5 bytes, not 9)
- Diagnostic: raw byte hex dump on first data, per-heartbeat logging (first 10)
- API counters: bytes_sent, bytes_received, heartbeats_received, crc_errors, detected_msg_ids

**Verified hardware configurations:**
- Pi Zero 2W + Matek H743 @ 115200 baud — CONNECTED, HB:12, ArduPilot QUADROTOR
- VISION_POSITION_ESTIMATE arriving at FC at 25Hz (confirmed in MAVLink Inspector)
- ODOMETRY arriving at FC at 25Hz
- FC telemetry flowing back: ATTITUDE, RAW_IMU, GPS_RAW_INT, VFR_HUD, SYS_STATUS

---

## API Endpoints

| Method | Path                         | Description                          |
|--------|------------------------------|--------------------------------------|
| GET    | /api/health                  | Runtime status, mode, build info     |
| GET    | /api/state                   | Full system state (attitude, sensors)|
| GET    | /api/hardware                | Hardware detection status            |
| GET    | /api/events                  | Recent event log                     |
| GET    | /api/telemetry/history       | Time-series telemetry data           |
| GET    | /api/threads                 | Thread stats (Hz, CPU, iterations)   |
| GET    | /api/engines                 | Engine stats (events, reflexes, etc) |
| GET    | /api/camera                  | Primary camera & VO pipeline stats   |
| GET    | /api/camera/frame            | Primary camera frame (PNG)           |
| GET    | /api/camera/features         | Current VO feature positions [{x,y,tracked,response}] |
| GET    | /api/cameras                 | List all camera slots                |
| GET    | /api/camera/secondary/stats  | Secondary (thermal) camera stats     |
| POST   | /api/camera/secondary/capture| Trigger on-demand thermal capture    |
| GET    | /api/camera/secondary/frame  | Secondary camera frame (JPEG/PNG)    |
| GET    | /api/vo/profiles             | Available VO mode profiles           |
| POST   | /api/vo/profile/{id}         | Switch VO mode at runtime            |
| GET    | /api/vo/trail                | VO position trail [{x,y,z,t}] for 3D visualization |
| GET    | /api/mavlink                 | MAVLink stats + RC channels + FC telemetry |
| GET    | /api/performance             | CPU, memory, latency breakdown       |
| GET    | /api/diagnostics             | Hardware diagnostics (camera, I2C)   |
| POST   | /api/diagnostics/scan        | Run fresh hardware diagnostics       |
| GET    | /api/sensors                 | Sensor modes (hardware/mavlink/sim)  |
| GET    | /api/simulator/config        | Current simulator parameters         |
| POST   | /api/simulator/config        | Update simulator parameters          |
| POST   | /api/command                 | Send command (arm, disarm, takeoff, land, rtl, hold, vo_reset) |
| GET    | /api/logs/status             | Flight log status (recording, password set) |
| POST   | /api/logs/password           | Set/update flight log password       |
| POST   | /api/logs/start              | Start encrypted flight log recording |
| POST   | /api/logs/stop               | Stop recording                       |
| POST   | /api/logs/sessions           | List all log sessions (auth required)|
| POST   | /api/logs/read               | Read & decrypt a log session (auth required) |
| WS     | /api/ws/telemetry            | Real-time telemetry @ 10Hz           |
| WS     | /api/ws/events               | Event stream                         |

### /api/mavlink Response Fields
```json
{
  "state": "CONNECTED",
  "messages_sent": 202,
  "messages_received": 2021,
  "heartbeats_received": 12,
  "bytes_sent": 27366,
  "bytes_received": 43170,
  "crc_errors": 0,
  "errors": 0,
  "msg_per_second": 42,
  "fc_type": "QUADROTOR",
  "fc_firmware": "ArduPilot QUADROTOR",
  "fc_armed": false,
  "transport_info": "/dev/ttyAMA0@115200",
  "detected_msg_ids": [30, 178, 253, 0, 77, 33, 1, 125, 152, 62, 42, 74, 27, 116, 29, 24, 65],
  "rc_channels": [1500, 1500, 1000, 1500, 1800, 1000, 1000, 1000, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
  "rc_chancount": 8,
  "rc_rssi": 255,
  "fc_telemetry": {
    "attitude_valid": true,
    "imu_valid": true,
    "baro_valid": true,
    "gps_valid": false,
    "hud_valid": true,
    "status_valid": true,
    "msg_count": 2021,
    "battery_voltage": 11.8,
    "battery_remaining": 75,
    "gps_fix": 0,
    "gps_sats": 0
  }
}
```

---

## WebSocket Telemetry Payload

```json
{
  "type": "telemetry",
  "timestamp": 1710192000.0,
  "runtime_mode": "native",
  "state": { "roll": 0.5, "pitch": -0.3, "yaw": 45.2, "altitude_agl": 7.0 },
  "threads": [ { "name": "T0_Supervisor", "actual_hz": 10.0, "running": true } ],
  "engines": { "events": {}, "reflexes": {}, "rules": {}, "memory": {}, "output": {} },
  "recent_events": [ { "timestamp": 100.5, "type": "OBSTACLE", "priority": 200, "message": "..." } ],
  "camera": { "fps_actual": 15.0, "vo_features_tracked": 21, "vo_valid": true },
  "mavlink": { "state": "CONNECTED", "messages_sent": 779, "heartbeats_received": 12 },
  "sensor_modes": { "imu": "simulation", "baro": "simulation", "gps": "simulation" }
}
```

---

## Key Bug Fixes (chronological)

> **INSTRUCTION FOR ALL AGENTS:** Every bug fix MUST be documented here using this format:
> `N. **Short title** — Root cause explanation. What was wrong, WHY it was wrong, and what was changed. Include file:line where applicable. Do NOT write "fixed bug" — write what the bug was and why the fix works.`

1. **VO displacement = 0** — Was using median pixel shift as displacement. Now: `displacement = pixel_shift * (ground_distance / focal_length)`
2. **MemoryPool race** — Replaced mutex-based pool with lock-free CAS free-list (O(1))
3. **FAST threshold overflow** — `int t = threshold_` prevents uint8_t subtraction underflow
4. **MAVLink VISION_POS** — Now uses accumulated VO local pose, not GPS coordinates
5. **MAVLink ODOMETRY** — Uses accumulated pose, not per-frame delta
6. **rand() thread safety** — Replaced with per-thread xorshift32 PRNG
7. **Roll calculation** — Fixed `atan2(acc_y, acc_z)` → `atan2(acc_y, -acc_z)` (acc_z is -9.81 when level)
8. **LK bilinear interpolation (CRITICAL)** — LK tracker used integer pixel access, preventing sub-pixel convergence. Added bilinear interpolation — fixes tracking on ALL cameras, especially low-contrast thermal
9. **Sobel 3x3 gradients** — Replaced simple central differences with Sobel operator in LK tracker. 4x signal amplification, 16x better matrix conditioning for thermal images
10. **USB camera V4L2 MMAP** — Rewrote USB camera driver from simple `read()` (fails on UVC) to proper V4L2 MMAP streaming with `select()` timeout
11. **MAVLink heartbeat filter (CRITICAL)** — Old code rejected type=0 (GENERIC) heartbeats and type=18 unconditionally. Now: only filters own echoed heartbeats (sysid+type match), GCS, and ADSB. Accepts GENERIC and all vehicle types.
12. **MAVLink CRC validation** — Parser now validates CRC-16/MCRF4XX on all received frames. Without CRC, garbage bytes from baud mismatch were counted as valid messages (RX:42 with 0 heartbeats).
13. **MAVLink auto-baud (CRITICAL)** — Old STX-counting method gave false positives (random bytes contain 0xFD/0xFE by chance). Replaced with full CRC-validated frame detection during probing. Only known messages (with CRC extra) count.
14. **MAVLink v2 zero truncation** — Heartbeat handler min length 7→5. MAVLink v2 trims trailing zeros, so heartbeats with base_mode=0 had payload len<7 and were silently dropped.
15. **MAVLink v2 signing** — Parser now detects incompat_flags bit 0, adds 13-byte signature to frame length. Without this, signed frames shifted buffer alignment and corrupted all subsequent parsing.
16. **USB thermal stream freezing** — MS210x AV-to-USB capture card hangs when `subprocess.Popen` holds the device open continuously. Replaced with sequential batch capture: open → capture N frames → close → sleep → repeat. Each batch is a fresh `v4l2-ctl` subprocess. `usb_camera.py`.
17. **Server frame cache stale** — `native_bridge.py` fetched thermal frames but never incremented `frame_count`, so `server.py` served the same cached frame indefinitely. Fix: increment `frame_count` on every successful fetch in `native_bridge.py`.
18. **ThermalPanel canvas rendering** — Canvas-based JPEG rendering in `ThermalPanel.js` failed silently (createImageBitmap not reliable for all JPEG variants). Replaced with direct `<img>` tag + blob URL, matching `CameraPanel.js` pattern.
19. **Pillow import failure on Pi (CRITICAL)** — `update.sh` installed Pillow via `apt install python3-pil` into system Python (`/usr/lib/python3.13/`), but JT-Zero service runs via `backend/venv/bin/uvicorn`. Venv cannot see system packages → `PIL=False FILTERS=False NUMPY=False` → all feature detectors disabled silently. Fix: `venv/bin/pip install Pillow` in `update.sh`. PEP 668 does NOT block pip inside venv.
20. **Silent feature detection crash** — `native_bridge.py:461` had `except Exception: pass` around Pillow feature detection, numpy fallback, and raw detector. ALL errors were silently swallowed. Also `_decode_jpeg_to_gray()` returned `(b'', None)` on error without any log. Fix: replaced all silent catches with `sys.stderr.write(f"[VO PyDetect] error: {e}")` + one-time dedup flag.
21. **VO Fallback recovery stuck** — Recovery used only `probe_conf >= 0.40`, but in dim environments (brightness ~41) CSI confidence hovers at exactly 0.40, sometimes passing, sometimes not. User had to increase room brightness to trigger recovery. Fix: added brightness-based recovery path (`frame_brightness >= 30`), lowered confidence threshold to 0.20 as secondary. Recovery now uses whichever triggers first. `native_bridge.py`.
22. **Pillow Image.Resampling.NEAREST** — Older apt-installed Pillow versions may not have `Image.Resampling` enum (introduced in Pillow 9.1). Added `try: Image.Resampling.NEAREST except AttributeError: Image.NEAREST` fallback in `_decode_jpeg_to_gray()`. `native_bridge.py`.
23. **`/api/camera/features` JSON error** — Endpoint could raise unhandled exception → FastAPI returns HTML 500 error page → `curl` gets JSONDecodeError. Fix: wrapped in `try/except`, always returns `[]` on error. `server.py`.
24. **MAVLink telemetry thread-safety deadlock (CRITICAL)** — `handle_message()` acquired `telem_lock_` spinlock via `telem_lock_.exchange(true)` but NEVER released it. Every call permanently locked the spinlock, causing `get_fc_telemetry()` (called from the Sensor/API thread) to spin forever → deadlock → YAW/roll/pitch data never updates → 3D View shows random cached values interpreted as wild rotations. Fix: introduced RAII `ScopedSpinLock` struct (destructor calls `telem_lock_.store(false, memory_order_release)`). Both `handle_message()` and `get_fc_telemetry()` now use `ScopedSpinLock` — lock is guaranteed released on ALL exit paths (normal return, break, early return). `mavlink_interface.h` (ScopedSpinLock struct + get_fc_telemetry), `mavlink_interface.cpp:handle_message()`.
25. **3D View YAW full-spin on 360→0 wrap (ROOT CAUSE of visual glitch)** — `Drone3DPanel.js` used naive linear lerp `rotation.y += (target - current) * 0.06` without angle normalization. When simulator/FC yaw crosses 360°→0° (or vice versa), the delta jumps ~6.28 rad, and the lerp smoothly rotates the model through a full 360° revolution instead of the correct ~2°. Fix: normalize yaw delta to `[-PI, PI]` via `delta -= Math.round(delta / (2*PI)) * 2*PI` before lerping. The model now always takes the shortest rotational path. This was the **actual root cause** of the visual YAW glitch — the C++ thread-safety issue (fix #24) was a separate real bug but did not cause the visual spinning. `frontend/src/components/Drone3DPanel.js:19`.
26. **CORS wildcard + credentials** — `allow_origins=["*"]` combined with `allow_credentials=True` allows any device on the WiFi to call `/api/command` (arm/disarm). Fix: restrict to explicit origin list via `JTZERO_ALLOWED_ORIGINS` env var (default: localhost:3000, localhost:8001, jtzero.local:8001). Also set `allow_credentials=False`, `allow_methods=["GET","POST"]`. `server.py:110-116`.
27. **Path traversal in read_session** — `filename` from user was joined directly with `LOG_DIR` without validation. `filename="../../../etc/passwd"` would read arbitrary files. Fix: compare `os.path.realpath(filepath)` against `os.path.realpath(LOG_DIR)` — reject if path escapes log directory. `flight_log.py:read_session()`.
28. **Silent write failure: recording appears active but data is lost** — On disk-full error, `_write_record()` logged once then silently continued. `is_recording` stayed True, misleading the user. Fix: on write error set `_running=False`, close file, set `_write_failed=True`. `/api/logs/status` now exposes `write_failed` field. `flight_log.py:_write_record()`.
29. **psutil import in hot path (0.5Hz call on Pi Zero)** — `import psutil` was inside `record_telemetry()`, executed every 0.5s. Python's import machinery has a lock that can cause latency spikes. Fix: moved to module-level try/except with `_PSUTIL_AVAILABLE` flag. `flight_log.py:203`.
30. **Plaintext password in memory** — `flight_logger._password = req.password` stored the raw password string in the server process. Fix: derive the Fernet key once at session start (`_derive_key(req.password)`) and store only `_key` bytes. PBKDF2 100k iterations now run once per session, not on every 0.5s write. `server.py:start_log`, `flight_log.py:_write_record`.
31. **Minimum password length 6** — "123456" passed validation. PBKDF2 is strong but a weak password defeats it. Fix: raised minimum to 12 characters. `server.py:set_log_password`.
32. **Fixed PBKDF2 salt across all installations** — `SALT = b"jtzero-flight-log-v1"` was hardcoded, making precomputed hash tables valid on every drone. Fix: `_get_or_create_salt()` generates a 16-byte random salt on first run and stores it in `config.json`. `flight_log.py`.
33. **Silent WebSocket exceptions** — `except Exception: pass` in both WebSocket handlers discarded error context, making crashes invisible in logs. Fix: added `sys.stderr.write(f"[WS/...] Unexpected error: {e}")` before disconnect. `server.py:websocket_telemetry`, `websocket_events`.
34. **numpy used as fallback (violates CLAUDE.md rule 5)** — `native_bridge.py` imported numpy and used it as feature detector fallback despite CLAUDE.md explicitly prohibiting numpy on Pi Zero (too heavy). Fix: removed numpy import and entire numpy fallback branch. Fallback chain is now: Pillow → pure Python. `native_bridge.py:33-35,548-574`.
35. **InvalidToken not distinguished from corrupted file** — `read_session()` caught all exceptions equally and returned `[]`, making wrong password indistinguishable from corrupted data. Fix: catch `InvalidToken` separately and return `None`; server returns `{"error":"Wrong password"}` vs `{"error":"Corrupted or empty file"}`. `flight_log.py:read_session`, `server.py:read_session`.
36. **imu_consistency compared wrong quantities** — Phase 3 computed `actual_dvx = raw_vx - kf_vx_` (post-update Kalman residual) and compared it with `imu_ax * dt` (velocity delta). These are different physical quantities: residual is `measurement - state`, delta-v is `v[t] - v[t-1]`. The comparison was always near-zero after Kalman update, making `imu_consistency` near-1.0 regardless of actual IMU/VO disagreement. Fix: snapshot `kf_vx_prev_` BEFORE predict step; `actual_dvx = raw_vx - kf_vx_prev_`. Scaling `*0.5 → *5.0` (1 m/s² over 66ms = 0.066 m/s delta). `camera_pipeline.cpp:Phase3`.
37. **Attitude derived from accelerometer only — gyro unused for roll/pitch** — `sensor_loop` used `roll = atan2(acc_y, acc_z)` (pure accelerometer: noisy on HF, corrupted during acceleration). `yaw += gyro_z * dt` — naked integration with no bias correction, drifting up to 3°/min. Fix: complementary filter alpha=0.98 for roll/pitch; on-ground gyro_z bias estimation (EMA BIAS_ALPHA=0.0005, gate `!armed && |gyro| < 0.05 rad/s`). `runtime.cpp:sensor_loop`. NOTE: applies only in non-FC mode; FC telemetry path unchanged.
38. **Gyro drift not separated from true yaw drift in hover correction** — `update_hover_state` estimated yaw drift entirely from optical flow (median_dx / focal), mixing true scene drift with gyro bias. Fix: during confirmed hover with `|gyro_z| < 0.3 rad/s`, estimate `hover_.gyro_z_bias` via EMA (BIAS_ALPHA=0.005). Subtract bias from drift rate: `corrected_rate = optical_rate - gyro_z_bias`. `camera_pipeline.cpp:update_hover_state`. New field `gyro_z_bias` and constant `BIAS_ALPHA` in `HoverState`.
39. **set_imu_hint() never called — IMU cross-validation silently dead** — `VisualOdometry::set_imu_hint()` existed since the beginning but was NEVER called from `runtime.cpp:camera_loop()`. This means `imu_hint_valid_` was always false, making Phase 3 (imu_consistency) always 1.0, and the IMU prediction step (Fix 39) never activating. Fix: added `camera_.set_imu_hint(state_.imu.acc_x, state_.imu.acc_y, state_.imu.gyro_z)` in `camera_loop()` BEFORE `tick()`. Also added IMU prediction in Kalman predict step: `kf_vx_ += imu_ax_ * dt` when `imu_hint_valid_`. `runtime.cpp:camera_loop`, `camera_pipeline.cpp:Phase2`.
40. **position_uncertainty was ad-hoc, not from filter state** — Formula `uncertainty = total_distance * 0.03 * (1 - confidence*0.5)` had no connection to Kalman filter state. EKF in ArduPilot could not trust it as a real covariance estimate. Fix: accumulate `pose_var_x_ += kf_vx_var_ * dt²` each frame. `position_uncertainty = sqrt(pose_var_x + pose_var_y)` (1-sigma radial, meters). Decay ×0.995/frame at confidence > 0.7; ×4 growth during dead-reckoning. `camera_pipeline.cpp:Phase4+`. New private members `pose_var_x_`, `pose_var_y_` in `VisualOdometry`.
41. **LK tracker started search at (0,0) flow — failed during inter-frame rotation** — Between T6 frames (66ms at 15Hz ≈ 13 IMU samples), gyroscope rotation was never used to seed LK's initial flow estimate. At ±10° yaw between frames, features shift ~30px on a 320px frame; LK starting at flow=(0,0) would need max_iterations to converge, often failing. Fix: T1 (200Hz) calls `camera_.accumulate_gyro(gx, gy, gz-bias, dt)` (thread-safe via `preint_mtx_`). T6 in `process()`: reads+resets pre-integration, computes `shift_x = focal * dgz`, `shift_y = -focal * dgy`, passes as `hint_dx[]`, `hint_dy[]` to `LKTracker::track()` (only when |shift| > 0.3px). LK initializes `flow_x = hint_dx[f]` instead of 0. New: `PreIntState` struct, `std::mutex preint_mtx_`, `accumulate_gyro()` method, `kf_vx_prev_/vy_prev_` members in `VisualOdometry`. New public methods `set_imu_hint()`, `accumulate_gyro()` on `CameraPipeline`. `camera.h`, `camera_pipeline.cpp`, `runtime.cpp`.
43. **SystemState data races (8 concurrent writers, no mutex) — verified 0 races via TSan** — All 8 runtime threads wrote to `SystemState state_` without synchronization. Root causes (5 separate fixes): (A) `sensor_loop` wrote `state_.range` and `state_.flow` outside the `!fc_active` block, therefore outside `sensor_mutex_` — fixed by adding per-write `sensor_mutex_` locks. (B) `update_flight_physics()` held only `slow_mutex_` while writing Zone B fields (`altitude_agl`, `baro.altitude`, `range.distance`) that `sensor_loop` also writes under `sensor_mutex_` — fixed by acquiring both mutexes in canonical order (sensor→slow). (C) `send_command()` and `rule_loop::execute()` called `update_safety_snapshot()` which reads `state_.range` while holding only `slow_mutex_` — fixed with dual lock (sensor→slow) in both callers. (D) `event_loop` passed live `state_` reference to `reflex_engine_.process()` without any lock — reflex lambdas read `battery_percent` and write `armed`/`flight_mode` concurrently with supervisor writes — fixed by wrapping `process()` call in dual lock. (E) `main.cpp:print_status()` called `rt.state()` (raw reference) from main thread — fixed by adding `Runtime::state_snapshot()` method (returns copy under dual lock) and using it in `print_status()`. Lock canonical order throughout: `sensor_mutex_`(1)→`slow_mutex_`(2)→`motor_mutex_`(3)→`emit_mutex_`(4). TSan run #5: **0 DATA RACE** in 30s sim. `runtime.cpp`, `runtime.h`, `main.cpp`.
45. **accumulate_gyro() не викликається при fc_active=true — LK hints мертві з реальним FC** — `sensor_loop` (T1) викликав `accumulate_gyro()` лише у гілці `if (!fc_active)`. З підключеним FC (Matek H743) весь блок пропускався → `preint_.valid` false завжди → TRK: 0-4 при будь-якому yaw. Fix 45a: додано `else` гілку — читаємо gyro з `state_.imu` (T5 заповнює з MAVLink) під `sensor_mutex_`, передаємо в `accumulate_gyro()`. Fix 45b: ATTITUDE msg (id=30) rollspeed/pitchspeed/yawspeed (rad/s) скопійовані в `fc_telem_.gyro_x/y/z` → `imu_valid=true`; ATTITUDE stream підвищено з 4Hz → 25Hz (REQUEST_DATA_STREAM + SET_MESSAGE_INTERVAL). **Результат підтверджено на залізі (Pi + Matek H743):** до фіксу TRK:0-4 при будь-якому yaw; після — TRK:170-176 Q:0.96-0.98 при повільному/середньому yaw (~10-60°/s). Падіння до 0 залишається лише при дуже різкому flick (features фізично виходять за межі 320px кадру за 66мс) — очікувана поведінка. `runtime.cpp:sensor_loop`, `mavlink_interface.cpp:case30`.
46. **VISION_POSITION_ESTIMATE 1Hz keep-alive викликає EKF3 cycling "stopped aiding"** — Коли `vo_valid=false`, код надсилав VISION_POSITION_ESTIMATE/ODOMETRY раз на секунду з `quality=0`. Намір: повідомити EKF3 що джерело живе але ненадійне. Реальний ефект: EKF3 отримує повідомлення → намагається fuse → провалює innovation check → "ExternalNav: stopped aiding" → через ~1с — новий пакет → цикл повторюється. Mission Planner показував "stopped aiding → initial pos NED → is using external nav data" десятки разів на хвилину. Fix: зупинити надсилання повністю коли `!vo.valid`. EKF3 один раз таймаутить → чисто переходить на IMU-only. Відновлення відбувається при першому валідному VO пакеті. `mavlink_interface.cpp:send_vo()`.
47. **T5 mavlink_loop пише Zone C (battery, armed, flight_mode) тільки під sensor_mutex_ — data race з T0** — `mavlink_loop` після отримання FC telemetry писав `battery_voltage`, `battery_percent`, `armed`, `flight_mode` (Zone C) тримаючи лише `sensor_mutex_`. T0 (supervisor) пише ці самі поля під `slow_mutex_`. Два потоки писали Zone C без спільного lock → torn write на ARM (особливо `flight_mode` enum). Fix: замінено `std::lock_guard<std::mutex> lk(sensor_mutex_)` на dual-lock у канонічному порядку (`sensor_mutex_` → `slow_mutex_`). `runtime.cpp:mavlink_loop`, рядок ~838.
48. **get_state() читає Zone C без slow_mutex_ — battery/armed/flight_mode завжди stale** — `python_bindings.cpp:get_state()` тримало лише `sensor_mutex_` і викликало `self.state()` (raw reference). Zone C поля (`battery_voltage`, `battery_percent`, `armed`, `flight_mode`) захищені `slow_mutex_`, який не брався — читання цих полів без lock. Дашборд показував stale або torn значення. Fix: замінено ручний lock + `self.state()` на `self.state_snapshot()` (бере обидва locks у канонічному порядку). `python_bindings.cpp:get_state()`, рядок ~402.
49. **get_performance() читає state_ без жодного lock; seqlock writer без reader — мертвий код** — (A) `get_performance()` мав `auto s = self.state()` (raw reference, 0 locks) — ghost variable: `s` ніде не використовувалась в lambda, але виклик без lock є UB при конкурентних записах. Fix: видалено рядок. (B) `sensor_seq_.fetch_add()` викликався двічі у T1 (рядки 355, 424) як writer side seqlock, але reader side не існував ніде в коді — 4 зайвих atomic ops/цикл без жодної користі. Аудит T6 camera_loop показав що sensor_mutex_ тримається лише ~100ns (5 float copies) до camera_.tick(), тому seqlock оптимізація не дала б нічого. Fix: видалено обидва `sensor_seq_.fetch_add()` і декларацію `sensor_seq_` з `runtime.h`. `python_bindings.cpp:get_performance()`, `runtime.cpp:sensor_loop`, `runtime.h`.
44. **Pillow відсутній у venv після fresh setup — VO Fallback ін'єкція зупиняється** — `requirements-pi.txt` не містив `Pillow` та `cryptography`. `setup.sh` встановлює залежності лише з цього файлу, тому на свіжій установці Pillow не попадав у venv. `_decode_jpeg_to_gray()` повертає `b''` (PIL=False, djpeg теж недоступний) → injection loop пропускає ін'єкцію (`if not gray_bytes: continue`) → C++ `tick()` у fallback ніколи не отримує кадр → `inject_state_` ніколи не досягає 2 → `features_snapshot_count_` залишається 0. При цьому `vo_result_.features_tracked=101` показує стале значення від останнього CSI кадру (перед fallback) — `activate_fallback()` скидає `features_snapshot_count_=0`, але `vo_result_` не скидає. Fix: додано `Pillow>=9.0.0` та `cryptography>=41.0.0` до `backend/requirements-pi.txt`. `update.sh` вже мав перевірку та встановлення Pillow (Bug Fix #19), але `setup.sh` обходив її, бо читає лише requirements файл.
50. **GPS-loss position uncertainty warning** — Коли GPS fix_type < 3 і armed=true, система не надсилала жодних сигналів оператору. Kalman-derived `position_uncertainty` (Fix #40) зростає з накопиченням дрейфу VO — фізично достовірний індикатор. Fix: `gps_warn_tick()` в `native_bridge.py`, викликається 1Hz з `_vo_fallback_monitor`. `uncertainty > 4m` → `MAV_SEVERITY_WARNING` "JT0: GPS DEGRADED unc=Xm VO only"; `uncertainty > 8m` → `MAV_SEVERITY_CRITICAL` "JT0: GPS LOST unc=Xm RTL ADVISED". Debounce 30s. Hover-safe: uncertainty не зростає в зависанні. `backend/native_bridge.py:gps_warn_tick`, `backend/server.py:_vo_fallback_monitor`.
51. **GPS warn спамить при GPS_TYPE=0 (opt-in Fix #50b)** — При `GPS_TYPE=0` (VO-only setup) `fix_type` завжди 0, `gps_warn_tick()` надсилала STATUSTEXT кожні 30с без зупинки. Fix: `_GPS_WARN_ENABLED = os.environ.get('JTZERO_GPS_WARN','0')=='1'`; за замовчуванням вимкнено. Увімкнути: `JTZERO_GPS_WARN=1` у service environment. `backend/native_bridge.py`.
52. **VO confidence нестабільний — стрибки кожні 200мс** — EMA alpha=0.3 давав кожному кадру вагу 30%, тому будь-який шумний кадр imu_consistency миттєво тягнув confidence вниз. Додатково: imu_consistency penalty множник 5.0 означав що 0.18 м/с розбіжність IMU/VO одразу давала floor=0.1. Обидві причини разом → confidence осцилював між ~0.09 і ~0.40 кожні 2-3 кадри навіть при нерухомому дроні. Fix A: alpha 0.3→0.1 (часова константа 200мс→670мс). Fix B: penalty 5.0→2.0 (те саме 0.18 м/с → imu_consistency=0.64 замість 0.1). `camera_pipeline.cpp:Phase4`.
54. **rpicam-vid AGC/AEC руйнує LK tracking** — Auto-exposure (AEC) давала bright=181 → засвітка → conf=0.00. Після Fix: `--shutter 8000` (8мс фіксована) + auto gain → нова проблема: AGC змінює pixel intensities між кадрами (bright:17→70 при включенні фонарика → conf падає з 0.17 до 0.13; bright:74→17 після → conf стрибає до 0.39). Корінь: LK обчислює градієнти на кадрі N з одним gain, трекає на кадрі N+1 з іншим gain → інтенсивності відрізняються → LK сприймає це як рух фічей → outlier rejection → conf падає. Також AGC викликає повільний дрейф conf вниз при тривалій нерухомості (~0.27→0.17 за 4хв). Final fix: `--shutter 8000 --gain 1.0` — фіксована витримка + unity gain (нативна чутливість сенсора, нуль AGC транзієнтів). gain=1.0 для денного польоту; ніч = thermal fallback. `jt-zero/camera/camera_drivers.cpp:130`.
55. **imu_consistency вбиває confidence під час будь-якого реального польоту** — `raw_confidence = track_quality × inlier_ratio × imu_consistency × feature_quality`. Під час польоту: `imu_consistency = max(0.1, 1 - discrepancy × 2.0)` де `discrepancy = |actual_dvx - expected_dvx|`. `actual_dvx = raw_vx - kf_vx_prev_` (великий при прискоренні), `expected_dvx = imu_ax × dt` (~0.003 m/s — мізерний). Результат: discrepancy = 0.4+ → imu_consistency = 0.1 при БУДЬ-ЯКОМУ русі → raw_confidence = 0.039 → EMA → conf=0.11-0.13 → vo_valid=false весь час польоту → EKF3 drift. Геометричне трекінг при цьому ідеальне (127/180 inliers). Fix A: debounce `vo_valid_stable_` (camera.h). Fix B: поріг 0.32 → 0.15. Fix C (кінцевий): прибрати `imu_consistency` з формули `raw_confidence` — він не є надійним індикатором якості VO при польоті. EKF3 сам фільтрує погані виміри через innovation gate. `raw_confidence = track_quality × inlier_ratio × feature_quality`. `camera_pipeline.cpp:777`.
53. **Kalman kf_vx_ drift при тривалій нерухомості — imu_consistency на підлозі** — Kalman gain K≈0.06 (малий через kalman_r_base=0.3 >> Q=0.0011). При acc_x=-0.069 м/с² steady-state `kf_vx_≈-0.073 м/с` навіть при нерухомому дроні. Phase 3 бачить `actual_dvx=0-(-0.073)=0.073` проти `expected=−0.0046` → discrepancy>0.18 → imu_consistency=0.1 → conf=0.09 після ~1хв нерухомості. Fix: hover decay `kf_vx_ *= 0.85f` кожен кадр коли `hover_.detected && hover_.duration>1s`. При 15fps: за 1с `kf_vx_ → initial×0.85^15≈0`. `camera_pipeline.cpp:Phase2+`.
59. **Dark spike не відловлювався — conf=0.55 при bright=1 обманює EKF3** — Bright spike filter (Fix #58) відловлював тільки `bright > avg*4`. При dark spike (`bright < avg*0.25`) — наприклад, рука перекриває камеру, тінь — VO отримує майже чорний кадр. Критично: при bright=1 LK tracker повертає conf=0.55-0.57 (трекує шум як реальні фічі). EKF3 повністю довіряє, pose стрибає. Fix: симетричний фільтр — `is_dark_spike = (bright < avg * 0.25)`. Обидва типи spike → `spike_suppress_frames_ = 45` (3с). Логіка EMA та suppress без змін. `camera_pipeline.cpp:tick()`. Для тіньових умов avg≈7, dark threshold=1.75 → bright=1 < 1.75 → caught.
58. **Bright spike → catastrophic pose jump (13м за один кадр)** — При появі яскравого об'єкта в кадрі (нога, лампа, пакет) brightness стрибає 2→23 (11×). LK tracker втрачає всі фічі → хибні flow вектори → Kalman інтегрує хибну швидкість протягом наступного 5с інтервалу → pose стрибає на 13м+. Особливо небезпечно в умовах тіні дрона (bright_rolling_avg≈2), де поріг тригера дуже низький. Fix: обчислення `frame_brightness_` перенесено ДО `vo_.process()`. EMA rolling average виключає spike-кадри. Якщо `brightness > rolling_avg * 4.0` і `rolling_avg >= 1.0` → `spike_suppress_frames_ = 45` (3с at 15fps), `vo_result_.valid = false` на весь інтервал. Ключовий параметр: `SPIKE_MIN_AVG=1.0` (а не 3.0!) — бо нормальна яскравість в тіні дрона ≈2. `jt-zero/camera/camera_pipeline.cpp:tick()`, `jt-zero/include/jt_zero/camera.h:bright_rolling_avg_,spike_suppress_frames_`.
57. **Систематичний IMU bias drift (~0.01м/5с без моторів) — dead-zone в Kalman predict** — IMU predict step `kf_vx_ += imu_ax_ * dt` (Fix #41) накопичує статичний IMU bias у позиційний drift. Hover decay (0.85/frame) частково компенсує, але steady-state ненульовий. Симптом: рівномірний -x/+y drift ±0.01м/5с на нерухомому дроні без моторів, conf=0.58 (трекінг нормальний). Fix: dead-zone 0.10 м/с² на `imu_ax_`/`imu_ay_` перед predict — при `|acc| < 0.10 м/с²` predict крок не виконується. Реальні маневри >> 0.2 м/с². Результат: x drift усунений повністю (ax < threshold). y drift (~0.01м/5с) може мати дві причини: (a) imu_ay між 0.05-0.10 м/с² → dead-zone 0.10 усуне; (b) VO bias від нахилу камери → dead-zone не допоможе. `camera_pipeline.cpp:Phase2 IMU predict`.
60. **VO velocity bias (камера нахилена ~5° pitch + floor texture) → LOITER Y drift 0.55 м/хв** — Hover decay (Fix #53a) множить `kf_vy_ × 0.85` але VO update step `kf_vy_ += K*(raw_vy - kf_vy_)` одразу повертає систематичний bias в Kalman state — decay безпорадний проти стабільного raw_vy ≠ 0. Fix #38 (gyro_z_bias) вирішив таку ж задачу для yaw. Fix #60 дзеркалить цю модель для лінійної швидкості: `vx_bias_`/`vy_bias_` — EMA від raw_vx/raw_vy під час стабільного hover (≥5с, `|raw_v| < 0.5 m/s`, α=0.005, ~30с settling). Bias віднімається з raw_v ПЕРЕД Kalman update: `kf_vx_ += Kx * (raw_vx - vx_bias_ - kf_vx_)`. Persistent через `reset()` (фізична калібровка); скидається лише через `clear_velocity_bias()`. RC ch12 disarmed → повний reset (pose + bias), armed → тільки pose. STATUSTEXT `"JT0: VO BIAS CALIB X=0.001 Y=0.009"` при стабілізації. `camera.h:VisualOdometry`, `camera_pipeline.cpp:Phase2+5`, `runtime.cpp:send_command("clear_bias")`, `native_bridge.py`.

---

## File Structure

```
jt-zero/
├── include/jt_zero/      # Public headers
│   ├── common.h           # SystemState, sensor data structs, MemoryPool
│   ├── sensors.h          # Sensor interfaces + auto-detect
│   ├── camera.h           # Camera sources + VO + Pipeline + reset_vo()
│   └── mavlink_interface.h # MAVLink with Serial/UDP/Sim + FCTelemetry + RC + STATUSTEXT
├── core/                  # Runtime core
│   └── runtime.cpp        # Thread management, main loop, vo_reset command
├── sensors/
│   └── sensors.cpp        # Sensor implementations + hw probing
├── camera/
│   ├── camera_pipeline.cpp # VO pipeline + SimulatedCamera + NEON integration
│   ├── camera_drivers.cpp  # PiCSI (V4L2/MMAP) + USB (V4L2)
│   └── neon_accel.h        # ARM NEON SIMD accelerated functions (frame brightness, Sobel, SAD)
├── drivers/
│   ├── bus.h/cpp          # I2C, SPI, UART HAL
│   └── sensor_drivers.h/cpp # MPU6050, BMP280, NMEA drivers
├── mavlink/
│   └── mavlink_interface.cpp # Serial/UDP/Sim + CRC parser + RC_CHANNELS(65) + send_statustext
├── api/
│   └── python_bindings.cpp # pybind11 module (+ send_statustext, rc_channels)
├── simulator/             # Test pattern generators
├── CMakeLists.txt
└── toolchain-pi-zero.cmake

backend/
├── server.py              # FastAPI + WebSocket + Flight Log API
├── native_bridge.py       # C++ bridge + VO Fallback monitor + RC reset + trail + STATUSTEXT
├── simulator.py           # Python fallback simulator
├── usb_camera.py          # V4L2 subprocess wrapper for USB thermal cam
├── flight_log.py          # AES-256 encrypted flight log (telemetry + point cloud)
├── system_metrics.py      # psutil-based real OS metrics
├── diagnostics.py         # Hardware diagnostics
├── venv/                  # Python venv (Pillow, cryptography, FastAPI)
└── static/                # Pre-built React frontend (served by FastAPI)

frontend/src/
├── App.js                 # Tab navigation + layout + voTrail fetch
├── components/
│   ├── CameraPanel.js     # Primary CSI camera stream + VO overlay
│   ├── ThermalPanel.js    # USB thermal camera stream + feature overlay
│   ├── CommandPanel.js    # ARM/DISARM/TAKEOFF/LAND/HOLD/RTL/E-STOP/SET HOMEPOINT
│   ├── MAVLinkPanel.js    # Compact MAVLink status (Dashboard)
│   ├── MAVLinkDiagPanel.js # Full diagnostics: RC Channels, FC Telemetry, Messages
│   ├── FlightLogPanel.js  # Encrypted flight log recording control
│   ├── Drone3DPanel.js    # 3D drone visualization + TrailLine + HomeMarker
│   ├── DocumentationTab.js # Docs: Quick Start, Install, VO Fallback, API, Hardware
│   └── ...                # 15+ panels total
└── hooks/useApi.js        # WebSocket + REST hooks
```

---

## FAQ: Running Without External IMU

**Q: Чи працюватиме система тільки з Pi Zero + польотний контролер, без зовнішнього IMU?**

**A: Так, повністю.** Ось як:

1. **Сценарій: Pi Zero + FC (ArduPilot/PX4)**
   - IMU вбудований у польотний контролер (він завжди має свій MPU6050/ICM20948)
   - JT-Zero отримує дані через MAVLink: `ATTITUDE`, `SCALED_IMU`, `GLOBAL_POSITION_INT`
   - Зовнішній MPU6050 на Pi НЕ потрібен

2. **Що робить JT-Zero без зовнішнього IMU:**
   - Камера + Visual Odometry — працює (не залежить від IMU)
   - MAVLink → FC — працює (передає VO дані польотнику)
   - Рефлекси та правила — працюють (використовують дані від FC)
   - IMU канал → автоматично переходить у SIM режим (генерує тестові дані)

3. **Мінімальна конфігурація:**
   - Pi Zero 2 W
   - Pi Camera Module v2 (або USB камера)
   - UART з'єднання з FC: TX→RX, RX→TX, GND
   - JT-Zero надсилає `VISION_POSITION_ESTIMATE` та `OPTICAL_FLOW_RAD` для fusion у EKF

4. **Оптимальна конфігурація:**
   - + MPU6050 на I2C (для власного AHRS і VO компенсації)
   - + BMP280 (незалежна альтиметрія)
   - + GPS UART (для absolute position backup)

---

## Build & Deploy

### Frontend (CI/CD — автоматично):
Frontend білдиться автоматично через **GitHub Actions** при пуші змін до `frontend/`. Результат комітиться в `backend/static/`. На Pi **Node.js/npm не потрібен**.

### Автоматичне встановлення (рекомендовано):
```bash
cd ~/jt-zero
chmod +x setup.sh
./setup.sh
```
Скрипт автоматично: встановить пакети, увімкне UART/I2C/SPI/Camera, збілдить C++, створить venv, налаштує systemd, перезавантажить Pi. ~10-15 хвилин.

### Оновлення:
```bash
cd ~/jt-zero
git pull && ./update.sh
```
`update.sh` логіка: pre-built frontend (з git) → локальний білд (якщо є npm) → повідомлення про помилку.

### На Pi (ручна збірка):
```bash
cd ~/jt-zero/jt-zero && mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release .. && make -j4
cp jtzero_native*.so ~/jt-zero/backend/
sudo systemctl restart jtzero
```

### Cross-compilation (from x86 host):
```bash
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-pi-zero.cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
scp jtzero_native*.so pi@jtzero.local:~/jt-zero/backend/
```

### Quick test after deploy:
```bash
sudo systemctl restart jtzero && sleep 15
curl -s http://localhost:8001/api/mavlink | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f\"State: {d['state']}, HB: {d.get('heartbeats_received',0)}\")
print(f\"Baud: {d.get('transport_info','?')}\")
print(f\"FC: {d['fc_type']} {d['fc_firmware']}\")
"
```

### Нова камера (не в списку підтримуваних):
Якщо `rpicam-hello --list-cameras` показує `No cameras available`:
1. Вимкніть автодетект: `camera_auto_detect=0` в `/boot/firmware/config.txt`
2. Додайте overlay: `dtoverlay=<sensor_name>,clock-frequency=37125000`
3. `sudo reboot`
4. Перевірте: `rpicam-hello --list-cameras`

JT-Zero автоматично визначить будь-яку камеру яку бачить libcamera.

---

## Session History
- Phase 1-11: Core runtime, sensors, camera, MAVLink, dashboard
- Bug fixes: VO displacement, MemoryPool, MAVLink semantics, roll atan2
- UI overhaul: 7-tab interface, detailed 3D drone, GPIO docs, Settings
- P1: Sensor auto-detect (I2C/UART probing)
- P2: Camera drivers (PiCSI V4L2, USB V4L2), MAVLink Serial/UDP transport
- Deployment: Successfully deployed on real Pi Zero 2W with native C++ runtime
- FC Connection guide: Matek H743-SLIM V3, SpeedyBee F405 V4, Pixhawk 2.4.8, Cube Orange+
- Platform/VO Mode refactor: Separated hardware config from algorithmic config
- USB thermal camera (Caddx 256): V4L2 MMAP driver, Shi-Tomasi detector, Sobel gradients, bilinear LK
- Verified on Pi 4 + Caddx thermal: Det:180, Track:16-59, Valid:True, Conf:0.18-0.29
- **MAVLink parser overhaul:** CRC validation, CRC-validated auto-baud, relaxed heartbeat filter, v2 signing support, diagnostic counters
- **MAVLink FC integration verified:** Pi Zero 2W + Matek H743 @ 115200 — CONNECTED, HB OK, VISION_POSITION_ESTIMATE @ 25Hz confirmed in MAVLink Inspector
- **EKF3 ExternalNav confirmed:** Both IMU0 and IMU1 using external nav data from JT-Zero
- **Automation scripts:** setup.sh (first install), update.sh (quick update with auto Pi model detection)
- **UI refresh:** Rounded corners (12px), ~1.5x larger fonts, 20-30% lighter colors, expanded MAVLink panel, Events tab scroll-lock
- **Multi-camera architecture:** CSI (PRIMARY/VO) + USB thermal (SECONDARY/on-demand), Variant B priority logic, 7 known CSI sensors + auto-detection
- **GENERIC CSI fallback:** Unknown sensors detected via rpicam-hello output parsing, raw name stored and displayed
- **IMX290 STARVIS added:** 8th known sensor (2MP, 1920x1080, FOV 82°, focal 400px)
- **GitHub Actions CI/CD:** `.github/workflows/build-frontend.yml` auto-builds React on push, commits to `backend/static/`
- **update.sh refactor:** Pre-built frontend from git (priority) → local npm build (fallback) → error message. Pi Zero no longer needs Node.js/npm
- **Verified on Pi Zero 2W:** IMX290 STARVIS detected, VO active (DET:180, TRACK:44, INL:44, Valid:True), MAVLink CONNECTED
- **Real USB camera capture:** `usb_camera.py` — v4l2-ctl subprocess (architecture-independent), auto-detect UVC devices, YUYV→grayscale
- **Verified on Pi 4:** AV TO USB2.0 (Caddx thermal) detected and streaming real frames via v4l2-ctl
- **V4L2 ioctl ABI fix:** Replaced raw fcntl/mmap V4L2 (broken on aarch64 due to struct size mismatch) with v4l2-ctl subprocess
- **USB thermal live streaming (FIXED):** Batch capture (v4l2-ctl --stream-count=2) — each call reopens device to force fresh analog conversion on MS210x. Frame cache bug fixed in native_bridge.py (frame_count not updating)
- **USB camera detection rewrite:** Parses `v4l2-ctl --list-devices` output instead of Python ioctl (which fails on aarch64)
- **Frontend ThermalPanel rewrite:** Matches CameraPanel MJPEG pattern (offscreen Image + canvas + sequential polling)
- **VO Fallback:** Automatic switch to USB thermal when CSI confidence drops below 10% for ~1s. Auto-recovery when CSI regains tracking. State machine: CSI_PRIMARY ↔ THERMAL_FALLBACK
- **Pillow venv fix:** `update.sh` now installs Pillow into venv (not system Python) — service runs via `venv/bin/uvicorn`
- **VO Fallback recovery fix:** Added brightness-based recovery (brightness >= 30). Previous confidence-only recovery (0.40) was too high for dim environments (~41 brightness). Now: brightness OR confidence (0.20)
- **SET HOMEPOINT:** `vo_reset` command in C++ (camera.h `reset_vo()`), Python, simulator. Button in Commands panel. RC channel edge-detected trigger (ch8 >= 1700 PWM)
- **3D Trail:** Backend accumulates VO dx/dy/dz into absolute positions (0.5s interval, max 500 points). `/api/vo/trail` API. Frontend: cyan TrailLine + amber HomeMarker in Drone3DPanel
- **ARM NEON SIMD:** `camera/neon_accel.h` — frame_brightness (16px/iter, ~8x), sobel_row (8px/iter, ~4x), structure_tensor_5x5, sad_8x8, row_sum. Integrated into camera_pipeline.cpp
- **MAVLink Diagnostics:** MAVLinkDiagPanel.js — RC Channels (18ch PWM bars), FC Telemetry, Message Types, Link Stats. C++ RC_CHANNELS (msg 65) parsing + Python bindings
- **MAVLink STATUSTEXT:** C++ `send_statustext(severity, text)` → Python `_send_statustext()`. Events: VO FALLBACK ACTIVE, CSI RECOVERED, SET HOMEPOINT, RC HOMEPOINT SET
- **Encrypted Flight Log:** `flight_log.py` — AES-256 Fernet, PBKDF2 100k iterations. Records telemetry + point cloud (camera pose + features for 3D landscape). Password-protected API. FlightLogPanel.js
- **System Constraints updated:** CPU ≤55% (alert 70%), RAM ≤180MB (alert 250MB) — justified by Pi Zero 2W thermal throttling analysis
- **DOCS update:** Added update.sh workflow as primary Install section, VO Fallback section, updated API Reference (+15 endpoints), Hardware, File Structure
- Test reports: /app/test_reports/iteration_1-16.json
- **MAVLink thread-safety fix (CRITICAL):** `handle_message()` had spinlock acquire but no release → deadlock → stale telemetry → YAW glitch in 3D View. Introduced RAII `ScopedSpinLock` struct — both `handle_message()` and `get_fc_telemetry()` now use it. Lock guaranteed released on all exit paths.

---

## Flight Controller Connection (Quick Reference)

### Підключення Pi → FC (3 дроти):
```
Pi Pin 8  (TX)  ──► FC RX (UART порт)
Pi Pin 10 (RX)  ◄── FC TX
Pi Pin 6  (GND) ─── FC GND
```

### ArduPilot параметри:
```
SERIALx_PROTOCOL = 2    (MAVLink2)
SERIALx_BAUD = 115      (115200 — auto-detected by JT-Zero)
VISO_TYPE = 1            (MAVLink vision)
EK3_SRC1_POSXY = 6      (ExternalNav)
EK3_SRC1_VELXY = 6      (ExternalNav)
EK3_SRC1_POSZ = 1       (Baro — if no rangefinder)
```

### UART порти по контролерах:
| FC | UART | Serial |
|----|------|--------|
| Matek H743-SLIM V3 | UART6 | SERIAL6 |
| SpeedyBee F405 V4 | UART4 | SERIAL4 |
| Pixhawk 2.4.8 | TELEM2 | SERIAL2 |
| Cube Orange+ | TELEM2 | SERIAL2 |

**Baud rate:** JT-Zero автоматично визначає baud rate FC (CRC-validated probe). Будь-який стандартний baud (57600-921600) підтримується. Ніякого ручного налаштування baud на стороні Pi не потрібно.

Детальна інструкція: /jt-zero/FC_CONNECTION.md

---

## Documentation Map (all in Ukrainian)

| File | Content |
|------|---------|
| `jt-zero/SYSTEM.md` | VO algorithm (FAST+Shi-Tomasi, Sobel LK, bilinear), Platform/VO Mode |
| `jt-zero/DEPLOYMENT.md` | Pi deployment (CSI + USB cameras, requirements-pi.txt) |
| `jt-zero/COMMANDS.md` | API curl commands, USB camera diagnostics, troubleshooting |
| `jt-zero/README.md` | Project overview, capabilities, architecture |
| `jt-zero/FC_CONNECTION.md` | Flight controller wiring (Matek, SpeedyBee, Pixhawk, Cube) |
| `jt-zero/LONG_RANGE_FLIGHT.md` | 5km autonomous flight guide |
| `setup.sh` | Auto-install script for fresh Pi OS (deps, UART, build, systemd) |
| `update.sh` | Smart update: C++ build, venv deps (Pillow+cryptography), frontend, service restart |
| `.github/workflows/build-frontend.yml` | GitHub Actions CI/CD for React frontend |
| `commands_reminder.md` | Quick reference: all git, Pi, API, diagnostic commands |
| `CLAUDE.md` | Technical reference for agents (this file) |
| `ABOUT_PROJECT.md` | Project overview in Ukrainian (for humans) |
| `memory/PRD.md` | Product requirements, backlog, roadmap |
| `memory/CHANGELOG.md` | Implementation changelog with dates |
| `backend/flight_log.py` | AES-256 encrypted flight log + point cloud recorder |
| `jt-zero/camera/neon_accel.h` | ARM NEON SIMD accelerated functions |
---

## Developer Context

**Developer:** Ihor (Ігор), Ukraine. Military service. Works in extremely limited time (sacrifices sleep).

**Communication rules:**
- Always respond in Ukrainian
- Direct, no flattery, no sugar-coating
- Don't invent — if unsure, say so
- Ihor is a visual learner — use diagrams, tables, structured output

**Personal context files:** `D:\Obsidian\CloudCode\_claude\`
- `CLAUDE.md` — personal preferences and communication style  
- `about_me.md` — who Ihor is
- `projects.md` — full JT-Zero context from personal perspective
- `agents/` — mentor, engineer, strategist agents
- `skills/` — drone systems, Python, C++, business
- `memory/` — key decisions and insights

**To load personal context at session start:**
Read `D:\Obsidian\CloudCode\_claude\CLAUDE.md` before starting work.