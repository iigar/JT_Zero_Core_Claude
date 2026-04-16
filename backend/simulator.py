"""
JT-Zero Python Runtime Simulator
Mirrors the C++ runtime behavior for web dashboard integration.
Generates realistic simulated sensor data.
"""

import time
import math
import random
import threading
from dataclasses import dataclass, field, asdict
from collections import deque
from enum import IntEnum

class FlightMode(IntEnum):
    IDLE = 0
    ARMED = 1
    TAKEOFF = 2
    HOVER = 3
    NAVIGATE = 4
    LAND = 5
    RTL = 6
    EMERGENCY = 7

class EventType(IntEnum):
    NONE = 0
    SENSOR_IMU_UPDATE = 1
    SENSOR_BARO_UPDATE = 2
    SENSOR_GPS_UPDATE = 3
    SENSOR_RANGE_UPDATE = 4
    SENSOR_FLOW_UPDATE = 5
    SYSTEM_STARTUP = 6
    SYSTEM_SHUTDOWN = 7
    SYSTEM_HEARTBEAT = 8
    SYSTEM_ERROR = 9
    SYSTEM_WARNING = 10
    FLIGHT_ARM = 11
    FLIGHT_DISARM = 12
    FLIGHT_TAKEOFF = 13
    FLIGHT_LAND = 14
    FLIGHT_RTL = 15
    FLIGHT_HOLD = 16
    FLIGHT_ALTITUDE_REACHED = 17
    FLIGHT_OBSTACLE_DETECTED = 18
    CAMERA_FRAME_READY = 19
    CAMERA_VO_UPDATE = 20
    MAVLINK_CONNECTED = 21
    MAVLINK_DISCONNECTED = 22
    MAVLINK_HEARTBEAT = 23
    CMD_USER = 24
    CMD_API = 25

EVENT_NAMES = {v: v.name for v in EventType}
MODE_NAMES = {v: v.name for v in FlightMode}

@dataclass
class IMUData:
    gyro_x: float = 0.0
    gyro_y: float = 0.0
    gyro_z: float = 0.0
    acc_x: float = 0.0
    acc_y: float = 0.0
    acc_z: float = -9.81
    valid: bool = True

@dataclass
class BarometerData:
    pressure: float = 1013.25
    altitude: float = 0.0
    temperature: float = 22.0
    valid: bool = True

@dataclass
class GPSData:
    lat: float = 50.4501
    lon: float = 30.5234
    alt: float = 150.0
    speed: float = 0.0
    satellites: int = 12
    fix_type: int = 3
    valid: bool = True

@dataclass
class RangefinderData:
    distance: float = 0.0
    signal_quality: float = 0.95
    valid: bool = True

@dataclass
class OpticalFlowData:
    flow_x: float = 0.0
    flow_y: float = 0.0
    quality: int = 200
    ground_distance: float = 3.0
    valid: bool = True

@dataclass
class EventRecord:
    timestamp: float = 0.0
    type: str = "NONE"
    priority: int = 0
    message: str = ""

@dataclass
class ThreadStats:
    name: str = ""
    target_hz: int = 0
    actual_hz: float = 0.0
    cpu_percent: float = 0.0
    loop_count: int = 0
    max_latency_us: int = 0
    running: bool = False

@dataclass
class SystemState:
    flight_mode: str = "IDLE"
    armed: bool = False
    battery_voltage: float = 12.6
    battery_percent: float = 100.0
    cpu_usage: float = 15.0
    ram_usage_mb: float = 45.0
    cpu_temp: float = 42.0
    uptime_sec: int = 0
    event_count: int = 0
    error_count: int = 0
    roll: float = 0.0
    pitch: float = 0.0
    yaw: float = 0.0
    altitude_agl: float = 0.0
    vx: float = 0.0
    vy: float = 0.0
    vz: float = 0.0
    imu: dict = field(default_factory=lambda: asdict(IMUData()))
    baro: dict = field(default_factory=lambda: asdict(BarometerData()))
    gps: dict = field(default_factory=lambda: asdict(GPSData()))
    rangefinder: dict = field(default_factory=lambda: asdict(RangefinderData()))
    optical_flow: dict = field(default_factory=lambda: asdict(OpticalFlowData()))

@dataclass
class CameraStats:
    camera_type: str = "SIMULATED"
    camera_open: bool = True
    frame_count: int = 0
    fps_actual: float = 14.8
    width: int = 320
    height: int = 240
    vo_features_detected: int = 0
    vo_features_tracked: int = 0
    vo_inlier_count: int = 0
    vo_tracking_quality: float = 0.0
    vo_confidence: float = 0.0
    vo_position_uncertainty: float = 0.0
    vo_total_distance: float = 0.0
    vo_dx: float = 0.0
    vo_dy: float = 0.0
    vo_dz: float = 0.0
    vo_vx: float = 0.0
    vo_vy: float = 0.0
    vo_valid: bool = True
    # Hardware profile
    active_profile: int = 0
    profile_name: str = "Pi Zero 2W"
    # VO Mode (switchable)
    vo_mode: int = 1
    vo_mode_name: str = "Balanced"
    # Adaptive parameters
    altitude_zone: int = 0
    altitude_zone_name: str = "LOW"
    adaptive_fast_thresh: float = 30.0
    adaptive_lk_window: float = 5.0
    # Hover yaw correction
    hover_detected: bool = False
    hover_duration: float = 0.0
    yaw_drift_rate: float = 0.0
    corrected_yaw: float = 0.0
    # VO Fallback state
    vo_source: str = "CSI_PRIMARY"
    vo_fallback_reason: str = ""
    vo_fallback_duration: float = 0.0
    vo_fallback_switches: int = 0
    # Frame brightness
    frame_brightness: float = 128.0

@dataclass
class MAVLinkStats:
    state: str = "CONNECTED"
    messages_sent: int = 0
    messages_received: int = 0
    errors: int = 0
    link_quality: float = 0.95
    system_id: int = 1
    component_id: int = 191
    fc_system_id: int = 1
    fc_autopilot: str = "ArduPilot"
    fc_firmware: str = "ArduPilot 4.5.0"
    fc_type: str = "QUADROTOR"
    fc_armed: bool = False
    vision_pos_sent: int = 0
    odometry_sent: int = 0
    optical_flow_sent: int = 0

class JTZeroSimulator:
    def __init__(self):
        self.state = SystemState()
        self.events = deque(maxlen=500)
        self.telemetry_history = deque(maxlen=2048)
        self.running = False
        self._lock = threading.Lock()
        self._thread = None
        self._tick = 0
        self._start_time = time.time()
        
        # Thread stats
        self.thread_stats = [
            ThreadStats("T0_Supervisor", 10, 10.0, 2.1, 0, 4, True),
            ThreadStats("T1_Sensors", 200, 199.8, 8.3, 0, 886, True),
            ThreadStats("T2_Events", 200, 200.0, 3.1, 0, 2, True),
            ThreadStats("T3_Reflex", 200, 200.0, 1.5, 0, 1, True),
            ThreadStats("T4_Rules", 20, 19.9, 0.8, 0, 3, True),
            ThreadStats("T5_MAVLink", 50, 49.8, 1.2, 0, 5, True),
            ThreadStats("T6_Camera", 15, 14.9, 12.5, 0, 15000, True),
            ThreadStats("T7_API", 30, 29.7, 0.5, 0, 2, True),
        ]
        
        # Engine stats
        self.engine_stats = {
            "events": {"total": 0, "dropped": 0, "pending": 0, "queue_size": 1024},
            "reflexes": {"rules": 3, "total_fires": 0, "avg_latency_us": 1.2},
            "rules": {"count": 3, "total_evaluations": 0},
            "memory": {"telemetry_records": 0, "event_records": 0, "usage_bytes": 434176},
            "output": {"total_outputs": 0, "pending": 0},
        }
        
        # Camera & MAVLink stats
        self.camera_stats = CameraStats()
        self.mavlink_stats = MAVLinkStats()
        
        # Multi-camera: secondary thermal camera
        self._usb_capture = None
        self._last_secondary_frame = None
        try:
            from usb_camera import find_usb_camera, USBCameraCapture
            dev_path, card, driver = find_usb_camera()
            if dev_path:
                self._usb_capture = USBCameraCapture(dev_path, 256, 192)
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
                    print(f"[Simulator] USB camera ready: {card} @ {dev_path}")
                else:
                    self._usb_capture = None
        except Exception as e:
            print(f"[Simulator] USB camera init failed: {e}")
            self._usb_capture = None
        
        if not self._usb_capture:
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
        self._secondary_capturing = False
        
    def start(self):
        if self.running:
            return
        self.running = True
        self._start_time = time.time()
        self._emit_event(EventType.SYSTEM_STARTUP, 255, "JT-Zero simulator started")
        self._thread = threading.Thread(target=self._run_loop, daemon=True)
        self._thread.start()
    
    def stop(self):
        self.running = False
        self._emit_event(EventType.SYSTEM_SHUTDOWN, 255, "JT-Zero simulator stopped")
        if self._thread:
            self._thread.join(timeout=2)
    
    def send_command(self, cmd: str, param1: float = 0, param2: float = 0) -> bool:
        with self._lock:
            if cmd == "arm":
                self.state.armed = True
                self.state.flight_mode = "ARMED"
                self._emit_event(EventType.FLIGHT_ARM, 200, "Armed by user command")
            elif cmd == "disarm":
                self.state.armed = False
                self.state.flight_mode = "IDLE"
                self._emit_event(EventType.FLIGHT_DISARM, 200, "Disarmed by user command")
            elif cmd == "takeoff":
                if self.state.armed:
                    self.state.flight_mode = "TAKEOFF"
                    self._emit_event(EventType.FLIGHT_TAKEOFF, 200, f"Takeoff to {param1}m")
                else:
                    return False
            elif cmd == "land":
                self.state.flight_mode = "LAND"
                self._emit_event(EventType.FLIGHT_LAND, 200, "Landing initiated")
            elif cmd == "rtl":
                self.state.flight_mode = "RTL"
                self._emit_event(EventType.FLIGHT_RTL, 220, "Return to launch")
            elif cmd == "hold":
                self.state.flight_mode = "HOVER"
                self._emit_event(EventType.FLIGHT_HOLD, 150, "Position hold")
            elif cmd == "emergency":
                self.state.armed = False
                self.state.flight_mode = "EMERGENCY"
                self._emit_event(EventType.SYSTEM_ERROR, 255, "EMERGENCY STOP")
            elif cmd == "vo_reset":
                self.camera.vo_total_distance = 0.0
                self.camera.vo_position_uncertainty = 0.0
                self.camera.vo_dx = 0.0
                self.camera.vo_dy = 0.0
                self.camera.vo_dz = 0.0
                self._emit_event(EventType.CMD_USER, 200, "VO origin reset (SET HOMEPOINT)")
            else:
                self._emit_event(EventType.CMD_USER, 100, f"Custom: {cmd}")
            return True
    
    def get_state(self) -> dict:
        with self._lock:
            return asdict(self.state)
    
    def get_events(self, count: int = 50) -> list:
        with self._lock:
            items = list(self.events)[-count:]
            return [asdict(e) for e in items]
    
    def get_telemetry_history(self, count: int = 100) -> list:
        with self._lock:
            items = list(self.telemetry_history)[-count:]
            return items
    
    def get_thread_stats(self) -> list:
        with self._lock:
            return [asdict(t) for t in self.thread_stats]
    
    def get_engine_stats(self) -> dict:
        with self._lock:
            return dict(self.engine_stats)
    
    def get_camera_stats(self) -> dict:
        with self._lock:
            return asdict(self.camera_stats)
    
    def get_mavlink_stats(self) -> dict:
        with self._lock:
            return asdict(self.mavlink_stats)
    
    def _emit_event(self, etype: EventType, priority: int, message: str):
        ev = EventRecord(
            timestamp=time.time() - self._start_time,
            type=EVENT_NAMES.get(etype, "UNKNOWN"),
            priority=priority,
            message=message
        )
        self.events.append(ev)
        self.state.event_count += 1
        self.engine_stats["events"]["total"] += 1
        self.engine_stats["memory"]["event_records"] += 1
    
    def _run_loop(self):
        while self.running:
            with self._lock:
                self._tick += 1
                t = self._tick / 20.0  # 20 Hz effective
                
                self._update_sensors(t)
                self._update_state(t)
                self._update_camera(t)
                self._update_mavlink(t)
                self._update_thread_stats()
                self._record_telemetry()
                
                # Periodic events
                if self._tick % 20 == 0:
                    self._emit_event(EventType.SYSTEM_HEARTBEAT, 50, "heartbeat")
                    self.engine_stats["rules"]["total_evaluations"] += 1
                
                # Sensor update events (every 2 seconds)
                if self._tick % 40 == 10:
                    self._emit_event(EventType.SENSOR_BARO_UPDATE, 10, 
                        f"alt={self.state.baro['altitude']:.1f}m press={self.state.baro['pressure']:.1f}hPa")
                if self._tick % 40 == 20:
                    self._emit_event(EventType.SENSOR_GPS_UPDATE, 10, 
                        f"lat={self.state.gps['lat']:.6f} lon={self.state.gps['lon']:.6f} sat={self.state.gps['satellites']}")
                if self._tick % 60 == 30:
                    self._emit_event(EventType.SENSOR_RANGE_UPDATE, 10, 
                        f"dist={self.state.rangefinder['distance']:.2f}m q={self.state.rangefinder['signal_quality']:.0%}")
                if self._tick % 60 == 45:
                    self._emit_event(EventType.SENSOR_FLOW_UPDATE, 10,
                        f"flow=({self.state.optical_flow['flow_x']:.3f},{self.state.optical_flow['flow_y']:.3f}) q={self.state.optical_flow['quality']}")
                
                # Occasional warnings
                if self._tick % 200 == 100 and self.state.battery_percent < 50:
                    self._emit_event(EventType.SYSTEM_WARNING, 100, 
                        f"Battery at {self.state.battery_percent:.0f}%")
                
            time.sleep(0.05)  # 20 Hz update
    
    def _update_sensors(self, t: float):
        s = self.state
        
        # IMU
        s.imu["gyro_x"] = 0.01 * math.sin(t * 0.5) + random.gauss(0, 0.002)
        s.imu["gyro_y"] = 0.008 * math.cos(t * 0.7) + random.gauss(0, 0.002)
        s.imu["gyro_z"] = 0.005 * math.sin(t * 0.3) + random.gauss(0, 0.001)
        s.imu["acc_x"] = 0.1 * math.sin(t * 0.2) + random.gauss(0, 0.05)
        s.imu["acc_y"] = 0.08 * math.cos(t * 0.15) + random.gauss(0, 0.05)
        s.imu["acc_z"] = -9.81 + 0.05 * math.sin(t * 0.1) + random.gauss(0, 0.02)
        
        # Barometer
        target_alt = 5.0 + 2.0 * math.sin(t * 0.05)
        if s.flight_mode in ("TAKEOFF", "HOVER", "NAVIGATE"):
            target_alt = 10.0 + 3.0 * math.sin(t * 0.03)
        elif s.flight_mode == "LAND":
            target_alt = max(0, s.baro["altitude"] - 0.1)
        
        s.baro["altitude"] = target_alt + random.gauss(0, 0.1)
        s.baro["pressure"] = 1013.25 - (s.baro["altitude"] * 0.12) + random.gauss(0, 0.01)
        s.baro["temperature"] = 22.0 + random.gauss(0, 0.5) - s.baro["altitude"] * 0.0065
        
        # GPS
        s.gps["lat"] = 50.4501 + 0.0001 * math.sin(t * 0.02)
        s.gps["lon"] = 30.5234 + 0.0001 * math.cos(t * 0.015)
        s.gps["alt"] = 150.0 + 5.0 * math.sin(t * 0.05) + random.gauss(0, 0.3)
        s.gps["speed"] = abs(2.0 + random.gauss(0, 0.5))
        s.gps["satellites"] = max(4, min(16, 12 + random.randint(-2, 2)))
        
        # Rangefinder
        s.rangefinder["distance"] = max(0.1, 3.0 + 1.5 * math.sin(t * 0.1) + random.gauss(0, 0.05))
        s.rangefinder["signal_quality"] = max(0, min(1, 0.95 + random.gauss(0, 0.03)))
        
        # Optical Flow
        s.optical_flow["flow_x"] = 0.05 * math.sin(t * 0.3) + random.gauss(0, 0.01)
        s.optical_flow["flow_y"] = 0.03 * math.cos(t * 0.2) + random.gauss(0, 0.01)
        s.optical_flow["quality"] = max(0, min(255, int(200 + random.gauss(0, 15))))
        s.optical_flow["ground_distance"] = s.rangefinder["distance"]
    
    def _update_state(self, t: float):
        s = self.state
        
        # Attitude from accelerometer (stable, no gyro drift)
        # acc_z = -9.81 when level → negate for correct atan2
        raw_roll  = math.atan2(s.imu["acc_y"], -s.imu["acc_z"]) * 57.2958
        raw_pitch = math.atan2(-s.imu["acc_x"],
                    math.sqrt(s.imu["acc_y"]**2 + s.imu["acc_z"]**2)) * 57.2958
        
        # Low-pass filter (smooth, no jitter)
        lp = 0.15
        s.roll  = s.roll + lp * (raw_roll - s.roll)
        s.pitch = s.pitch + lp * (raw_pitch - s.pitch)
        s.yaw   = (s.yaw + s.imu["gyro_z"] * 0.05 * 57.2958) % 360
        
        s.altitude_agl = s.baro["altitude"]
        s.uptime_sec = int(time.time() - self._start_time)
        
        # Battery drain
        s.battery_voltage = max(10.0, s.battery_voltage - 0.00001)
        s.battery_percent = max(0, (s.battery_voltage - 10.0) / 2.6 * 100)
        
        # CPU simulation
        s.cpu_usage = 15.0 + random.gauss(0, 2)
        s.ram_usage_mb = 45.0 + random.gauss(0, 1)
        s.cpu_temp = 42.0 + random.gauss(0, 0.5)
        
        # Flight mode transitions
        if s.flight_mode == "TAKEOFF" and s.altitude_agl > 8.0:
            s.flight_mode = "HOVER"
            self._emit_event(EventType.FLIGHT_ALTITUDE_REACHED, 150, "Takeoff complete, hovering")
            self.engine_stats["reflexes"]["total_fires"] += 1
    
    def _update_thread_stats(self):
        for ts in self.thread_stats:
            if ts.running:
                ts.loop_count += 1
                ts.actual_hz = ts.target_hz + random.gauss(0, 0.2)
                ts.cpu_percent = max(0, ts.cpu_percent + random.gauss(0, 0.1))
    
    def _record_telemetry(self):
        s = self.state
        record = {
            "timestamp": time.time() - self._start_time,
            "roll": s.roll,
            "pitch": s.pitch,
            "yaw": s.yaw,
            "altitude": s.altitude_agl,
            "battery_voltage": s.battery_voltage,
            "cpu_usage": s.cpu_usage,
            "imu_gyro_x": s.imu["gyro_x"],
            "imu_gyro_y": s.imu["gyro_y"],
            "imu_gyro_z": s.imu["gyro_z"],
            "imu_acc_x": s.imu["acc_x"],
            "imu_acc_y": s.imu["acc_y"],
            "imu_acc_z": s.imu["acc_z"],
            "baro_pressure": s.baro["pressure"],
            "baro_temp": s.baro["temperature"],
            "gps_lat": s.gps["lat"],
            "gps_lon": s.gps["lon"],
            "gps_alt": s.gps["alt"],
            "gps_speed": s.gps["speed"],
            "range_distance": s.rangefinder["distance"],
            "flow_x": s.optical_flow["flow_x"],
            "flow_y": s.optical_flow["flow_y"],
        }
        self.telemetry_history.append(record)
        self.engine_stats["memory"]["telemetry_records"] += 1


    def get_performance(self) -> dict:
        """Return internal engine performance metrics."""
        return {
            "memory": {
                "total_mb": 0.42,
                "engines_bytes": 8192,
                "event_queue_bytes": 4096,
                "camera_bytes": 76800,
            },
            "latency": {
                "reflex_avg_us": 1.2,
                "reflex_fires": self.engine_stats["reflexes"]["total_fires"],
            },
            "throughput": {
                "events_total": self.engine_stats["events"]["total"],
                "events_dropped": self.engine_stats["events"]["dropped"],
                "drop_rate": 0,
            },
            "threads": [
                {"name": t.name, "cpu_percent": t.cpu_percent}
                for t in self.thread_stats
            ],
            "total_cpu_percent": sum(t.cpu_percent for t in self.thread_stats),
        }

    def get_sensor_modes(self) -> dict:
        """Return sensor mode info (all simulated in simulator mode)."""
        return {
            "imu": "simulated",
            "baro": "simulated",
            "gps": "simulated",
            "rangefinder": "simulated",
            "optical_flow": "simulated",
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

    # ── VO Mode Management ──

    _vo_modes = [
        {"id": 0, "name": "Light", "type": "LIGHT",
         "fast_threshold": 30, "lk_window": 5, "lk_iterations": 4, "max_features": 100},
        {"id": 1, "name": "Balanced", "type": "BALANCED",
         "fast_threshold": 25, "lk_window": 7, "lk_iterations": 5, "max_features": 180},
        {"id": 2, "name": "Performance", "type": "PERFORMANCE",
         "fast_threshold": 20, "lk_window": 9, "lk_iterations": 6, "max_features": 250},
    ]

    _platforms = [
        {"id": 0, "name": "Pi Zero 2W", "type": "PI_ZERO_2W",
         "width": 640, "height": 480, "focal_length": 554.0, "target_fps": 15.0},
        {"id": 1, "name": "Pi 4", "type": "PI_4",
         "width": 1280, "height": 720, "focal_length": 830.0, "target_fps": 30.0},
        {"id": 2, "name": "Pi 5", "type": "PI_5",
         "width": 1280, "height": 960, "focal_length": 1108.0, "target_fps": 30.0},
    ]

    def get_vo_profiles(self) -> list:
        return self._vo_modes

    def set_vo_profile(self, mode_id: int) -> bool:
        if 0 <= mode_id < len(self._vo_modes):
            m = self._vo_modes[mode_id]
            with self._lock:
                self.camera_stats.vo_mode = mode_id
                self.camera_stats.vo_mode_name = m["name"]
                self.camera_stats.adaptive_fast_thresh = float(m["fast_threshold"])
                self.camera_stats.adaptive_lk_window = float(m["lk_window"])
            return True
        return False

    def get_platforms(self) -> list:
        return self._platforms

    # ── Multi-Camera Support ──

    def get_cameras(self) -> list:
        """Return info about all camera slots (Variant B)."""
        with self._lock:
            cam = self.camera_stats
            # In simulator: primary is simulated CSI
            primary = {
                "slot": "PRIMARY",
                "camera_type": cam.camera_type,
                "camera_open": cam.camera_open,
                "active": True,
                "frame_count": cam.frame_count,
                "fps_actual": cam.fps_actual,
                "width": cam.width,
                "height": cam.height,
                "label": "Simulated (VO)",
                "device": "simulated",
                "has_vo": True,
                "csi_sensor": None,
            }
            secondary = dict(self._secondary_camera)
            secondary["has_vo"] = False
            return [primary, secondary]

    def get_secondary_camera_stats(self) -> dict:
        """Return secondary camera stats."""
        with self._lock:
            return dict(self._secondary_camera)

    def vo_fallback_tick(self):
        """VO fallback monitoring — no-op in simulator."""
        pass

    def gps_warn_tick(self):
        """GPS-loss position uncertainty warning — no-op in simulator."""
        pass

    def get_features(self) -> list:
        """Return simulated VO features for frontend testing."""
        with self._lock:
            cam = self.camera_stats
            n_detected = cam.vo_features_detected
            n_tracked = cam.vo_features_tracked
            features = []
            t = time.time() - self._start_time
            for i in range(min(n_detected, 120)):
                # Spread features across 320x240 VO resolution
                fx = (hash((i * 7 + 13)) % 300) + 10 + 3 * math.sin(t * 0.2 + i)
                fy = (hash((i * 11 + 7)) % 220) + 10 + 3 * math.cos(t * 0.15 + i)
                features.append({
                    "x": float(fx),
                    "y": float(fy),
                    "tracked": i < n_tracked,
                    "response": 50 + (hash(i * 3) % 150),
                })
            return features

    def capture_secondary(self) -> bool:
        """Trigger capture from secondary camera (continuous stream)."""
        with self._lock:
            if self._usb_capture and self._usb_capture.streaming:
                self._secondary_capturing = True
                self._secondary_camera["active"] = True
                self._secondary_camera["frame_count"] = self._usb_capture.frame_count
                self._secondary_camera["last_capture_time"] = time.time() - self._start_time
                self._secondary_camera["fps_actual"] = 5.0
                return True
            self._secondary_capturing = True
            self._secondary_camera["active"] = True
            self._secondary_camera["frame_count"] += 1
            self._secondary_camera["last_capture_time"] = time.time() - self._start_time
            self._secondary_camera["fps_actual"] = 1.0
            return True

    def get_vo_trail(self) -> list:
        """Simulated VO trail for 3D visualization."""
        trail = []
        t = time.time() - self._start_time
        # Generate a simple circular path
        count = min(int(t / 0.5), 100)
        for i in range(count):
            tt = i * 0.5
            trail.append({
                'x': round(0.05 * math.sin(tt * 0.3), 4),
                'y': round(0.05 * math.cos(tt * 0.3), 4),
                'z': round(0.001 * tt, 4),
                't': round(tt, 1),
            })
        return trail

    def get_secondary_frame_data(self) -> bytes:
        """Return latest frame from USB continuous stream or simulated."""
        if self._usb_capture and self._usb_capture.streaming:
            frame = self._usb_capture.capture_frame()
            if frame:
                return frame
        w = self._secondary_camera.get("width", 256)
        h = self._secondary_camera.get("height", 192)
        data = bytearray(w * h)
        t = time.time() - self._start_time
        cx1 = 128 + 30 * math.sin(t * 0.1)
        cy1 = 96 + 20 * math.cos(t * 0.15)
        for y in range(h):
            dy1_sq = (y - cy1) ** 2
            for x in range(w):
                val = 40
                d1 = math.sqrt((x - cx1)**2 + dy1_sq)
                if d1 < 40:
                    val += int(160 * (1.0 - d1 / 40.0))
                data[y * w + x] = max(0, min(255, val + random.randint(-3, 3)))
        return bytes(data)

    def _update_camera(self, t: float):
        cam = self.camera_stats
        cam.frame_count += 1
        cam.fps_actual = 14.8 + random.gauss(0, 0.3)
        
        # Simulate FAST corner detection + LK tracking
        base_features = 80 + int(30 * math.sin(t * 0.1))
        cam.vo_features_detected = max(10, base_features + random.randint(-10, 10))
        cam.vo_features_tracked = max(5, int(cam.vo_features_detected * (0.7 + random.gauss(0, 0.05))))
        cam.vo_inlier_count = max(3, int(cam.vo_features_tracked * (0.85 + random.gauss(0, 0.03))))
        cam.vo_tracking_quality = min(1.0, max(0.0, cam.vo_features_tracked / max(1, cam.vo_features_detected)))
        cam.vo_confidence = min(1.0, max(0.0, cam.vo_tracking_quality * (cam.vo_inlier_count / max(1, cam.vo_features_tracked)) * 0.95))
        
        # VO motion estimate
        cam.vo_dx = 0.001 * math.sin(t * 0.3) + random.gauss(0, 0.0005)
        cam.vo_dy = 0.0008 * math.cos(t * 0.2) + random.gauss(0, 0.0005)
        cam.vo_dz = random.gauss(0, 0.0001)
        cam.vo_vx = cam.vo_dx * 15.0  # ~15 FPS
        cam.vo_vy = cam.vo_dy * 15.0
        cam.vo_valid = cam.vo_features_tracked >= 5
        
        # Accumulate distance and uncertainty
        cam.vo_total_distance += abs(cam.vo_dx) + abs(cam.vo_dy)
        cam.vo_position_uncertainty = cam.vo_total_distance * 0.03 * (1.0 - cam.vo_confidence * 0.5)
        
        # ── Adaptive Altitude Parameters ──
        alt = self.state.altitude_agl
        if alt < 10.0:
            cam.altitude_zone = 0
            cam.altitude_zone_name = "LOW"
            cam.adaptive_fast_thresh = 30.0
            cam.adaptive_lk_window = 5.0
        elif alt < 50.0:
            cam.altitude_zone = 1
            cam.altitude_zone_name = "MEDIUM"
            frac = (alt - 10.0) / 40.0
            cam.adaptive_fast_thresh = 30.0 - frac * 5.0
            cam.adaptive_lk_window = 5.0 + frac * 2.0
        elif alt < 200.0:
            cam.altitude_zone = 2
            cam.altitude_zone_name = "HIGH"
            frac = (alt - 50.0) / 150.0
            cam.adaptive_fast_thresh = 25.0 - frac * 5.0
            cam.adaptive_lk_window = 7.0 + frac * 2.0
        else:
            cam.altitude_zone = 3
            cam.altitude_zone_name = "CRUISE"
            cam.adaptive_fast_thresh = 20.0
            cam.adaptive_lk_window = 9.0
        
        # ── Hover Yaw Correction ──
        motion = math.sqrt(cam.vo_dx**2 + cam.vo_dy**2) * 1000  # to px-scale
        is_mode_hover = self.state.flight_mode in ("HOVER", "ARMED")
        
        if is_mode_hover and motion < 0.5:
            if not cam.hover_detected:
                cam._hover_counter = getattr(cam, '_hover_counter', 0) + 1
                if cam._hover_counter >= 30:
                    cam.hover_detected = True
            cam.hover_duration += 0.05  # 20Hz * 0.05s = 1s
            # Simulate yaw drift during hover
            cam.yaw_drift_rate = 0.001 * math.sin(t * 0.01) + random.gauss(0, 0.0002)
            cam.corrected_yaw = self.state.yaw * 0.0174533 - cam.yaw_drift_rate * cam.hover_duration
        else:
            cam.hover_detected = False
            cam.hover_duration = 0
            cam.yaw_drift_rate = 0
            cam._hover_counter = 0
            cam.corrected_yaw = self.state.yaw * 0.0174533
        
        # Camera events
        if self._tick % 60 == 15:
            zone_names = ["LOW", "MED", "HIGH", "CRUISE"]
            zone = zone_names[cam.altitude_zone] if cam.altitude_zone < 4 else "?"
            self._emit_event(EventType.CAMERA_VO_UPDATE, 20,
                f"frame={cam.frame_count} feat={cam.vo_features_tracked}/{cam.vo_features_detected} "
                f"q={cam.vo_tracking_quality:.0%} zone={zone}"
                f"{' HOVER' if cam.hover_detected else ''}")
    
    def _update_mavlink(self, t: float):
        mav = self.mavlink_stats
        mav.fc_armed = self.state.armed
        
        # Simulate message sending at ~50Hz tick, but messages at different rates
        mav.messages_sent += 3  # heartbeat + vision + odometry per tick
        mav.messages_received += 2  # heartbeat + attitude from FC
        
        # Vision position: ~30Hz
        mav.vision_pos_sent += 1
        mav.odometry_sent += 1
        
        # Optical flow: every 2nd tick
        if self._tick % 2 == 0:
            mav.optical_flow_sent += 1
        
        # Link quality varies slightly
        mav.link_quality = min(1.0, max(0.0, 0.95 + random.gauss(0, 0.01)))
        
        # MAVLink events
        if self._tick % 100 == 50:
            self._emit_event(EventType.MAVLINK_HEARTBEAT, 30, 
                f"MAV link: sent={mav.messages_sent} recv={mav.messages_received} q={mav.link_quality:.0%}")
