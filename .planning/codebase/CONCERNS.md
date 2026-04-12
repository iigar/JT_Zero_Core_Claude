# Codebase Concerns

**Analysis Date:** 2026-04-12

---

## CRITICAL — Flight Safety

### Data Race on SystemState (8 threads, no mutex)

- Issue: `SystemState state_` in `jt-zero/include/jt_zero/runtime.h:123` is a plain struct with no synchronization. All 8 threads (T0–T7) read and write it concurrently. T1 writes IMU/baro/attitude fields at 200 Hz; T7 reads the entire struct to serialize JSON for the API bridge at 30 Hz; T3 reads `flight_mode` and `armed` at 200 Hz for reflex decisions; T5 writes `mavlink`-derived fields.
- Files: `jt-zero/include/jt_zero/runtime.h` (struct definition, line 123), `jt-zero/core/runtime.cpp` (all thread loops)
- Impact: **FLIGHT SAFETY RISK.** A torn read of `flight_mode` or `armed` during a concurrent write can produce a garbage enum value, causing the reflex or rules engine to act on a corrupt mode. On ARM Cortex-A53 (Pi Zero 2W), 32-bit aligned writes are atomic but multi-field compound reads are not. `float motor[4]` writes from the output engine are not atomic across the four elements.
- Fix approach: Introduce a `std::shared_mutex state_mutex_` in `Runtime`. Writers (T0, T1, T5) take a unique lock scoped to the specific sub-struct being updated. Readers (T3, T7) take a shared lock. Alternatively, separate the struct into per-thread-owned segments and use only atomic copies for cross-thread reads (e.g., a snapshot pattern: `std::atomic<SystemState> state_snapshot_` updated via `store` with `memory_order_release`).
- Priority: **HIGH** — open task in `Worklog.md`

---

## CRITICAL — Memory Corruption Risk

### MemoryPool::allocate() ABA Race

- Issue: `MemoryPool<T, N>::allocate()` in `jt-zero/include/jt_zero/common.h:120–131` uses a CAS-based lock-free free-list. The implementation has an ABA vulnerability: between `load(free_head_)` and `compare_exchange_weak`, another thread can pop the head block, use it, and push it back — the CAS succeeds even though `head.next` now points to the newly-pushed-back block's old `next`, not the current one. This can cause two threads to receive a pointer to the same block.
- Files: `jt-zero/include/jt_zero/common.h:120–131`
- Impact: Two concurrent allocators receive the same `T*`. On a robotics system, this can corrupt event payloads or sensor data structures in-flight.
- Fix approach: Use a tagged pointer (ABA counter) packed into a 64-bit atomic alongside the index (common on 64-bit ARM). Alternatively, use a generation counter: `struct TaggedHead { int32_t idx; int32_t gen; }` stored in `std::atomic<TaggedHead>` with a 64-bit CAS (`std::atomic<int64_t>` cast). The `Worklog.md` notes this race as open (entry: "MemoryPool race — Replaced mutex-based pool with lock-free CAS free-list (O(1))" in bug fix #2, but the ABA issue remains unresolved).
- Priority: **HIGH** — open task in `Worklog.md`

---

## HIGH — Hardware / Deployment

### Camera Burnout History and CSI Sensitivity

- Issue: Historical CSI camera burnout events have occurred on the Pi Zero hardware. CLAUDE.md documents 8 supported CSI sensors but makes no mention of voltage protection or hot-plug safety. The camera pipeline probes `rpicam-hello --list-cameras` at runtime to auto-detect sensors; repeated probing on a marginal sensor could accelerate degradation.
- Files: `jt-zero/camera/camera_pipeline.cpp` (`initialize_multicam()`), `CLAUDE.md` (Camera Pipeline section)
- Impact: Loss of primary VO source mid-flight. USB thermal fallback activates, but USB thermal FOV is narrower (25–50°) and VO confidence is lower — not a safe long-term primary source.
- Fix approach: Add probe cooldown and a maximum probe-retry count in `initialize_multicam()`. Document minimum wiring requirements (ribbon cable seating, 3.3V rail stability).

### UART Must Use ttyAMA0 Not ttyS0

- Issue: `CLAUDE.md` (MAVLink section) confirms verified hardware uses `/dev/ttyAMA0@115200`. The sensor auto-detect table in `CLAUDE.md` lists GPS on `/dev/ttyS0`. On Pi Zero 2W, `ttyS0` is the mini-UART (limited baud, lossy at high rates); `ttyAMA0` is the full PL011 UART (required for reliable MAVLink). Misconfiguration causes `ttyS0` to be used for MAVLink by default in some Pi OS images.
- Files: `jt-zero/include/jt_zero/sensors.h` (NMEA GPS sensor, `ttyS0` listed in auto-detect), `jt-zero/mavlink/mavlink_interface.cpp` (transport cascade), `CLAUDE.md` (Sensor Hardware table)
- Impact: MAVLink frames corrupted or missing at 115200 baud on `ttyS0`. CRC errors cause heartbeat loss. Drone loses VO position updates.
- Fix approach: Document in `setup.sh` that `enable_uart=1` + `dtoverlay=disable-bt` must be in `/boot/firmware/config.txt` to remap PL011 to `ttyAMA0`. Add a startup check that warns if MAVLink transport resolves to `ttyS0`.

### Pi Zero 2W Thermal Limits and CPU Budget

- Issue: System constraints are CPU ≤55% (alert at 70%) and RAM ≤180MB (alert at 250MB) per `CLAUDE.md`. Pi Zero 2W is 4× Cortex-A53 @ 1GHz with no active cooling. USB bus is shared between thermal camera and WiFi — thermal capture adds ~3ms CPU per frame. At 15fps camera + 200Hz sensors + MAVLink, thermal throttling can push CPU to 70%+.
- Files: `backend/system_metrics.py`, `CLAUDE.md` (System Constraints section)
- Impact: Thermal throttling causes CPU frequency reduction → all real-time threads run slower → T1 (200Hz sensors) and T3 (200Hz reflexes) miss deadlines → latency spikes visible in `max_latency_us` thread stats.
- Fix approach: Add active CPU temperature monitoring to T0 (supervisor) with a pre-throttle alert at 70°C (before the SoC throttles at 80°C). Consider disabling USB thermal streaming during high-load flight phases (leave it for ground inspection only).

### EKF3 ExternalNav — Indoor Low Confidence (9%)

- Issue: `CLAUDE.md` Session History notes: "VO Fallback: Automatic switch to USB thermal when CSI confidence drops below 10% for ~1s." Indoor testing shows VO confidence hovering at 9% — just below the 10% recovery threshold used internally. The CONF_RECOVER threshold was later adjusted to 0.20, but this was specifically for the fallback recovery path; indoor confidence on CSI in a featureless environment can stay permanently below 25%.
- Files: `jt-zero/camera/camera_pipeline.cpp` (VO pipeline, fallback state machine), `backend/native_bridge.py` (`vo_fallback_tick()`)
- Impact: When EKF3 receives `VISION_POSITION_ESTIMATE` with covariance derived from 9% confidence, it assigns low weight to VO. Indoor navigation without GPS relies entirely on VO — low confidence causes EKF3 to drift rapidly.
- Fix approach: For indoor use, set `EK3_SRC1_POSXY = 6` but also reduce `EK3_VISI_VERR_MAX` to tolerate higher VO uncertainty. In software, track the "vision cycling" condition (repeated FALLBACK↔CSI_PRIMARY transitions within a short window) and surface it as a dashboard warning.

---

## HIGH — Security

### Breaking Change: PBKDF2 Salt Rotation on Deploy

- Issue: The 2026-04-02 session replaced the hardcoded `SALT = b"jtzero-flight-log-v1"` with a per-installation random 16-byte salt stored in `config.json`. This is a one-way migration: all existing `.jtzlog` files encrypted with the old salt are permanently unreadable after deploy.
- Files: `backend/flight_log.py` (`_get_or_create_salt()`), `backend/config.json` (generated, contains salt)
- Impact: If an operator upgrades without reading the Worklog, all historical flight logs are silently lost. The API returns `{"error": "Wrong password"}` for old logs — indistinguishable from an incorrect password attempt.
- Fix approach: Before deploying the new `flight_log.py`, export/archive all existing sessions. Add a migration notice in `update.sh` output. The `POST /api/logs/password` endpoint must be called after deploy to set a new password for the new salt.

### Remaining Attack Surface: config.json Contains Salt in Plaintext

- Issue: `config.json` stores the PBKDF2 salt in plaintext on the Pi filesystem. If an attacker has filesystem read access (e.g., via SD card extraction), they obtain the salt and can brute-force the password offline with `PBKDF2(password, salt, 100000)`. The minimum password length of 12 characters is the only defense.
- Files: `backend/config.json`, `backend/flight_log.py`
- Impact: Encrypted flight logs may be decryptable offline if the salt is obtained and the password is weak.
- Fix approach: No practical fix for SD card extraction threat model on a Pi without TPM. Ensure minimum password length enforcement (currently 12, enforced in `backend/server.py:set_log_password`). Document this threat in `DEPLOYMENT.md`.

---

## MEDIUM — Technical Debt

### Build Artifacts and Credentials Tracked in Git

- Issue: The following files are committed to `origin` (`iigar/JT_Zero_Core`) and are present in the working tree:
  - `.gitconfig` — developer git configuration (tracked, confirmed via `git ls-files`)
  - `backend/jtzero_native.cpython-311-aarch64-linux-gnu.so` — compiled binary for aarch64
  - `jt-zero/build/` — full CMake build tree including `CMakeCache.txt`, compiler test binaries (`a.out`), and `CMakeFiles/`
- Files: `.gitconfig`, `backend/jtzero_native.cpython-311-aarch64-linux-gnu.so`, `jt-zero/build/` (all tracked via `git ls-files`)
- Impact: Repo size bloat. Committing `.so` binaries means the `origin` branch contains platform-specific artifacts that break cross-compilation workflows. `jt-zero/build/CMakeCache.txt` contains absolute paths from the build host, causing `cmake ..` to fail on a fresh clone. `.gitconfig` may contain personal identity or credential helpers.
- Fix approach: Add to `.gitignore`: `*.so`, `jt-zero/build/`, `.gitconfig`. Run `git rm --cached` for each tracked file. **Note:** Changes must go to `claude` remote only — never push to `origin`. Coordinate with Emergent AI team (origin owner) before removing files from `origin/main22`.
- Priority: **MED** — open task in `Worklog.md`

### GPS Auto-Detect Lists ttyS0 (Mini-UART) as Default

- Issue: `CLAUDE.md` sensor auto-detect table lists GPS NMEA on `/dev/ttyS0`. On Pi Zero 2W running Raspberry Pi OS, the default UART assignment maps `ttyS0` to the Bluetooth UART (mini-UART) and `ttyAMA0` to the Bluetooth chip. Without the `disable-bt` overlay, GPS on `ttyS0` competes with Bluetooth and produces garbage NMEA data.
- Files: `jt-zero/include/jt_zero/sensors.h`, `jt-zero/drivers/bus.h`
- Impact: GPS reads fail silently in default Pi OS config. System falls back to GPS simulation mode without a clear error.
- Fix approach: Change GPS auto-detect order to probe `ttyAMA0` first, then `ttyS0`. Log a warning if `ttyS0` is selected (likely indicates missing `disable-bt` overlay).

---

## MEDIUM — Algorithm / Navigation

### VO Drift Accumulation (~±5m in 10-Minute Flight)

- Issue: `CLAUDE.md` documents known VO drift of ~±5m over a 10-minute flight. The Kalman filter accumulates `pose_var_x_`/`pose_var_y_` covariance (Bug Fix #40), so EKF3 is correctly informed of growing uncertainty. However, there is no VO loop-closure or absolute position reset mechanism beyond the `SET HOMEPOINT` command (which resets the VO origin to zero, not to a GPS-derived absolute position).
- Files: `jt-zero/camera/camera_pipeline.cpp` (pose accumulation), `jt-zero/mavlink/mavlink_interface.cpp` (VISION_POSITION_ESTIMATE send)
- Impact: In GPS-denied environments (indoor, jamming), position error grows unbounded. After 10 minutes, the drone's EKF3 position estimate can be 5m off from true position. RTL in GPS-denied mode will not return to the true launch point.
- Fix approach: Implement a landmark-based position correction trigger: when the drone revisits a previously seen scene patch (detectable via feature descriptor comparison), correct the accumulated pose. This is a significant algorithmic addition (visual place recognition). Short-term mitigation: document the drift rate and set a maximum mission duration for GPS-denied flight.

### IMU Pre-Integration Mutex in Hot Path (T1 at 200Hz)

- Issue: Bug Fix #41 added `std::mutex preint_mtx_` in `VisualOdometry` to protect `PreIntState` between T1 (200Hz IMU) and T6 (15Hz camera). A `std::mutex` under high contention has ~100–500ns latency on ARM. At 200Hz, T1 calls `camera_.accumulate_gyro()` every 5ms. T6 calls `process()` every 66ms and locks the same mutex to read+reset the pre-integration state.
- Files: `jt-zero/camera/camera_pipeline.cpp` (`accumulate_gyro()`, `process()`), `jt-zero/include/jt_zero/camera.h` (`PreIntState`, `preint_mtx_`)
- Impact: T1 mutex acquisition adds latency variance to the 200Hz sensor loop. On a heavily loaded Pi Zero, this can cause T1 to miss its 5ms deadline, introducing IMU jitter that degrades the complementary filter (Bug Fix #2 in session 2026-04-03).
- Fix approach: Replace `std::mutex` with a lock-free SPSC approach: T1 accumulates into a per-T1 local, then does a single atomic swap at the end of each 5ms cycle into a `std::atomic<PreIntState>` (using a double-buffer or a simple 64-bit packed representation for `dgx/dgy/dgz` if precision allows).

---

## LOW — Operational

### Post-Deploy Password Reset Required

- Issue: After deploying the new `flight_log.py` (PBKDF2 random salt), the operator must call `POST /api/logs/password` before any logging is possible. If this step is skipped, `start_log` will fail with a key derivation error because no password has been set for the new salt.
- Files: `backend/server.py` (`start_log`, `set_log_password`), `backend/flight_log.py`
- Impact: Flight logs silently fail to record on first post-deploy mission.
- Fix approach: In `update.sh`, print a visible post-deploy checklist: "ACTION REQUIRED: Reset flight log password via `POST /api/logs/password` before next mission."

### Fix 6 (LK IMU Hints) Untested on Hardware

- Issue: `Worklog.md` (Відкриті задачі) lists "Тест Fix 6 на Pi: виміряти tracked features при швидкому yaw" as a LOW priority open task. Fix 6 added IMU pre-integration hints to Lucas-Kanade tracker for rapid yaw compensation. The fix was implemented in session 2026-04-03 but has not been validated on physical hardware.
- Files: `jt-zero/camera/camera_pipeline.cpp` (LK hint application), `jt-zero/core/runtime.cpp` (`camera_loop`, `accumulate_gyro` call)
- Impact: Unknown regression risk. If the hint offsets are computed incorrectly (wrong sign, wrong focal scale), rapid yaw could cause LK to search in the wrong direction, reducing tracked feature count below the minimum for valid VO output.
- Fix approach: During next hardware session, execute a controlled yaw-only maneuver at ±45° and compare `features_tracked` with and without IMU hints enabled. Log `hint_dx`/`hint_dy` values alongside `vo_features_tracked` in the flight log.

### USB Thermal Camera ~5fps Constraint

- Issue: The MS210x AV-to-USB capture card requires batch capture (`v4l2-ctl --stream-count=2`) with device reopen per grab due to frame repeat bug (Bug Fix #16). This limits USB thermal to ~5fps — below the minimum for reliable LK optical flow tracking (LK is designed for frame-to-frame displacements of <15px; at 5fps, feature displacement between frames can exceed this at any meaningful flight speed).
- Files: `backend/usb_camera.py`, `backend/native_bridge.py` (injection loop)
- Impact: USB thermal VO fallback is unreliable above slow hover speeds. Feature tracking degrades as speed increases, causing VO fallback to produce invalid estimates that get sent to EKF3 at 25Hz.
- Fix approach: In `vo_fallback_tick()`, gate VISION_POSITION_ESTIMATE transmission based on `features_tracked` minimum threshold (e.g., ≥5 inliers) and cap `position_uncertainty` to a high value (e.g., 5.0m) during USB thermal fallback. This tells EKF3 to down-weight the estimate rather than treating it as high-quality.

---

## Test Coverage Gaps

### C++ Runtime Has No Unit Tests

- What's not tested: `MemoryPool`, `RingBuffer`, `sensor_loop`, `camera_loop`, thread lifecycle in `runtime.cpp`, Kalman filter phases in `camera_pipeline.cpp`, MAVLink CRC validation in `mavlink_interface.cpp`
- Files: `jt-zero/core/runtime.cpp`, `jt-zero/camera/camera_pipeline.cpp`, `jt-zero/mavlink/mavlink_interface.cpp`, `jt-zero/include/jt_zero/common.h`
- Risk: The ABA race in `MemoryPool` and the `SystemState` data race listed above cannot be caught by code review alone — they require either TSan (ThreadSanitizer) or a deterministic test harness that replays concurrent access patterns. Neither exists.
- Priority: **High** for `MemoryPool` and `SystemState` (safety-critical). Medium for Kalman phases (algorithm correctness).
- Fix approach: Add a CMake test target. Use Google Test or Catch2. Run TSan build (`-fsanitize=thread`) on x86 CI before Pi deployment. A basic `MemoryPool` stress test with 8 threads allocating/deallocating concurrently would catch the ABA race deterministically under TSan.

### Python Backend Has No Automated Tests

- What's not tested: `flight_log.py` encryption/decryption round-trip, `server.py` API endpoint authorization, `native_bridge.py` fallback state machine transitions, `usb_camera.py` device detection logic
- Files: `backend/flight_log.py`, `backend/server.py`, `backend/native_bridge.py`, `backend/usb_camera.py`
- Risk: The 42 documented bug fixes in `CLAUDE.md` were all found manually during hardware testing. Any refactor to these files risks silently reintroducing fixed bugs.
- Priority: **Medium**
- Fix approach: Add `pytest` to `backend/requirements-pi.txt`. Create `backend/tests/test_flight_log.py` covering: salt generation, encrypt/decrypt round-trip, wrong password returns `None`, path traversal rejection.

---

*Concerns audit: 2026-04-12*
