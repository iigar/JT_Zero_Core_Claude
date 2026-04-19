"""
JT-Zero Native Bridge
Wraps the compiled C++ runtime (jtzero_native) with the same interface
as the Python simulator, enabling seamless switching.
"""

import time
import math
import sys
import os
import io
import threading
import subprocess

# Try to import native module
try:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import jtzero_native as _native
    NATIVE_AVAILABLE = True
    BUILD_INFO = dict(_native.get_build_info())
except ImportError:
    NATIVE_AVAILABLE = False
    BUILD_INFO = None

# For JPEG → grayscale decoding in VO Fallback
try:
    from PIL import Image
    PIL_AVAILABLE = True
except ImportError:
    PIL_AVAILABLE = False

# For Python-side feature detection during VO Fallback visualization
# Issue 9 fix: numpy removed — too heavy for Pi Zero (CLAUDE.md rule 5).
# Fallback chain: Pillow (primary) → pure Python (secondary).
NUMPY_AVAILABLE = False

# Pillow ImageFilter/ImageChops for Pillow-only feature detection
try:
    from PIL import ImageFilter, ImageChops
    PILLOW_FILTERS = True
except ImportError:
    PILLOW_FILTERS = False


class NativeRuntime:
    """Adapter wrapping C++ Runtime with simulator-compatible interface."""
    
    _VO_MODES = [
        {"id": 0, "name": "Light", "type": "LIGHT",
         "fast_threshold": 30, "lk_window": 5, "lk_iterations": 4, "max_features": 100},
        {"id": 1, "name": "Balanced", "type": "BALANCED",
         "fast_threshold": 25, "lk_window": 7, "lk_iterations": 5, "max_features": 180},
        {"id": 2, "name": "Performance", "type": "PERFORMANCE",
         "fast_threshold": 20, "lk_window": 9, "lk_iterations": 6, "max_features": 250},
    ]
    
    _PLATFORMS = [
        {"id": 0, "name": "Pi Zero 2W", "type": "PI_ZERO_2W",
         "width": 640, "height": 480, "focal_length": 554.0, "target_fps": 15.0},
        {"id": 1, "name": "Pi 4", "type": "PI_4",
         "width": 1280, "height": 720, "focal_length": 830.0, "target_fps": 30.0},
        {"id": 2, "name": "Pi 5", "type": "PI_5",
         "width": 1280, "height": 960, "focal_length": 1108.0, "target_fps": 30.0},
    ]
    
    def __init__(self):
        if not NATIVE_AVAILABLE:
            raise RuntimeError("jtzero_native module not found")
        
        self._rt = _native.Runtime()
        self._active_vo_mode = 1  # Balanced
        
        # Auto-detect: use real hardware on Pi, simulator elsewhere
        # Override with JT_ZERO_SIMULATE=1 to force simulation
        force_sim = os.environ.get("JT_ZERO_SIMULATE", "").lower() in ("1", "true", "yes")
        if force_sim:
            self._rt.set_simulator_mode(True)
            print("[JT-Zero] Forced SIMULATOR mode (JT_ZERO_SIMULATE=1)")
        else:
            # Check if we're on a Raspberry Pi
            is_pi = os.path.exists("/sys/firmware/devicetree/base/model")
            if is_pi:
                self._rt.set_simulator_mode(False)
                print("[JT-Zero] Running on Pi — HARDWARE mode (auto-detect sensors)")
            else:
                self._rt.set_simulator_mode(True)
                print("[JT-Zero] Not on Pi — SIMULATOR mode")
        
        self._start_time = time.time()
        self.running = False
        
        # ── VO Fallback monitoring state ──
        self._vo_fallback_active = False
        self._vo_bright_history = []  # rolling window of brightness values (ONLY indicator)
        self._vo_fallback_thread = None
        self._vo_fallback_stop = threading.Event()
        self._vo_fallback_start_time = 0
        self._vo_fallback_cooldown_until = 0
        self._vo_csi_good_probes = 0
        self._VO_BRIGHT_DROP = 20       # trigger when avg brightness below this (camera dark)
        self._VO_BRIGHT_RECOVER = 30    # recovery: brightness above this = CSI OK
        self._VO_CONF_RECOVER = 0.20    # recovery: confidence > this (lowered for dim environments)
        self._VO_WINDOW_SIZE = 10       # 1 second at 10Hz
        self._VO_MIN_SAMPLES = 8        # need 0.8s of data
        self._VO_MIN_FALLBACK_S = 3     # 3s minimum in fallback
        self._VO_COOLDOWN_S = 5         # 5s cooldown after recovery
        self._VO_PROBES_TO_RECOVER = 1
        self._VO_INJECT_W = 320
        self._VO_INJECT_H = 240
        
        # ── Python-side feature detection for visualization ──
        self._vo_python_features = []  # cached Python-detected features
        
        # ── RC-based VO Reset ──
        self._rc_reset_channel = 7      # RC channel index (0-based, ch8 on transmitter)
        self._rc_reset_threshold = 1700  # PWM threshold to trigger reset
        self._rc_reset_armed = False     # edge detection: only trigger once per switch flip
        
        # ── VO Position Trail (3D visualization) ──
        self._vo_trail = []              # list of {x, y, z, t} positions
        self._vo_trail_max = 500         # max trail points
        self._vo_trail_interval = 0.5    # seconds between trail samples
        self._vo_trail_last_t = 0
        self._vo_pos_x = 0.0            # accumulated VO position
        self._vo_pos_y = 0.0
        self._vo_pos_z = 0.0

        # ── GPS-loss position uncertainty warning ──
        # Fires when GPS is degraded and Kalman-derived uncertainty exceeds thresholds.
        # Uses pose_var_x_/y_ (Fix #40) — physically correct, not ad-hoc time-based.
        # Does not spam: minimum _GPS_WARN_DEBOUNCE_S between messages.
        # Opt-in: set env var JTZERO_GPS_WARN=1 to enable (off by default — GPS_TYPE=0 setups get no spam).
        import os as _os
        self._GPS_WARN_ENABLED      = _os.environ.get('JTZERO_GPS_WARN', '0') == '1'
        self._GPS_WARN_THRESHOLD_M  = 4.0   # WARNING level (m)
        self._GPS_WARN_CRITICAL_M   = 8.0   # CRITICAL level (m)
        self._GPS_WARN_DEBOUNCE_S   = 30.0  # min seconds between STATUSTEXT messages
        self._GPS_WARN_MIN_FIX      = 3     # fix_type < this → GPS degraded
        self._gps_warn_last_sent    = 0.0   # timestamp of last warning
        self._gps_warn_level        = 'ok'  # 'ok' | 'warn' | 'critical'
    
    def start(self):
        if self.running:
            return
        ok = self._rt.initialize()
        if not ok:
            raise RuntimeError("C++ runtime initialization failed")
        self._rt.start()
        self._start_time = time.time()
        self.running = True
    
    def stop(self):
        if not self.running:
            return
        self._rt.stop()
        self.running = False
    
    def send_command(self, cmd: str, param1: float = 0, param2: float = 0) -> bool:
        if cmd == "vo_reset":
            self._vo_trail = []
            self._vo_pos_x = 0.0
            self._vo_pos_y = 0.0
            self._vo_pos_z = 0.0
            self._send_statustext(6, "JT0: SET HOMEPOINT")
        return self._rt.send_command(cmd, param1, param2)
    
    def _send_statustext(self, severity: int, text: str):
        """Send STATUSTEXT via MAVLink (visible in Mission Planner)."""
        try:
            if hasattr(self._rt, 'send_statustext'):
                self._rt.send_statustext(severity, text)
        except Exception:
            pass
    
    def get_state(self) -> dict:
        state = dict(self._rt.get_state())
        # Fix roll/pitch: C++ uses atan2(acc_y, acc_z) where acc_z=-9.81 → roll≈180°
        # Correct: atan2(acc_y, -acc_z) → roll≈0° when level
        imu = state.get("imu", {})
        if isinstance(imu, dict):
            ay = imu.get("acc_y", 0)
            az = imu.get("acc_z", -9.81)
            ax = imu.get("acc_x", 0)
            state["roll"] = math.atan2(ay, -az) * 57.2958
            state["pitch"] = math.atan2(-ax, math.sqrt(ay**2 + az**2)) * 57.2958
        return state
    
    def get_events(self, count: int = 50) -> list:
        events = self._rt.get_events(count)
        return [dict(e) for e in events]
    
    def get_telemetry_history(self, count: int = 100) -> list:
        history = self._rt.get_telemetry_history(count)
        return [dict(h) for h in history]
    
    def get_thread_stats(self) -> list:
        return [dict(t) for t in self._rt.get_threads()]
    
    def get_engine_stats(self) -> dict:
        return dict(self._rt.get_engines())
    
    def get_camera_stats(self) -> dict:
        d = dict(self._rt.get_camera())
        d.setdefault("vo_inlier_count", d.get("vo_features_tracked", 0))
        d.setdefault("vo_confidence", d.get("vo_tracking_quality", 0))
        d.setdefault("vo_position_uncertainty", 0)
        d.setdefault("vo_total_distance", 0)
        # Platform info (auto-detected by C++)
        d.setdefault("platform", 0)
        d.setdefault("platform_name", "Pi Zero 2W")
        # VO Mode — inject from managed state
        m = self._VO_MODES[self._active_vo_mode]
        d["vo_mode"] = self._active_vo_mode
        d["vo_mode_name"] = m["name"]
        # Adaptive parameters defaults
        d.setdefault("altitude_zone", 0)
        d.setdefault("altitude_zone_name", "LOW")
        d.setdefault("adaptive_fast_thresh", float(m["fast_threshold"]))
        d.setdefault("adaptive_lk_window", float(m["lk_window"]))
        # Hover yaw correction defaults
        d.setdefault("hover_detected", False)
        d.setdefault("hover_duration", 0.0)
        d.setdefault("yaw_drift_rate", 0.0)
        d.setdefault("corrected_yaw", 0.0)
        # VO Fallback state
        if hasattr(self._rt, 'get_fallback_state'):
            try:
                fs = dict(self._rt.get_fallback_state())
                d["vo_source"] = fs.get("source", "CSI_PRIMARY")
                d["vo_fallback_reason"] = fs.get("reason", "")
                d["vo_fallback_duration"] = fs.get("fallback_duration", 0.0)
                d["vo_fallback_switches"] = fs.get("total_switches", 0)
            except Exception:
                d.setdefault("vo_source", "CSI_PRIMARY")
                d.setdefault("vo_fallback_reason", "")
                d.setdefault("vo_fallback_duration", 0.0)
                d.setdefault("vo_fallback_switches", 0)
        else:
            d.setdefault("vo_source", "CSI_PRIMARY")
            d.setdefault("vo_fallback_reason", "")
            d.setdefault("vo_fallback_duration", 0.0)
            d.setdefault("vo_fallback_switches", 0)
        d.setdefault("frame_brightness", 128.0)
        return d
    
    def get_frame_data(self) -> bytes:
        """Get latest camera frame as raw grayscale bytes (320x240)."""
        try:
            return self._rt.get_frame_data()
        except Exception:
            return b''
    
    def get_features(self) -> list:
        """Get current VO feature positions [{x, y, tracked, response}, ...]."""
        try:
            feats = [dict(f) for f in self._rt.get_features()]
            if feats:
                return feats
        except Exception:
            pass
        # During fallback: use Python-detected features from the thermal frame
        if self._vo_fallback_active and self._vo_python_features:
            return self._vo_python_features
        # Simulated features when C++ has no real camera (Emergent preview)
        if self._rt.is_simulator_mode():
            t = time.time()
            n = 80 + int(20 * math.sin(t * 0.1))
            result = []
            for i in range(n):
                fx = (hash((i * 7 + 13)) % 300) + 10 + 3 * math.sin(t * 0.2 + i)
                fy = (hash((i * 11 + 7)) % 220) + 10 + 3 * math.cos(t * 0.15 + i)
                result.append({
                    "x": float(fx), "y": float(fy),
                    "tracked": i < int(n * 0.7),
                    "response": 50 + (hash(i * 3) % 150),
                })
            return result
        return []
    
    def get_mavlink_stats(self) -> dict:
        return dict(self._rt.get_mavlink())
    
    def get_sensor_modes(self) -> dict:
        try:
            if hasattr(self._rt, 'get_sensor_modes'):
                return dict(self._rt.get_sensor_modes())
        except Exception:
            pass
        # Fallback: native mode without new C++ binding = mavlink
        return {
            "imu": "mavlink",
            "baro": "mavlink",
            "gps": "mavlink",
            "rangefinder": "mavlink",
            "optical_flow": "mavlink",
            "hw_info": {
                "i2c_available": False,
                "imu_detected": False,
                "baro_detected": False,
                "gps_detected": False,
                "spi_available": False,
                "uart_available": False,
                "imu_model": "none",
                "baro_model": "none",
                "gps_model": "none",
            },
        }
    
    def get_performance(self) -> dict:
        return dict(self._rt.get_performance())
    
    def get_sim_config(self) -> dict:
        return dict(self._rt.get_sim_config())
    
    def set_sim_config(self, config: dict):
        self._rt.set_sim_config(config)
    
    def get_vo_profiles(self) -> list:
        try:
            if hasattr(self._rt, 'get_vo_profiles'):
                return [dict(p) for p in self._rt.get_vo_profiles()]
        except Exception:
            pass
        return list(self._VO_MODES)
    
    def set_vo_profile(self, mode_id: int) -> bool:
        if 0 <= mode_id < len(self._VO_MODES):
            # Try C++ first
            try:
                if hasattr(self._rt, 'set_vo_profile'):
                    self._rt.set_vo_profile(mode_id)
            except Exception:
                pass
            # Always update managed state
            self._active_vo_mode = mode_id
            return True
        return False

    # ── VO Fallback: Python-side monitoring + injection ──

    def _check_rc_vo_reset(self):
        """Check RC channel for SET HOMEPOINT trigger (edge-detected)."""
        try:
            mavlink = dict(self._rt.get_mavlink())
            rc = mavlink.get('rc_channels')
            if not rc or len(rc) <= self._rc_reset_channel:
                return
            pwm = rc[self._rc_reset_channel]
            if pwm >= self._rc_reset_threshold:
                if not self._rc_reset_armed:
                    self._rc_reset_armed = True
                    # Trigger vo_reset
                    self._rt.send_command("vo_reset", 0, 0)
                    self._vo_trail = []  # clear trail on reset
                    self._send_statustext(6, "JT0: RC HOMEPOINT SET")
                    sys.stderr.write(f"[VO Reset] RC ch{self._rc_reset_channel + 1} = {pwm} >= {self._rc_reset_threshold} → HOMEPOINT SET\n")
                    sys.stderr.flush()
            else:
                self._rc_reset_armed = False
        except Exception:
            pass

    def _record_vo_trail(self):
        """Record VO position for 3D trail visualization."""
        now = time.time()
        try:
            cam = dict(self._rt.get_camera())
            # Accumulate deltas into absolute position
            self._vo_pos_x += cam.get('vo_dx', 0)
            self._vo_pos_y += cam.get('vo_dy', 0)
            self._vo_pos_z += cam.get('vo_dz', 0)
        except Exception:
            return
        # Sample at interval
        if now - self._vo_trail_last_t < self._vo_trail_interval:
            return
        self._vo_trail_last_t = now
        self._vo_trail.append({
            'x': round(self._vo_pos_x, 4),
            'y': round(self._vo_pos_y, 4),
            'z': round(self._vo_pos_z, 4),
            't': round(now - self._start_time, 1),
        })
        if len(self._vo_trail) > self._vo_trail_max:
            self._vo_trail = self._vo_trail[-self._vo_trail_max:]

    def get_vo_trail(self) -> list:
        """Return VO position trail for 3D visualization."""
        return list(self._vo_trail)

    def vo_fallback_tick(self):
        """Called periodically (~10Hz from WebSocket/background).
        Monitors CSI confidence and manages thermal frame injection.
        Also checks RC channel for vo_reset and records VO trail."""
        if not self.running:
            return
        
        # ── RC-based VO Reset (always active, not just during fallback) ──
        self._check_rc_vo_reset()
        
        # ── VO Trail recording ──
        self._record_vo_trail()
        
        # Check if USB thermal camera is available
        self.__init_multicam()
        has_thermal = (self._usb_capture is not None and self._usb_capture.streaming)
        
        if not has_thermal:
            return
        
        # Check if C++ has new fallback bindings
        has_bindings = hasattr(self._rt, 'inject_frame')
        if not has_bindings:
            return
        
        if not self._vo_fallback_active:
            # ── Normal mode: monitor brightness ONLY ──
            # Confidence is USELESS: VO tracks noise at high conf on dark frames!
            # Brightness is the reliable indicator: dark = camera blocked
            
            if time.time() < self._vo_fallback_cooldown_until:
                return
            
            try:
                cam = dict(self._rt.get_camera())
                brightness = cam.get('frame_brightness', 128)
            except Exception:
                return
            
            self._vo_bright_history.append(brightness)
            if len(self._vo_bright_history) > self._VO_WINDOW_SIZE:
                self._vo_bright_history = self._vo_bright_history[-self._VO_WINDOW_SIZE:]
            
            if len(self._vo_bright_history) >= self._VO_MIN_SAMPLES:
                avg_bright = sum(self._vo_bright_history) / len(self._vo_bright_history)
                
                if avg_bright < self._VO_BRIGHT_DROP:
                    # ── TRIGGER: camera is dark ──
                    reason = f"CSI dark avg_bright={avg_bright:.0f}<{self._VO_BRIGHT_DROP}"
                    sys.stderr.write(f"[VO Fallback] TRIGGERING: {reason}\n")
                    sys.stderr.flush()
                    
                    try:
                        self._rt.activate_fallback(reason)
                    except Exception as e:
                        sys.stderr.write(f"[VO Fallback] activate error: {e}\n")
                        sys.stderr.flush()
                        return
                    
                    self._vo_fallback_active = True
                    self._vo_fallback_start_time = time.time()
                    self._vo_bright_history = []
                    self._send_statustext(4, "JT0: VO FALLBACK ACTIVE")
                    
                    self._vo_fallback_stop.clear()
                    self._vo_fallback_thread = threading.Thread(
                        target=self._vo_inject_loop, daemon=True)
                    self._vo_fallback_thread.start()
        else:
            # ── Fallback mode: check CSI recovery via brightness + probe ──
            # Minimum time in fallback before checking recovery
            if time.time() - self._vo_fallback_start_time < self._VO_MIN_FALLBACK_S:
                return
            
            # Get CSI brightness from camera stats (updated during CSI probes)
            try:
                cam = dict(self._rt.get_camera())
                csi_brightness = cam.get('frame_brightness', 0)
            except Exception:
                csi_brightness = 0
            
            # Get probe confidence from C++ fallback state
            try:
                fs = dict(self._rt.get_fallback_state())
                probe_conf = fs.get('last_csi_probe_conf', 0)
            except Exception:
                probe_conf = 0
            
            # Recovery condition: brightness recovered OR confidence good enough
            # Primary: brightness > BRIGHT_RECOVER (symmetric with trigger)
            # Secondary: probe confidence > CONF_RECOVER (for cases where brightness is unreliable)
            recovery_reason = None
            if csi_brightness >= self._VO_BRIGHT_RECOVER:
                recovery_reason = f"brightness={csi_brightness:.0f}>={self._VO_BRIGHT_RECOVER}"
            elif probe_conf >= self._VO_CONF_RECOVER:
                recovery_reason = f"probe_conf={probe_conf:.2f}>={self._VO_CONF_RECOVER}"
            
            if recovery_reason:
                self._vo_csi_good_probes += 1
                sys.stderr.write(f"[VO Fallback] CSI probe good ({recovery_reason}), "
                               f"count={self._vo_csi_good_probes}/{self._VO_PROBES_TO_RECOVER}\n")
                sys.stderr.flush()
                
                if self._vo_csi_good_probes >= self._VO_PROBES_TO_RECOVER:
                    # ── RECOVER TO CSI ──
                    sys.stderr.write(f"[VO Fallback] CSI recovered ({recovery_reason}) → deactivating\n")
                    sys.stderr.flush()
                    
                    self._vo_fallback_stop.set()
                    if self._vo_fallback_thread:
                        self._vo_fallback_thread.join(timeout=2)
                    
                    try:
                        self._rt.deactivate_fallback()
                    except Exception as e:
                        sys.stderr.write(f"[VO Fallback] deactivate error: {e}\n")
                        sys.stderr.flush()
                    
                    self._vo_fallback_active = False
                    self._vo_bright_history = []
                    self._vo_csi_good_probes = 0
                    self._vo_fallback_cooldown_until = time.time() + self._VO_COOLDOWN_S
                    self._vo_python_features = []  # clear Python features on recovery
                    self._send_statustext(6, "JT0: VO CSI RECOVERED")
            else:
                # Bad probe — reset good probe counter
                self._vo_csi_good_probes = 0

    def gps_warn_tick(self):
        """Called periodically (~1Hz from background monitor).

        Watches GPS fix quality and Kalman position_uncertainty.
        Sends MAVLink STATUSTEXT warnings when:
          - GPS fix_type < 3 (no 3D fix)  AND  armed
          - uncertainty > _GPS_WARN_THRESHOLD_M  → WARNING
          - uncertainty > _GPS_WARN_CRITICAL_M   → CRITICAL

        Uses Kalman covariance-derived uncertainty (Fix #40): physically
        meaningful, grows with real drift, stays low in hover.
        Debounced: at most one message per _GPS_WARN_DEBOUNCE_S seconds.
        """
        if not self.running or not self._GPS_WARN_ENABLED:
            return

        try:
            state = dict(self._rt.get_state())
            gps   = state.get('gps', {})
            armed = state.get('armed', False)
        except Exception:
            return

        if not armed:
            # Not flying — reset warning level silently
            self._gps_warn_level = 'ok'
            return

        # GPS is good → reset, no action
        fix_type = gps.get('fix_type', 0) if isinstance(gps, dict) else 0
        if fix_type >= self._GPS_WARN_MIN_FIX:
            if self._gps_warn_level != 'ok':
                sys.stderr.write(f"[GPS Warn] GPS recovered (fix_type={fix_type})\n")
                sys.stderr.flush()
            self._gps_warn_level = 'ok'
            return

        # GPS degraded — check uncertainty
        try:
            cam_stats = self.get_camera_stats()
            uncertainty = cam_stats.get('vo_position_uncertainty', 0.0)
        except Exception:
            return

        now = time.time()
        debounced = (now - self._gps_warn_last_sent) >= self._GPS_WARN_DEBOUNCE_S

        if uncertainty >= self._GPS_WARN_CRITICAL_M:
            new_level = 'critical'
        elif uncertainty >= self._GPS_WARN_THRESHOLD_M:
            new_level = 'warn'
        else:
            new_level = 'ok'

        if new_level == 'ok':
            self._gps_warn_level = 'ok'
            return

        if new_level == self._gps_warn_level and not debounced:
            return  # same level, too soon to repeat

        # Escalation or debounce timeout → send
        self._gps_warn_level = new_level
        self._gps_warn_last_sent = now

        if new_level == 'critical':
            msg = f"JT0: GPS LOST unc={uncertainty:.1f}m RTL ADVISED"
            severity = 2  # MAV_SEVERITY_CRITICAL
        else:
            msg = f"JT0: GPS DEGRADED unc={uncertainty:.1f}m VO only"
            severity = 4  # MAV_SEVERITY_WARNING

        sys.stderr.write(f"[GPS Warn] {msg} fix={fix_type}\n")
        sys.stderr.flush()
        self._send_statustext(severity, msg)

    def _vo_inject_loop(self):
        """Background thread: captures thermal JPEG → grayscale → injects to C++ VO."""
        sys.stderr.write(f"[VO Fallback] Injection loop started — PIL={PIL_AVAILABLE} FILTERS={PILLOW_FILTERS} NUMPY={NUMPY_AVAILABLE}\n")
        sys.stderr.flush()
        
        while not self._vo_fallback_stop.is_set():
            if not self._usb_capture or not self._usb_capture.streaming:
                time.sleep(0.2)
                continue
            
            # Get latest thermal JPEG
            jpeg_data = self._usb_capture.capture_frame()
            if not jpeg_data:
                time.sleep(0.05)
                continue
            
            # Decode JPEG → grayscale → resize to VO resolution
            gray_bytes, gray_img = self._decode_jpeg_to_gray(jpeg_data)
            if not gray_bytes:
                if not getattr(self, '_decode_skip_logged', False):
                    sys.stderr.write(f"[VO Fallback] gray_bytes empty — PIL={PIL_AVAILABLE} jpeg_len={len(jpeg_data)} FILTERS={PILLOW_FILTERS}\n")
                    sys.stderr.flush()
                    self._decode_skip_logged = True
                time.sleep(0.05)
                continue
            
            # Run Python feature detection on the grayscale frame (for visualization)
            # Uses Pillow filters (no numpy needed) — detects corners on thermal content
            if gray_img and PILLOW_FILTERS:
                try:
                    py_feats = self._detect_features_pillow(gray_img)
                    self._vo_python_features = py_feats
                    if not getattr(self, '_pyfeat_logged', False):
                        sys.stderr.write(f"[VO PyDetect] Pillow corner detector: {len(py_feats)} features\n")
                        sys.stderr.flush()
                        self._pyfeat_logged = True
                except Exception as e:
                    if not getattr(self, '_pyfeat_err_logged', False):
                        sys.stderr.write(f"[VO PyDetect] Error: {e}\n")
                        sys.stderr.flush()
                        self._pyfeat_err_logged = True
            elif gray_bytes and len(gray_bytes) == self._VO_INJECT_W * self._VO_INJECT_H:
                # Pure Python fallback (no Pillow, no numpy) — slower but works
                try:
                    py_feats = self._detect_features_raw(gray_bytes, self._VO_INJECT_W, self._VO_INJECT_H)
                    self._vo_python_features = py_feats
                    if not getattr(self, '_rawfeat_logged', False):
                        sys.stderr.write(f"[VO PyDetect] Raw detector: {len(py_feats)} features\n")
                        sys.stderr.flush()
                        self._rawfeat_logged = True
                except Exception as e:
                    if not getattr(self, '_rawfeat_err_logged', False):
                        sys.stderr.write(f"[VO PyDetect] raw detector error: {e}\n")
                        sys.stderr.flush()
                        self._rawfeat_err_logged = True
            else:
                if not getattr(self, '_no_detect_logged', False):
                    sys.stderr.write(f"[VO PyDetect] No detection method available! gray_img={gray_img is not None} FILTERS={PILLOW_FILTERS} NUMPY={NUMPY_AVAILABLE} gray_len={len(gray_bytes)} expected={self._VO_INJECT_W * self._VO_INJECT_H}\n")
                    sys.stderr.flush()
                    self._no_detect_logged = True
            
            # Inject into C++ VO pipeline
            try:
                self._rt.inject_frame(gray_bytes, self._VO_INJECT_W, self._VO_INJECT_H)
            except Exception as e:
                sys.stderr.write(f"[VO Fallback] inject error: {e}\n")
                sys.stderr.flush()
            
            # Match thermal camera capture rate (~5fps → sleep 0.15s between injects)
            self._vo_fallback_stop.wait(0.15)
        
        sys.stderr.write("[VO Fallback] Injection loop stopped\n")
        sys.stderr.flush()

    def _decode_jpeg_to_gray(self, jpeg_data: bytes):
        """Decode JPEG to grayscale at VO resolution (320x240).
        Returns (bytes, PIL.Image|None). Uses Pillow if available, else djpeg subprocess.
        """
        w, h = self._VO_INJECT_W, self._VO_INJECT_H
        
        # Method 1: Pillow (fast, returns Image for feature detection)
        if PIL_AVAILABLE:
            try:
                img = Image.open(io.BytesIO(jpeg_data))
                img = img.convert('L')
                # Pillow version compatibility: NEAREST may be in Resampling enum
                try:
                    resample = Image.Resampling.NEAREST
                except AttributeError:
                    resample = Image.NEAREST
                img = img.resize((w, h), resample)
                return img.tobytes(), img
            except Exception as e:
                if not getattr(self, '_decode_err_logged', False):
                    sys.stderr.write(f"[VO Decode] PIL error: {e}\n")
                    sys.stderr.flush()
                    self._decode_err_logged = True
                return b'', None
        
        # Method 2: djpeg subprocess (libjpeg-turbo, available on Pi OS)
        try:
            proc = subprocess.run(
                ['djpeg', '-grayscale', '-pnm'],
                input=jpeg_data, capture_output=True, timeout=2
            )
            if proc.returncode == 0 and proc.stdout:
                pgm = proc.stdout
                # Parse PGM P5 header: "P5\nW H\nMAXVAL\n<pixels>"
                idx = 0
                lines_found = 0
                while lines_found < 3 and idx < min(len(pgm), 100):
                    if pgm[idx:idx+1] == b'\n':
                        lines_found += 1
                    idx += 1
                header = pgm[:idx].decode('ascii', errors='ignore').split()
                raw = pgm[idx:]
                if len(header) >= 3:
                    ow, oh = int(header[1]), int(header[2])
                    if len(raw) >= ow * oh:
                        # Nearest-neighbor resize to VO resolution
                        resized = bytearray(w * h)
                        xr, yr = ow / w, oh / h
                        for yo in range(h):
                            yi = int(yo * yr)
                            for xo in range(w):
                                resized[yo * w + xo] = raw[yi * ow + int(xo * xr)]
                        if not getattr(self, '_djpeg_ok_logged', False):
                            sys.stderr.write(f"[VO] djpeg decode OK: {ow}x{oh} -> {w}x{h}\n")
                            sys.stderr.flush()
                            self._djpeg_ok_logged = True
                        return bytes(resized), None
        except FileNotFoundError:
            if not getattr(self, '_djpeg_warn', False):
                sys.stderr.write("[VO] WARNING: Neither Pillow nor djpeg — "
                                "install: sudo apt install python3-pil\n")
                sys.stderr.flush()
                self._djpeg_warn = True
        except Exception:
            pass
        return b'', None

    @staticmethod
    def _detect_features_pillow(gray_img, max_features=120):
        """Corner detection using Pillow only (no numpy/scipy dependency).
        
        Uses Sobel filters → min(|Ix|, |Iy|) corner response → threshold.
        Returns features at VO resolution coordinates (320x240).
        ~20-40ms on Pi 4 for 320x240 frame.
        """
        w, h = gray_img.size  # 320x240
        border = 12  # skip frame border (capture card artifacts)
        
        # Horizontal Sobel → gradient in X direction (centered at 128)
        Ix = gray_img.filter(ImageFilter.Kernel(
            (3, 3), [-1, 0, 1, -2, 0, 2, -1, 0, 1], scale=1, offset=128))
        # Vertical Sobel → gradient in Y direction
        Iy = gray_img.filter(ImageFilter.Kernel(
            (3, 3), [-1, -2, -1, 0, 0, 0, 1, 2, 1], scale=1, offset=128))
        
        # |Ix| and |Iy| — distance from center (128)
        Ix_abs = Ix.point(lambda p: abs(p - 128))
        Iy_abs = Iy.point(lambda p: abs(p - 128))
        
        # Corner response = min(|Ix|, |Iy|) — strong in BOTH directions = corner
        corner = ImageChops.darker(Ix_abs, Iy_abs)
        
        # Get raw pixel bytes for fast iteration
        corner_data = corner.tobytes()
        
        # Collect candidates above threshold, excluding border
        threshold = 10
        candidates = []
        for y in range(border, h - border):
            row_start = y * w + border
            row_end = y * w + w - border
            for i in range(row_start, row_end):
                val = corner_data[i]
                if val > threshold:
                    candidates.append((i % w, y, val))
        
        if not candidates:
            return []
        
        # Sort by response strength (strongest first)
        candidates.sort(key=lambda c: -c[2])
        
        # Sparse subsampling — enforce minimum distance between features
        # This prevents clusters of dots at the same corner
        min_dist_sq = 6 * 6  # 6px minimum distance
        selected = []
        for c in candidates:
            too_close = False
            for s in selected:
                dx = c[0] - s[0]
                dy = c[1] - s[1]
                if dx * dx + dy * dy < min_dist_sq:
                    too_close = True
                    break
            if not too_close:
                selected.append(c)
                if len(selected) >= max_features:
                    break
        
        return [{"x": float(c[0]), "y": float(c[1]),
                 "tracked": True, "response": c[2]}
                for c in selected]

    @staticmethod
    def _detect_features_raw(gray_bytes, width, height, max_features=80):
        """Pure Python corner detector — no Pillow, no numpy.
        
        Slower (~100ms on Pi 4) but works when no libraries available.
        Uses simple gradient-based corner detection on raw bytes.
        """
        border = 12
        threshold = 8
        min_dist = 8
        
        # Compute corner response at sampled points (skip every 2px for speed)
        candidates = []
        step = 2
        for y in range(border, height - border, step):
            for x in range(border, width - border, step):
                idx = y * width + x
                # Horizontal gradient: |pixel(x+2) - pixel(x-2)| (wider for robustness)
                gx = abs(gray_bytes[idx + 2] - gray_bytes[idx - 2])
                # Vertical gradient: |pixel(y+2) - pixel(y-2)|
                gy = abs(gray_bytes[idx + 2*width] - gray_bytes[idx - 2*width])
                # Corner = strong in BOTH directions
                resp = min(gx, gy)
                if resp > threshold:
                    candidates.append((x, y, resp))
        
        if not candidates:
            return []
        
        # Sort by response, sparse subsample
        candidates.sort(key=lambda c: -c[2])
        min_dist_sq = min_dist * min_dist
        selected = []
        for c in candidates:
            too_close = False
            for s in selected:
                dx = c[0] - s[0]
                dy = c[1] - s[1]
                if dx * dx + dy * dy < min_dist_sq:
                    too_close = True
                    break
            if not too_close:
                selected.append(c)
                if len(selected) >= max_features:
                    break
        
        return [{"x": float(c[0]), "y": float(c[1]),
                 "tracked": True, "response": c[2]}
                for c in selected]

    # ── Multi-Camera Support ──
    # Until C++ bindings are recompiled on the Pi with multi-camera,
    # provide managed state for secondary (thermal) camera.

    def __init_multicam(self):
        """Lazy-init multi-camera state with real USB camera detection."""
        if not hasattr(self, '_secondary_camera'):
            # Try to find real USB camera
            self._usb_capture = None
            try:
                from usb_camera import find_usb_camera, USBCameraCapture
                dev_path, card, driver = find_usb_camera()
                if dev_path:
                    self._usb_capture = USBCameraCapture(dev_path, 640, 480)
                    if self._usb_capture.open():
                        self._secondary_camera = {
                            "slot": "SECONDARY",
                            "camera_type": "USB_THERMAL",
                            "camera_open": True,
                            "active": False,
                            "frame_count": 0,
                            "fps_actual": 0.0,
                            "width": self._usb_capture.actual_w,
                            "height": self._usb_capture.actual_h,
                            "label": f"USB Thermal ({card or 'Down'})",
                            "device": dev_path,
                            "last_capture_time": 0,
                            "frame_format": self._usb_capture.frame_format,
                        }
                        import sys
                        sys.stderr.write(f"[MultiCam] USB camera ready: {card} @ {dev_path} ({self._usb_capture.actual_w}x{self._usb_capture.actual_h})\n")
                        sys.stderr.flush()
                        return
                    else:
                        self._usb_capture = None
            except Exception as e:
                import sys
                sys.stderr.write(f"[MultiCam] USB camera init failed: {e}\n")
                sys.stderr.flush()
                self._usb_capture = None
            
            # Fallback: simulated secondary
            self._secondary_camera = {
                "slot": "SECONDARY",
                "camera_type": "USB_THERMAL",
                "camera_open": False,
                "active": False,
                "frame_count": 0,
                "fps_actual": 0.0,
                "width": 256,
                "height": 192,
                "label": "USB Thermal (not connected)",
                "device": "none",
                "last_capture_time": 0,
                "frame_format": "gray",
            }

    def get_cameras(self) -> list:
        """Return info about all camera slots (Variant B: CSI priority)."""
        self.__init_multicam()
        cam = self.get_camera_stats()
        cam_type = cam.get("camera_type", "SIM")
        csi_name = cam.get("csi_sensor_name", "")
        
        # Determine primary label based on camera type
        if cam_type == "PI_CSI":
            if csi_name and csi_name != "none":
                label = f"{csi_name} (VO)"
            else:
                label = "CSI (VO)"
            device = "rpicam-vid"
        elif cam_type == "USB":
            label = "USB (VO fallback)"
            device = "/dev/video0"
        else:
            label = "Simulated (VO)"
            device = "simulated"
        
        primary = {
            "slot": "PRIMARY",
            "camera_type": cam_type,
            "camera_open": cam.get("camera_open", False),
            "active": True,
            "frame_count": cam.get("frame_count", 0),
            "fps_actual": cam.get("fps_actual", 0),
            "width": cam.get("width", 320),
            "height": cam.get("height", 240),
            "label": label,
            "device": device,
            "has_vo": True,
            "csi_sensor": csi_name if csi_name and csi_name != "none" else None,
        }
        secondary = dict(self._secondary_camera)
        secondary["has_vo"] = False
        return [primary, secondary]

    def get_secondary_camera_stats(self) -> dict:
        self.__init_multicam()
        return dict(self._secondary_camera)

    def capture_secondary(self) -> bool:
        self.__init_multicam()
        if self._usb_capture and self._usb_capture.streaming:
            # Continuous stream — frame is always available
            self._secondary_camera["active"] = True
            self._secondary_camera["frame_count"] = self._usb_capture.frame_count
            self._secondary_camera["last_capture_time"] = time.time() - self._start_time
            self._secondary_camera["fps_actual"] = 5.0
            return True
        self._secondary_camera["frame_count"] += 1
        self._secondary_camera["last_capture_time"] = time.time() - self._start_time
        return True

    def get_secondary_frame_data(self) -> bytes:
        """Return latest frame from USB continuous stream or simulated."""
        self.__init_multicam()
        if self._usb_capture and self._usb_capture.streaming:
            frame = self._usb_capture.capture_frame()
            if frame:
                self._secondary_camera['active'] = True
                self._secondary_camera['frame_count'] = self._usb_capture.frame_count
                return frame
        import math
        import random
        w = self._secondary_camera.get("width", 256)
        h = self._secondary_camera.get("height", 192)
        t = time.time() - self._start_time
        cx1 = 128 + 30 * math.sin(t * 0.1)
        cy1 = 96 + 20 * math.cos(t * 0.15)
        data = bytearray(w * h)
        for y in range(h):
            dy1_sq = (y - cy1) ** 2
            for x in range(w):
                val = 40
                d1 = math.sqrt((x - cx1)**2 + dy1_sq)
                if d1 < 40:
                    val += int(160 * (1.0 - d1 / 40.0))
                data[y * w + x] = max(0, min(255, val + random.randint(-3, 3)))
        return bytes(data)
