# JT-Zero Changelog

## 2026-04-19 — Fix #51-53: GPS warn opt-in, confidence стабілізація, Kalman hover decay

### Fix #51 — GPS warn opt-in (JTZERO_GPS_WARN=1)
- При GPS_TYPE=0 gps_warn_tick() спамила STATUSTEXT. Тепер вимкнено за замовч.
- `backend/native_bridge.py`

### Fix #52 — VO confidence стабілізація
- EMA alpha: 0.3 → 0.1 (часова константа 200мс → 670мс, менше осциляцій)
- imu_consistency penalty: 5.0 → 2.0 (м'якший штраф за IMU/VO розбіжність)
- `jt-zero/camera/camera_pipeline.cpp`

### Fix #53 — Kalman hover velocity decay
- kf_vx_/vy_ *= 0.85 кожен кадр при hover_.detected && hover_.duration>1s
- Запобігає накопиченню IMU bias у Kalman за тривалої нерухомості (conf падав до 0.09)
- `jt-zero/camera/camera_pipeline.cpp`

### Debug cleanup
- Видалено [VO DBG] fprintf з camera_pipeline.cpp
- `jt-zero/camera/camera_pipeline.cpp`

## 2026-04-19 — Fix #50 opt-in + debug cleanup (замінено вище)

### Fix #50b — GPS warn opt-in via JTZERO_GPS_WARN=1
- **Проблема:** GPS_TYPE=0 setups (VO-only) отримували спам STATUSTEXT без GPS — `fix_type` завжди 0, uncertainty зростає → постійні WARNING/CRITICAL без сенсу
- **Рішення:** `gps_warn_tick()` читає `os.environ.get('JTZERO_GPS_WARN', '0')` при ініціалізації → `_GPS_WARN_ENABLED` flag. За замовчуванням вимкнено. Щоб увімкнути: `JTZERO_GPS_WARN=1` у `/etc/jtzero.env` або `systemd` service file.
- **Файли:** `backend/native_bridge.py` (init + gps_warn_tick early return)

### Debug cleanup — видалено fprintf з camera_pipeline.cpp
- Видалено `[VO DBG]` fprintf (кожні 10с) доданий для діагностики fix #52. Діагностика завершена: поріг знижено до 0.32, conf=0.33 проходить.
- **Файл:** `jt-zero/camera/camera_pipeline.cpp`

## 2026-04-16 — GPS-Loss Position Uncertainty Warning (Fix #50)

### Fix #50 — GPS DEGRADED / GPS LOST STATUSTEXT
- **Що:** Коли GPS fix_type < 3 і дрон armed, Kalman-derived `position_uncertainty` (Fix #40) зростає з накопиченням дрейфу VO. Ніяких попереджень не надходило.
- **Рішення:** `gps_warn_tick()` в `native_bridge.py` — викликається 1Hz з `_vo_fallback_monitor`.
  - `uncertainty > 4m` → `MAV_SEVERITY_WARNING` "JT0: GPS DEGRADED unc=Xm VO only"
  - `uncertainty > 8m` → `MAV_SEVERITY_CRITICAL` "JT0: GPS LOST unc=Xm RTL ADVISED"
  - Debounce: 30с між повторними повідомленнями. Escalation без debounce.
  - Hover-aware: uncertainty зростає тільки з реальним дрейфом, не в зависанні.
  - GPS recovery: логує `[GPS Warn] GPS recovered` при поверненні фіксу.
- **Файли:** `backend/native_bridge.py` (gps_warn_tick + state init), `backend/server.py` (виклик 1Hz), `backend/simulator.py` (no-op stub)

## 2026-04-15 — Thread Safety, Git Hygiene, LK IMU Hints Hardware Fix

### Phase 01 — SystemState Thread Safety (Fix #43)
- **Що:** 8 потоків писали в `SystemState state_` без синхронізації → data races під TSan
- **Виправлено 5 sites:** range/flow поза sensor_mutex_; update_flight_physics без dual lock; send_command/rule_loop викликали update_safety_snapshot() з неправильним lock; reflex lambdas без lock; main.cpp читав raw state reference
- **Канонічний порядок mutex:** sensor_mutex_(1) → slow_mutex_(2) → motor_mutex_(3) → emit_mutex_(4)
- **Додано:** `Runtime::state_snapshot()` — повертає копію під dual lock; `SafetySnapshot` 8-byte atomic struct
- **Верифіковано:** TSan Run #5 — **0 DATA RACE**, 30с, 8 потоків
- **CPU:** 6%, RAM: 33MB на Pi Zero 2W ✓

### Git Hygiene
- **Що:** jt-zero/build/, backend/*.so (cpython-311), .gitconfig випадково потрапили в git
- **Дія:** `git rm --cached -r` → 77 файлів видалено з tracking
- **Виправлено .gitignore:** дублювання `*.env` × ~50 (через баг `echo -e` в скрипті), додано `*.so`, `.gitconfig`, `frontend/package-lock.json`

### Fix #45 — LK IMU Hints мертві з реальним FC (hardware verified)
- **Що:** `accumulate_gyro()` викликалась тільки в `if (!fc_active)` блоці sensor_loop. З підключеним Matek H743 → fc_active=true → preint_.valid завжди false → LK hints ніколи не активувались → TRK: 0-4 при будь-якому yaw
- **Fix 45a:** додано `else` гілку в sensor_loop — читає gyro з state_.imu (заповнює T5 з MAVLink SCALED_IMU) під sensor_mutex_, передає в accumulate_gyro()
- **Fix 45b:** ATTITUDE msg (id=30) rollspeed/pitchspeed/yawspeed (rad/s) скопійовані в fc_telem_.gyro_x/y/z → imu_valid=true; ATTITUDE stream: 4Hz → 25Hz (via REQUEST_DATA_STREAM + SET_MESSAGE_INTERVAL)
- **Верифіковано на залізі:** Pi Zero 2W + Matek H743, Pi Camera v2. Slow/medium yaw (<90°/s): **TRK:176 Q:0.98** (до фіксу: TRK:0-4). Падіння до 0 при дуже різкому flick — очікувана фізика (features виходять за 320px за 66мс)
- **Файли:** `runtime.cpp:sensor_loop`, `mavlink_interface.cpp:case30`, потоки stream rates

### Оновлено backlog (PRD.md)
- **Закрито з P1:** "Deploy + verify complementary filter on FC" (✓ виконано Fix 45), "Test LK hint effectiveness" (✓ TRK:176 підтверджено)
- **Залишилось P1:** flight log test, STATUSTEXT test в Mission Planner
- **Додано P2:** GPS-loss position_uncertainty warning (RuleEngine, Kalman-based, HOVER-aware)

## 2026-04-03 — VO+IMU Fusion Overhaul (6 fixes)

### Fix 36: imu_consistency — правильне ΔV порівняння (`camera_pipeline.cpp`)
- **Було:** `actual_dvx = raw_vx - kf_vx_` — post-update Kalman residual, не ΔV
- **Порівнювалося:** Kalman residual (залишок вимірювання) з IMU delta-v — математично некоректно
- **Рішення:** snapshot `kf_vx_prev_` ДО predict-кроку → `actual_dvx = raw_vx - kf_vx_prev_`
- Масштабний коефіцієнт: `0.5 → 5.0` (при 1 m/s² розбіжності за 66ms → 0.066 m/s)

### Fix 37: Complementary filter для attitude (`runtime.cpp sensor_loop`)
- **Було:** `roll = atan2(acc_y, acc_z)` — тільки акселерометр (шумний на HF). `yaw += gyro_z * dt` — голий інтеграл без bias-корекції, drift до 3°/хв
- **Рішення:** CF alpha=0.98: `cf_roll = 0.98*(cf_roll + gx*dt) + 0.02*atan2(ay, az)`
- Yaw: `(gz - gyro_z_bias) * dt`. On-ground bias: EMA при `!armed && |gyro| < 0.05 rad/s`, BIAS_ALPHA=0.0005

### Fix 38: Gyro bias estimation в hover (`camera_pipeline.cpp`)
- При `is_hovering` і `|gyro_z| < 0.3 rad/s` → `hover_.gyro_z_bias += (gyro_z - bias) * 0.005`
- drift_rate тепер: `rate_from_optical_flow - hover_.gyro_z_bias`
- Нова константа `HoverState::BIAS_ALPHA = 0.005f` в `camera.h`

### Fix 39: IMU prediction step у Kalman + wire set_imu_hint (`camera_pipeline.cpp`, `runtime.cpp`)
- **Проблема 1:** Predict = тільки `P += Q`. State відставав при прискореннях дрона
- **Рішення:** `kf_vx_ += imu_ax_ * dt` перед update (спрощений EKF), якщо `imu_hint_valid_`
- **Проблема 2 (прихована):** `set_imu_hint()` існував в API але НІКОЛИ не викликався з `runtime.cpp` → `imu_hint_valid_` завжди false → Fixes 36, 37, 39 ніколи не активувалися
- **Рішення:** `camera_.set_imu_hint(acc_x, acc_y, gyro_z)` додано в `camera_loop()` ДО `tick()`

### Fix 40: position_uncertainty з реальної KF covariance (`camera_pipeline.cpp`)
- **Було:** `uncertainty = total_distance * 0.03 * (1 - confidence*0.5)` — ad hoc без зв'язку з фільтром
- **Рішення:** `pose_var_x_ += kf_vx_var_ * dt²` кожен кадр → `uncertainty = sqrt(pose_var_x + pose_var_y)`
- Decay ×0.995 при confidence > 0.7; growth ×4 при dead-reckoning (no position_update)
- ArduPilot EKF отримує правдиву 1-sigma невизначеність → кращий баланс з GPS/baro

### Fix 41: IMU pre-integration для LK initial-flow hints (`camera.h`, `camera_pipeline.cpp`, `runtime.cpp`)
- **Проблема:** між кадрами 15Hz (~66ms = 13 IMU samples) LK не знає про rotation. При ±10° повороті features шукаються у старих позиціях → track failure
- **Архітектура:**
  - `PreIntState {dgx, dgy, dgz}` + `std::mutex preint_mtx_` додано в `VisualOdometry`
  - T1 (200Hz): `camera_.accumulate_gyro(gx, gy, gz-bias, dt)` — thread-safe накопичення
  - T6: `shift_x = focal * dgz`, `shift_y = -focal * dgy` → `hint_dx[]`, `hint_dy[]` в LKTracker
  - LKTracker::track(): нові параметри `const float* hint_dx = nullptr, hint_dy = nullptr`
  - `flow_x` стартує з `hint_dx[f]` замість 0; ворота: |shift| > 0.3px
- **Нові публічні методи на `CameraPipeline`:** `set_imu_hint()`, `accumulate_gyro()`

## 2026-03-30 — MAVLink Thread Safety Fix + 3D YAW Wrap Fix (CRITICAL)

### Bug Fix #24: handle_message() deadlock
- `handle_message()` acquired `telem_lock_` spinlock but NEVER released it → deadlock → stale telemetry
- Introduced RAII `ScopedSpinLock` struct — both `handle_message()` and `get_fc_telemetry()` now use it
- Files: `jt-zero/include/jt_zero/mavlink_interface.h`, `jt-zero/mavlink/mavlink_interface.cpp`

### Bug Fix #25: 3D View YAW full-spin on 360→0 wrap (ROOT CAUSE)
- `Drone3DPanel.js` used naive lerp without angle normalization. Yaw crossing 360°→0° caused full 360° model spin
- Fix: normalize yaw delta to [-PI, PI] before lerping — model always takes shortest path
- Also fixed: `Feature` → `FeaturePoint` in `camera.h` and `camera_pipeline.cpp` (compilation error)
- Frontend rebuilt to `backend/static/` with `REACT_APP_BACKEND_URL=""`


## 2026-03-29 — Encrypted Flight Log + STATUSTEXT + NEON + MAVLink Diag

### Encrypted Flight Log
- `flight_log.py`: AES-256 (Fernet) encrypted log files, PBKDF2 key derivation (100k iterations)
- Password stored as SHA-256 hash in `config.json`
- Records: telemetry (VO pos, conf, CPU, RAM) + point cloud (camera pose + features) + events
- API: `/api/logs/status`, `/api/logs/password`, `/api/logs/start`, `/api/logs/stop`, `/api/logs/sessions`, `/api/logs/read`
- FlightLogPanel.js on MAVLink tab — password-protected recording control
- ~3-5 KB/s write rate, ~10-18 MB/hour of flight

### MAVLink STATUSTEXT
- C++ `send_statustext(severity, text)` added to `mavlink_interface.cpp`
- Exposed via Python bindings
- Sends on events: VO FALLBACK ACTIVE, VO CSI RECOVERED, SET HOMEPOINT, RC HOMEPOINT SET
- Visible in Mission Planner HUD at any radio range

### ARM NEON SIMD Optimization
- `camera/neon_accel.h` — frame_brightness, sobel_row, structure_tensor_5x5, sad_8x8, row_sum
- Integrated into camera_pipeline.cpp

### MAVLink Diagnostics Panel
- RC Channels (18ch PWM bars), FC Telemetry, Message Types, Link Stats
- C++ RC_CHANNELS (msg 65) parsing

### System Constraints Updated
- CPU: <= 55% (was 65%), alert 70%
- RAM: <= 180MB (was 300MB), alert 250MB

### RC-based SET HOMEPOINT
- C++ RC_CHANNELS (msg 65) parsing added to `mavlink_interface.cpp` — 18 channels + rssi
- Python RC monitor with edge detection: RC ch8 PWM >= 1700 → `vo_reset`
- RC channels exposed in `get_mavlink()` API

### 3D Trail Visualization
- Backend: accumulates VO dx/dy/dz into absolute positions, sampled every 0.5s (max 500 points)
- API: `GET /api/vo/trail` returns `[{x, y, z, t}, ...]`, cleared on `vo_reset`
- Frontend: cyan TrailLine + amber HomeMarker in Drone3DPanel 3D View

## 2026-03-28 — SET HOMEPOINT + DOCS + Recovery Fix

### SET HOMEPOINT (VO Reset)
- **C++ `camera.h`**: Added `reset_vo()` method on `CameraPipeline` (calls `vo_.reset()`)
- **C++ `runtime.cpp`**: Added `"vo_reset"` command handling in `send_command()` — resets position to (0,0,0), clears distance, Kalman state, hover state
- **Python `simulator.py`**: Added `"vo_reset"` command handling
- **CommandPanel.js**: Added "SET HOMEPOINT" button alongside ARM/DISARM/TAKEOFF/etc.
- **API**: `POST /api/command {"command":"vo_reset"}` → resets VO origin

### VO Fallback Recovery Fix (brightness-based)
- **Problem**: Recovery used confidence threshold (0.40), but in dim environments (brightness ~41) confidence hovers at the threshold → CSI never recovers
- **Fix**: Added brightness-based recovery path: if `frame_brightness >= 30` (BRIGHT_RECOVER), switch back to CSI immediately
- Lowered confidence recovery threshold from 0.40 to 0.20 as secondary path
- Recovery now logs the exact reason: `brightness=41>=30` or `probe_conf=0.25>=0.20`

### DOCS Tab Updated
- Added **VO Fallback** section: architecture diagram, trigger logic, parameters, USB thermal setup, venv dependency note
- Updated **API Reference**: added 10 missing endpoints (camera/features, cameras, secondary, vo/profiles, diagnostics, sensors)
- Updated **File Structure**: added usb_camera.py, venv/, static/, update.sh
- Updated **Hardware Requirements**: added USB Thermal, FC, RC Transmitter as components
- Updated **Install Step 8**: added Pillow to venv setup
- Updated command list: added `vo_reset`


### Root Cause: Pillow installed in SYSTEM Python, service runs in VENV
- `update.sh` ran `apt install python3-pil` → installed into `/usr/lib/python3.13/`
- Service uses `/home/pi/jt-zero/backend/venv/bin/uvicorn` → venv can't see system packages
- Result: `PIL=False FILTERS=False NUMPY=False` — ALL feature detectors disabled
- **Fix**: `update.sh` now detects venv and installs Pillow via `venv/bin/pip install Pillow`

### Additional Fixes
- Error logging in `_decode_jpeg_to_gray`, numpy fallback, raw detector (replaced silent `except: pass`)
- Diagnostic startup log: `PIL=T/F FILTERS=T/F NUMPY=T/F`
- `Image.Resampling.NEAREST` compat fallback
- `/api/camera/features` hardened with try/except → always returns `[]`



## 2026-03-27 — VO Fallback Stabilization + Feature Overlay Fix

### Brightness-Only Trigger (CRITICAL FIX)
- **Removed confidence-based trigger**: FAST detector tracks sensor noise in pitch darkness, causing confidence ~70% when camera is blocked
- **New trigger**: Rolling average brightness < 20 (10-sample window, min 5 samples)
- **Recovery**: Uses CSI brightness probes (3 consecutive good probes needed), with 3s minimum fallback time and 5s cooldown

### ThermalPanel Feature Overlay Fix
- **ROOT CAUSE FOUND**: C++ `get_features()` returns empty on ARM64 Pi during fallback — `vo.feature_count()` reads stale `active_count_` due to no memory barrier between T6 thread (writes) and Python thread (reads)
- **C++ snapshot fix (pending recompile)**: Added `features_snapshot_[]` + `std::atomic<uint32_t>` to CameraPipeline with release/acquire barriers. Will activate when user does clean rebuild
- **Python-side feature detection**: When C++ returns empty during fallback, Python runs its own corner detector (simplified Shi-Tomasi via numpy) on the same thermal frame being injected into C++. Features are at REAL corners/edges of the thermal image, not pseudo-random positions
- **Python detector**: Sobel gradients → `min(|Ix|, |Iy|)` corner response → 3x3 NMS → top 120 by response. ~5-10ms on Pi 4 for 320x240
- **Dual-trigger canvas rendering**: Features redraw on JPEG frame load AND on camera stats update AND on features change
- **PTS counter**: Shows real `features.length` (Python-detected corner count)

### CLAUDE.md Updated
- Documented brightness-only trigger with reasoning
- Updated configuration table with new parameters
- Added Feature Overlay section


## 2026-03-27 — VO Fallback to USB Thermal Camera (P1)

### C++ Core: Hybrid VO Fallback (Python → C++ injection)
- **VOSource enum** (CSI_PRIMARY, THERMAL_FALLBACK): identifies current VO camera source
- **VOFallbackConfig**: configurable thresholds — CONF_DROP_THRESH=0.10, CONF_RECOVER_THRESH=0.25, FRAMES_TO_SWITCH=15 (~1.5s), CSI_PROBE_INTERVAL=3s
- **VOFallbackState**: runtime state tracking — source, reason, low_conf_count, fallback_duration, total_switches
- **inject_frame()**: Thread-safe SPSC (atomic state machine 0→1→2→0) for Python→C++ frame injection
- **activate_fallback() / deactivate_fallback()**: External control from Python, resets VO with thermal/CSI focal lengths
- **tick() modified**: In fallback mode, processes injected thermal frames instead of CSI capture. Periodic CSI probe (every 3s) using FAST detector for recovery check
- **Why hybrid?**: C++ USBCamera uses YUYV which returns all-zero frames on MS210x capture card. Working USB capture is Python usb_camera.py with MJPEG via v4l2-ctl subprocess

### Python: VO Fallback Monitor (`native_bridge.py`)
- **vo_fallback_tick()**: Called at 10Hz from WebSocket telemetry loop
- Monitors vo_confidence → counts consecutive low readings → triggers activate_fallback()
- **Injection thread**: Captures JPEG from usb_camera.py → decodes to grayscale via Pillow → injects at ~5fps via inject_frame()
- **Recovery monitor**: Reads CSI probe results from C++ fallback state → calls deactivate_fallback() when CSI recovers

### pybind11 Bindings (`python_bindings.cpp`)
- **inject_frame(data, w, h)**: Injects grayscale frame for VO
- **activate_fallback(reason)** / **deactivate_fallback()**: External fallback control
- **is_confidence_low()**: Check if CSI below threshold for N frames
- **get_fallback_state()**: Get detailed fallback state dict
- **camera_stats_to_dict**: Now includes vo_source, vo_fallback_reason, vo_fallback_duration, vo_fallback_switches

### Backend & Frontend
- **server.py**: Calls vo_fallback_tick() in WebSocket telemetry loop (10Hz)
- **simulator.py**: vo_fallback_tick() no-op stub + fallback fields in CameraStats
- **Frontend**: VO Source badge, fallback alert banner, sidebar indicator (unchanged from initial implementation)
- **update.sh**: Added `pip3 install pillow` + VO Source status in health check

## 2026-03-26 — USB Thermal Camera Live Streaming (P0 Fix)

### Root Cause: Frame Cache Never Invalidated
- **Bug**: `get_secondary_frame_data()` in `native_bridge.py` returned frame data but did NOT update `frame_count` in the camera state dict. Server's cache in `server.py` used `frame_count` to decide whether to serve fresh data. Since `frame_count` never changed, the server returned the first cached frame forever.
- **Fix**: Added `self._secondary_camera['frame_count'] = self._usb_capture.frame_count` in `get_secondary_frame_data()`.
- **Symptoms**: Backend captured different frames (unique MD5 hashes), FPS counter showed 12fps, but displayed image was static.

### USB Camera Detection Rewrite
- **Old**: Python `ioctl(VIDIOC_QUERYCAP)` — failed silently on aarch64 Pi
- **New**: Parses `v4l2-ctl --list-devices` subprocess output — reliable on all architectures
- Device shows as `AV TO USB2.0 (usb-...)` at `/dev/video1`

### Capture Architecture: Batch (not Persistent)
- **Persistent process** (`--stream-count=0`): MS210x capture card repeats same frame when device stays open (analog converter "sticks"). Achieved 12fps but all frames identical.
- **Batch capture** (`--stream-count=2`): Each call reopens device, forcing fresh analog-to-digital conversion. Slower (~5fps) but frames are genuinely different.
- Added MD5 hash logging to confirm frame uniqueness: every frame logged as `NEW`

### Frontend ThermalPanel Rewrite
- Rewritten to match CameraPanel's proven pattern: offscreen `new Image()` + canvas + `drawFrame()` in `onload`
- Fixed canvas dimensions: 640x480 (matching MJPEG resolution)
- Sequential polling (70ms delay between fetches)

### Logging Fix
- `_log()` changed from `print()` to `sys.stderr.write()` — now visible in `journalctl`
- `[MultiCam]` messages in `native_bridge.py` also use stderr

### Configuration
- `BATCH_SIZE=2` (1 warm-up + 1 real frame), `TEST_BATCH=4` for initial test
- Resolution: 640x480 MJPEG (known working on MS210x)
- No gap between batch captures (device reopen is sufficient reset)

### Verified on Pi 4
- CSI Camera v2 (VO): ACTIVE, ~14fps
- USB Thermal (AV TO USB2.0): ACTIVE, live video streaming, ~5fps
- Both cameras running simultaneously

## 2026-03-24 — IMX290 STARVIS + GENERIC CSI Fallback

### New Sensor Support
- **IMX290 STARVIS** added to known CSI sensors (8th sensor): 2MP 1920x1080, FOV 82°, focal 400px, excellent low-light (Sony STARVIS back-illuminated)
- **GENERIC CSI fallback**: Unknown sensors detected via `rpicam-hello` output parsing (`"N : sensor_name [WxH ...]"` format). Raw sensor chip name stored and displayed in dashboard
- **CSISensorType::GENERIC = 99**: New enum value for unknown-but-working cameras
- **PiCSICamera::detected_raw_name()**: Static method returns raw chip ID string
- **CameraPipelineStats**: Added `csi_sensor_type` and `csi_sensor_name` fields
- **Python bindings**: `camera_stats_to_dict()` now includes CSI sensor info
- **native_bridge.py**: `get_cameras()` reads sensor name from C++ stats (not hardcoded)
- **Verified on Pi Zero 2W**: IMX290 auto-detected as "IMX290 STARVIS (VO)", VO active

### New Camera Setup (not in known list)
If `rpicam-hello` shows "No cameras available":
1. Set `camera_auto_detect=0` in `/boot/firmware/config.txt`
2. Add `dtoverlay=<sensor>,clock-frequency=37125000`
3. Reboot → `rpicam-hello --list-cameras` should show the camera
4. JT-Zero auto-detects as GENERIC or known sensor

## 2026-03-24 — GitHub Actions CI/CD + update.sh Refactor

### Frontend Build Automation
- **GitHub Actions workflow** (`.github/workflows/build-frontend.yml`): Auto-builds frontend on push when `frontend/src/`, `frontend/public/`, or `package.json` changes. Commits `backend/static/` back to repo with `[skip ci]`.
- **Pre-built frontend in git**: `backend/static/` (8.7MB) committed to repo — Pi Zero no longer needs Node.js/npm
- **update.sh refactored**: Checks for pre-built `backend/static/index.html` first (instant), falls back to local npm build only if missing
- **Root cause fix**: Pi Zero 2W (416MB RAM) cannot run `npm install` — OOM kills the process silently. Pre-built approach eliminates this entirely.
- **.gitignore cleaned**: Removed ~60 duplicate entries, kept `!backend/static/` exception

## 2026-03-23 — Multi-Camera Architecture (P1)

### Multi-Camera Support
- **C++ Header (camera.h)**: Added `CameraSlot` enum (PRIMARY/SECONDARY), `CameraSlotInfo` struct, multi-camera methods to `CameraPipeline` (init_secondary, capture_secondary, get_slot_info, camera_count)
- **C++ Pipeline (camera_pipeline.cpp)**: Implemented secondary camera lifecycle (init, capture on-demand, shutdown), slot info reporting
- **Backend API (server.py)**: 4 new endpoints — `GET /api/cameras`, `GET /api/camera/secondary/stats`, `POST /api/camera/secondary/capture`, `GET /api/camera/secondary/frame`
- **Backend Bridge (native_bridge.py)**: Multi-camera fallback methods for C++ runtime (lazy-init, simulated thermal frames)
- **Backend Simulator (simulator.py)**: Full multi-camera simulation with thermal hotspot generation
- **Frontend ThermalPanel.js**: New component — on-demand capture, auto-refresh (1fps), iron palette false-color rendering, temperature legend
- **Frontend App.js**: CameraTab with SPLIT/VO ONLY/THERMAL view switcher, Dashboard sidebar CAMERAS section
- **WebSocket**: `cameras` array added to telemetry payload
- **Testing**: 24/24 tests pass (100%)

### Variant B — CSI Priority, USB Fallback
- **C++ camera.h**: Added `CSISensorType` enum (OV5647, IMX219, IMX477, IMX708, OV9281, IMX296, OV64A40), `CSISensorInfo` struct with sensor specs
- **C++ camera_drivers.cpp**: `detect_sensor()` — parses rpicam-hello output to identify CSI sensor model; `find_usb_device()` — scans /dev/video0..9 for USB cameras (skips CSI V4L2 devices); `initialize_multicam()` — Variant B logic
- **C++ PiCSICamera**: Stores detected sensor type and info, dynamic camera name from sensor model
- **Backend labels**: Dynamic PRIMARY label (CSI sensor name or "USB fallback"), "USB Thermal (Down)" for secondary
- **Frontend**: CSI sensor badge in Camera tab header, "USB Fallback" warning badge when no CSI

## 2026-03-23 — EKF3 Integration, Automation, UI Refresh

### EKF3 ExternalNav
- ArduPilot EKF3 confirmed using JT-Zero VO data: "EKF3 IMU0/1 is using external nav data"
- VISION_POSITION_ESTIMATE @ 25Hz + ODOMETRY @ 25Hz flowing to FC
- Verified on both Pi Zero 2W and Pi 4B + Matek H743

### Automation Scripts
- `setup.sh` — full first-install automation (deps, UART/I2C/SPI, build, systemd, reboot)
- `update.sh` — quick update with auto Pi model detection (make -j2 for Zero, -j4 for Pi 4/5)

### UI Refresh
- Rounded corners, font scaling, colors lightened
- MAVLink panel expanded with full diagnostics
- Events tab: scroll-lock

## 2026-03-22 — MAVLink Parser Overhaul (P0 Fix)

### Bug Fixes
- Auto-baud detection, CRC validation, heartbeat filter, v2 zero truncation, v2 signing support
- Verified: Pi Zero 2W + Matek H743 @ 115200 baud, 0 CRC errors

## 2026-02/03 — Visual Odometry & Camera Overhaul
- USB Camera V4L2 MMAP rewrite
- LK Tracker bilinear interpolation, Sobel 3x3 gradients
- Shi-Tomasi grid corner detector for thermal
- Verified on Pi 4 + Caddx thermal: Det:180, Track:16-59, Conf:0.18-0.29

