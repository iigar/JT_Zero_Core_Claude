# Testing Patterns

**Analysis Date:** 2026-04-12

---

## Test Framework

**Runner:** pytest (invoked via CI and manually)

**Assertion Library:** pytest built-in assertions + `requests` for HTTP

**HTTP Client:** `requests` (synchronous) — no `httpx` async client used

**Run Commands:**
```bash
# From repo root — run all backend tests
cd backend && ../backend/venv/bin/pytest tests/ -v

# Run specific test file
backend/venv/bin/pytest backend/tests/test_multicamera.py -v

# Run with XML report output (matches CI pattern)
backend/venv/bin/pytest backend/tests/ -v --junit-xml=test_reports/pytest/pytest_results.xml

# Run legacy script-style test (not pytest)
python3 backend_test.py
```

---

## Test File Organization

**Location:** Two locations exist:

1. `backend/tests/` — pytest test suite (primary, structured)
   - `test_jtzero_api.py` — core REST endpoint tests
   - `test_multicamera.py` — multi-camera API tests (iteration 16)
   - `test_vo_features.py` — VO profiles and adaptive parameter tests

2. `backend_test.py` — root-level legacy script (class-based, not pytest, uses Emergent Preview URL hardcoded)

**Naming:** `test_{area}.py` — lowercase with underscores.

**Structure within files:**
```python
class TestAreaName:
    """Tests for /api/endpoint — description"""

    def test_specific_behavior(self):
        """One-line docstring describing what is verified"""
        response = requests.get(f"{BASE_URL}/api/endpoint")
        assert response.status_code == 200
        data = response.json()
        assert "field" in data
```

---

## Test Suite Details

### `backend/tests/test_jtzero_api.py`

Core API contract tests. Covers:
- `TestHealthEndpoint` — `/api/health`: status ok, runtime mode (native/simulator), build_info fields
- `TestStateEndpoint` — `/api/state`: flight_mode enum values, roll/pitch/yaw types, altitude, battery, IMU fields, GPS fields, barometer fields
- `TestTelemetryEndpoint` — `/api/telemetry`: state object, threads array structure, engines object
- `TestEventsEndpoint` — `/api/events`: array shape, event field structure (timestamp, type, priority, message)

Base URL from environment:
```python
BASE_URL = os.environ.get('REACT_APP_BACKEND_URL', '').rstrip('/')
```

### `backend/tests/test_multicamera.py`

Multi-camera system tests (iteration 16). Covers:
- `TestCamerasEndpoint` — `/api/cameras`: 2 slots, PRIMARY/SECONDARY metadata, `has_vo` flags, USB_THERMAL type, 256x192 thermal resolution
- `TestSecondaryCameraStats` — `/api/camera/secondary/stats`: field structure, camera_type
- `TestSecondaryCameraCapture` — `POST /api/camera/secondary/capture`: increments frame_count, sets active=True
- `TestSecondaryCameraFrame` — `/api/camera/secondary/frame`: PNG content-type, PNG magic bytes (`\x89PNG\r\n\x1a\n`), `X-Frame-Id` header
- `TestPrimaryCameraBackwardCompatibility` — `/api/camera` and `/api/camera/frame` still return 200
- `TestMultiCameraIntegration` — full thermal capture workflow end-to-end; both cameras queryable simultaneously

### `backend/tests/test_vo_features.py`

VO pipeline parameter tests. Covers:
- `TestVOProfiles` — `/api/vo/profiles`: exactly 3 profiles, exact field values for Pi Zero 2W / Pi 4 / Pi 5
- `TestCameraAdaptiveFields` — adaptive FAST threshold (15-35 range), LK window (3-15 range), altitude zone enum
- `TestCameraHoverFields` — hover_detected bool, hover_duration >= 0, yaw_drift_rate, corrected_yaw
- `TestCameraAllNewFields` — integration check: all VO fields present simultaneously
- `TestWebSocketTelemetryNewFields` — verifies camera data accessible via REST (WebSocket sends same payload)

### `backend_test.py` (Legacy)

Script-style tester, not pytest. Uses class `JTZeroTester` with `run_test()` method:
```python
def run_test(self, name, method, endpoint, expected_status, data=None, validate_response=None):
    # runs HTTP call, checks status, validates JSON, returns (bool, dict)
```
Covers: health (native mode only), state, performance, events, telemetry, threads, engines, camera, MAVLink, commands (ARM/TAKEOFF/LAND/RTL/HOLD/DISARM/EMERGENCY).

Hardcoded to Emergent Preview URL — **not suitable for Pi hardware testing without modification**.

---

## CI/CD — GitHub Actions

**Workflow file:** `.github/workflows/build-frontend.yml`

**Trigger:** Push to any branch when files under `frontend/src/**`, `frontend/public/**`, `frontend/package.json`, `frontend/tailwind.config.js`, or `frontend/postcss.config.js` change.

**What it does:**
1. Checks out repo
2. Sets up Node.js 20 with npm cache
3. `npm install --no-audit --no-fund`
4. `REACT_APP_BACKEND_URL="" npm run build` — builds with empty backend URL (Pi-compatible)
5. Copies `frontend/build/` → `backend/static/`
6. Commits and pushes with message `ci: rebuild frontend [skip ci]`

**What CI does NOT test:** The GitHub Actions CI only builds the frontend — it does **not** run the pytest suite or the C++ build. There is no backend test pipeline in CI.

**Result:** Pi Zero never needs Node.js installed — it pulls the pre-built static files from `backend/static/`.

---

## Test Reports

**Location:** `test_reports/`

**Format:** Two formats maintained in parallel:
- `test_reports/iteration_{N}.json` — JSON summaries with passed_tests list, frontend/backend issues, action items
- `test_reports/pytest/pytest_results_iter{N}.xml` — JUnit XML from `--junit-xml` flag

**Iterations:** Reports exist for iterations 1–16. Iteration 16 is latest (all 24 multi-camera tests pass, 100%).

---

## Hardware vs Simulator Testing

### Simulator (Emergent / Development)

All automated tests run against the Python simulator by default. The simulator auto-activates when the compiled C++ `.so` is not present:
```python
# backend/server.py
try:
    from native_bridge import NativeRuntime, NATIVE_AVAILABLE
    if NATIVE_AVAILABLE:
        runtime = NativeRuntime()
        RUNTIME_MODE = "native"
    else:
        raise ImportError(...)
except:
    from simulator import JTZeroSimulator
    runtime = JTZeroSimulator()
    RUNTIME_MODE = "simulator"
```

Simulator provides identical API surface — all pytest tests pass against simulator.

### Physical Pi Zero 2W Hardware

Hardware testing is **manual only**. Process:
1. Deploy via `update.sh` or `setup.sh`
2. Service runs via `systemctl start jtzero` (uses `backend/venv/bin/uvicorn`)
3. Connect to `http://jtzero.local:8001` in browser
4. Collect debug logs: `journalctl -u jtzero -f`

**Key hardware-only behaviors that cannot be simulated:**
- CSI camera auto-detection (`rpicam-hello --list-cameras`)
- USB thermal camera (v4l2-ctl subprocess, MS210x batch capture)
- I2C hardware probing (MPU6050 at 0x68, BMP280 at 0x76)
- MAVLink serial connection to Matek H743 FC (auto-baud CRC validation)
- ARM NEON SIMD acceleration (auto-fallback to scalar on x86)

---

## Manual Testing Procedures

### VO Pipeline

1. Verify camera detected: `GET /api/cameras` — check PRIMARY slot `camera_type` is `CSI_IMX219` (or detected sensor), `camera_open: true`
2. Check VO running: `GET /api/camera` — `vo_features_detected > 0`, `vo_features_tracked > 0`, `vo_tracking_quality > 0.1`
3. Verify VISION_POSITION_ESTIMATE delivery: MAVLink Inspector in Mission Planner shows msg #102 at ~25Hz
4. Test VO Fallback: block CSI camera lens → wait ~3s → `GET /api/camera` shows `vo_source: "USB_THERMAL"`, `GET /api/camera/secondary/stats` shows `active: true`
5. Verify fallback recovery: unblock CSI → wait for `MIN_FALLBACK_S` (3s) + `PROBES_TO_RECOVER` (1 probe) → `vo_source` returns to `CSI_*`
6. Dashboard: Camera/VO tab shows feature overlay — orange tracked squares, yellow detected circles, displacement vector

### MAVLink

1. Check connection: `GET /api/mavlink` → `state: "CONNECTED"`, `heartbeats_received > 0`
2. Verify FC telemetry: `fc_telemetry.attitude_valid: true` (ATTITUDE msgs from FC)
3. Verify message rates: `msg_per_second` should be ~40-50
4. Check `crc_errors: 0` — non-zero means baud rate mismatch or noisy serial
5. RC channels: `rc_channels` array of 18 PWM values when RC receiver connected to FC
6. Dashboard: MAVLink Diagnostics tab → RC Channels bars, FC Telemetry panel, Link Stats

### Camera (USB Thermal)

1. Trigger capture: `POST /api/camera/secondary/capture` → `{"success": true}`
2. Verify frame: `GET /api/camera/secondary/frame` → PNG with valid `\x89PNG` header, `X-Frame-Id` header present
3. Check active flag: `GET /api/camera/secondary/stats` → `active: true`, `frame_count > 0`
4. Dashboard: Camera/VO tab → THERMAL or SPLIT view → false-color iron palette image

### EKF3 Integration Verification

In Mission Planner → MAVLink Inspector:
- Message #102 `VISION_POSITION_ESTIMATE` arriving at ~25Hz
- Message #331 `ODOMETRY` arriving at ~25Hz
- ArduPilot logs: EKF3 `ExtNav` lane shows position updates (not NaN)
- EKF3 should NOT be cycling start/stop aiding (Bug Fix #1 resolved this — quality=0 + rate limiting)

---

## Known Test Gaps

**No C++ unit tests** — The C++ core (`jt-zero/`) has no unit test harness. No `gtest`, no `catch2`, no test CMake target. All C++ correctness is verified only through the Python API layer.

**No CI for backend tests** — GitHub Actions only builds frontend. Pytest suite is not run automatically on push. Tests must be run manually in Emergent or on hardware.

**No frontend tests** — No Jest test files in `frontend/src/`. The `package.json` includes `react-scripts test` (Create React App default) but no test files exist. Frontend correctness verified by visual inspection only.

**No WebSocket integration test** — `TestWebSocketTelemetryNewFields` in `test_vo_features.py` explicitly notes: "Full WebSocket test would require async client" and falls back to verifying REST endpoint data shapes instead.

**No load / stress tests** — No tests for behavior under high CPU load on Pi Zero. Thread timing guarantees (T0@10Hz, T1@200Hz, etc.) are not verified automatically.

**No failure-path tests** — No tests for: disk full (flight log), wrong password (flight log), MAVLink CRC error injection, USB camera disconnect mid-flight, CSI camera disconnect.

**No cross-iteration regression suite** — Each iteration's test file is separate. There is no single `pytest` run that covers all iterations together. Old test files may become stale as API evolves.

**Hardware-only bugs** — Several critical bugs (MAVLink auto-baud, USB thermal MS210x freeze, Pillow venv install) were only discoverable on real Pi hardware. Simulator cannot reproduce them.

---

*Testing analysis: 2026-04-12*
