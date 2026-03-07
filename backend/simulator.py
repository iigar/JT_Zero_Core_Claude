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
            ThreadStats("T5_MAVLink", 50, 49.8, 1.2, 0, 5, False),
            ThreadStats("T6_Camera", 15, 14.9, 12.5, 0, 15000, False),
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
                self._update_thread_stats()
                self._record_telemetry()
                
                # Periodic events
                if self._tick % 20 == 0:
                    self._emit_event(EventType.SYSTEM_HEARTBEAT, 50, "heartbeat")
                    self.engine_stats["rules"]["total_evaluations"] += 1
                
                # Sensor update events (every 2 seconds)
                if self._tick % 40 == 10:
                    self._emit_event(EventType.SENSOR_BARO_UPDATE, 10, 
                        f"alt={s.baro['altitude']:.1f}m press={s.baro['pressure']:.1f}hPa")
                if self._tick % 40 == 20:
                    self._emit_event(EventType.SENSOR_GPS_UPDATE, 10, 
                        f"lat={s.gps['lat']:.6f} lon={s.gps['lon']:.6f} sat={s.gps['satellites']}")
                if self._tick % 60 == 30:
                    self._emit_event(EventType.SENSOR_RANGE_UPDATE, 10, 
                        f"dist={s.rangefinder['distance']:.2f}m q={s.rangefinder['signal_quality']:.0%}")
                if self._tick % 60 == 45:
                    self._emit_event(EventType.SENSOR_FLOW_UPDATE, 10,
                        f"flow=({s.optical_flow['flow_x']:.3f},{s.optical_flow['flow_y']:.3f}) q={s.optical_flow['quality']}")
                
                # Occasional warnings
                if self._tick % 200 == 100 and s.battery_percent < 50:
                    self._emit_event(EventType.SYSTEM_WARNING, 100, 
                        f"Battery at {s.battery_percent:.0f}%")
                
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
        
        # Attitude
        s.roll = math.atan2(s.imu["acc_y"], s.imu["acc_z"]) * 57.2958
        s.pitch = math.atan2(-s.imu["acc_x"],
                  math.sqrt(s.imu["acc_y"]**2 + s.imu["acc_z"]**2)) * 57.2958
        s.yaw = (s.yaw + s.imu["gyro_z"] * 0.05 * 57.2958) % 360
        
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
