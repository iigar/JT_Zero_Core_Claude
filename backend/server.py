"""
JT-Zero Ground Station API Server
FastAPI backend with WebSocket telemetry streaming.
Auto-detects native C++ runtime, falls back to Python simulator.
"""

import os
import sys
import asyncio
import time
import struct
import zlib
import socket
from pathlib import Path
from contextlib import asynccontextmanager
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.staticfiles import StaticFiles
from fastapi.responses import FileResponse, Response
from pydantic import BaseModel
from typing import Optional
from system_metrics import get_system_metrics
from diagnostics import run_diagnostics, get_cached_diagnostics
from flight_log import FlightLogger, verify_password, set_password, is_password_set, hash_password, _derive_key

# ─── Runtime Selection ────────────────────────────────────
# Try native C++ runtime first, fall back to Python simulator

RUNTIME_MODE = "simulator"  # "native" or "simulator"

try:
    from native_bridge import NativeRuntime, NATIVE_AVAILABLE, BUILD_INFO
    if NATIVE_AVAILABLE:
        runtime = NativeRuntime()
        RUNTIME_MODE = "native"
        print(f"[JT-Zero API] Using NATIVE C++ runtime (GCC {BUILD_INFO.get('compiler', '?')})")
    else:
        raise ImportError("Native module not available")
except Exception as e:
    print(f"[JT-Zero API] Native runtime not available ({e}), using Python simulator")
    from simulator import JTZeroSimulator
    runtime = JTZeroSimulator()
    RUNTIME_MODE = "simulator"
    BUILD_INFO = None

@asynccontextmanager
async def lifespan(app: FastAPI):
    runtime.start()
    # Run initial hardware diagnostics scan
    try:
        mavlink = runtime.get_mavlink_stats()
        diag = run_diagnostics(mavlink_stats=mavlink)
        print(f"[JT-Zero API] Hardware scan: camera={diag['summary']['camera']}, "
              f"i2c={diag['summary']['i2c_devices']} devices, "
              f"mavlink={'OK' if diag['summary']['mavlink_connected'] else 'N/A'} "
              f"({diag['scan_duration_ms']}ms)")
    except Exception as e:
        print(f"[JT-Zero API] Hardware scan failed: {e}")
    
    # Start VO Fallback background monitor (independent of WebSocket connections)
    vo_fallback_task = asyncio.create_task(_vo_fallback_monitor())
    yield
    vo_fallback_task.cancel()
    runtime.stop()

# ─── Flight Logger (global) ───────────────────────────────
flight_logger = FlightLogger()


def _sd_watchdog_ping():
    """Ping systemd watchdog via NOTIFY_SOCKET.

    systemd sets WatchdogSec=30 in jtzero.service.
    If this ping stops arriving, systemd assumes the process is hung and restarts it.
    No external dependencies — uses only stdlib socket.
    Safe to call even if not running under systemd (NOTIFY_SOCKET will be absent).
    """
    notify_socket = os.environ.get("NOTIFY_SOCKET")
    if not notify_socket:
        return
    try:
        addr = notify_socket
        if addr.startswith("@"):
            addr = "\0" + addr[1:]  # abstract unix socket
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
            sock.sendto(b"WATCHDOG=1", addr)
    except Exception:
        pass  # not running under systemd — silently ignore


async def _vo_fallback_monitor():
    """Background task: monitors CSI confidence and manages VO fallback to thermal.
    Runs independently of WebSocket connections at ~10Hz.
    Also records flight log telemetry and point cloud.
    Also monitors GPS-loss position uncertainty and sends STATUSTEXT warnings."""
    await asyncio.sleep(5)  # Let runtime stabilize
    tick_count = 0
    while True:
        try:
            if hasattr(runtime, 'vo_fallback_tick'):
                runtime.vo_fallback_tick()

            # GPS-loss warning (1Hz — no need to check every 100ms)
            if tick_count % 10 == 0 and hasattr(runtime, 'gps_warn_tick'):
                runtime.gps_warn_tick()

            # Systemd watchdog ping (every 10s — WatchdogSec=30 in jtzero.service)
            # If this loop hangs, ping stops → systemd restarts the service
            if tick_count % 100 == 0:
                _sd_watchdog_ping()

            # Flight log recording (if active)
            if flight_logger.is_recording:
                flight_logger.record_telemetry(runtime)
                flight_logger.record_pointcloud(runtime)

            tick_count += 1
            if tick_count % 50 == 0:  # Every 5 seconds
                try:
                    cam = runtime.get_camera_stats()
                    conf = cam.get('vo_confidence', -1)
                    src = cam.get('vo_source', '?')
                    bright = cam.get('frame_brightness', -1)
                    # Show rolling average if available
                    pose_x = getattr(runtime, '_vo_pos_x', 0.0)
                    pose_y = getattr(runtime, '_vo_pos_y', 0.0)
                    if hasattr(runtime, '_vo_bright_history') and runtime._vo_bright_history:
                        avg_b = sum(runtime._vo_bright_history) / len(runtime._vo_bright_history)
                        n = len(runtime._vo_bright_history)
                        print(f"[VO Monitor] bright={bright:.0f} avg_b={avg_b:.0f}({n}) conf={conf:.2f} src={src} pose_x={pose_x:.2f}m pose_y={pose_y:.2f}m t={tick_count}", flush=True)
                    else:
                        print(f"[VO Monitor] bright={bright:.0f} conf={conf:.2f} src={src} pose_x={pose_x:.2f}m pose_y={pose_y:.2f}m t={tick_count}", flush=True)
                except Exception as e2:
                    print(f"[VO Monitor] stats error: {e2}", flush=True)
        except Exception as e:
            print(f"[VO Monitor] tick error: {e}", flush=True)
            # Notify FC that Python monitor is degraded — visible in Mission Planner
            try:
                if hasattr(runtime, '_send_statustext'):
                    runtime._send_statustext(3, "JT0: MONITOR ERR — VO/GPS WATCH DEGRADED")
            except Exception:
                pass
        await asyncio.sleep(0.1)  # 10Hz

app = FastAPI(
    title="JT-Zero Ground Station",
    version="1.0.0",
    lifespan=lifespan
)

# Issue 1 fix: restrict CORS to known origins instead of wildcard.
# Set JTZERO_ALLOWED_ORIGINS env var to override (comma-separated).
_ALLOWED_ORIGINS = os.environ.get(
    "JTZERO_ALLOWED_ORIGINS",
    "http://localhost:3000,http://localhost:8001,http://jtzero.local:8001"
).split(",")

app.add_middleware(
    CORSMiddleware,
    allow_origins=_ALLOWED_ORIGINS,
    allow_credentials=False,
    allow_methods=["GET", "POST"],
    allow_headers=["Content-Type"],
)

# ─── Models ───────────────────────────────────────────────

class CommandRequest(BaseModel):
    command: str
    param1: Optional[float] = 0
    param2: Optional[float] = 0

class LogPasswordRequest(BaseModel):
    password: str

class LogSessionRequest(BaseModel):
    password: str
    filename: str

class CommandResponse(BaseModel):
    success: bool
    message: str
    command: str

# ─── REST Endpoints ───────────────────────────────────────

@app.get("/api/health")
async def health():
    return {
        "status": "ok",
        "runtime": "jt-zero",
        "version": "1.0.0",
        "mode": RUNTIME_MODE,
        "native": RUNTIME_MODE == "native",
        "build_info": BUILD_INFO,
        "uptime": runtime.get_state().get("uptime_sec", 0)
    }

@app.get("/api/hardware")
async def get_hardware():
    """Hardware detection status for all sensors."""
    if hasattr(runtime, 'get_hardware_info'):
        return runtime.get_hardware_info()
    # Default: all simulated (no real hardware in this environment)
    return {
        "i2c_available": False,
        "spi_available": False,
        "uart_available": False,
        "sensors": {
            "imu": {"detected": False, "model": "none", "mode": "simulation", "bus": "I2C", "address": "0x68"},
            "baro": {"detected": False, "model": "none", "mode": "simulation", "bus": "I2C", "address": "0x76"},
            "gps": {"detected": False, "model": "none", "mode": "simulation", "bus": "UART", "address": "9600"},
            "rangefinder": {"detected": False, "model": "none", "mode": "simulation", "bus": "I2C/UART", "address": "-"},
            "optical_flow": {"detected": False, "model": "none", "mode": "simulation", "bus": "SPI", "address": "CS0"},
        },
        "auto_detect_ran": True,
        "note": "No hardware detected — all sensors using simulation"
    }

@app.get("/api/state")
async def get_state():
    return runtime.get_state()

@app.get("/api/diagnostics")
async def get_diagnostics():
    """Return cached hardware diagnostics with live MAVLink status."""
    diag = get_cached_diagnostics()
    # Always refresh MAVLink status (it changes after startup)
    mavlink = runtime.get_mavlink_stats()
    connected = mavlink.get("state") == "CONNECTED"
    diag["mavlink"]["connected"] = connected
    diag["mavlink"]["fc_type"] = mavlink.get("fc_type", "N/A")
    diag["mavlink"]["fc_firmware"] = mavlink.get("fc_firmware", "N/A")
    diag["summary"]["mavlink_connected"] = connected
    # Recalculate overall based on live MAVLink status
    cam_ok = diag["summary"].get("camera", "NONE") != "NONE"
    diag["summary"]["overall"] = "ok" if cam_ok and connected else "partial"
    return diag

@app.post("/api/diagnostics/scan")
async def scan_diagnostics():
    """Run a fresh hardware diagnostics scan."""
    mavlink = runtime.get_mavlink_stats()
    return run_diagnostics(mavlink_stats=mavlink)

@app.get("/api/sensors")
async def api_get_sensor_modes():
    """Return sensor modes (hardware vs simulated) and detection info."""
    result = runtime.get_sensor_modes()
    return result

@app.get("/api/events")
async def get_events(count: int = 50):
    raw = runtime.get_events(200)
    return _filter_events(raw, count)


def _filter_events(events: list, max_count: int = 50) -> list:
    """Filter & deduplicate events:
    - Remove noise (empty messages, IMU_UPDATE, SYS_HEARTBEAT)
    - Group same type+message, show latest timestamp with (xN)
    """
    if not events:
        return events

    # Filter noise
    skip_types = {"IMU_UPDATE", "SYS_HEARTBEAT"}
    filtered = [ev for ev in events if ev.get("message") and ev.get("type") not in skip_types]

    # Group by (type, message) — preserve order of first occurrence
    from collections import OrderedDict
    groups = OrderedDict()
    for ev in filtered:
        key = (ev.get("type", ""), ev.get("message", ""))
        if key not in groups:
            groups[key] = {"event": ev, "count": 1}
        else:
            groups[key]["count"] += 1
            groups[key]["event"] = ev  # keep latest timestamp

    deduped = []
    for (etype, msg), g in groups.items():
        entry = {
            "timestamp": g["event"].get("timestamp", 0),
            "priority": g["event"].get("priority", 0),
            "type": etype,
            "message": msg + (f" (x{g['count']})" if g["count"] > 1 else ""),
        }
        deduped.append(entry)

    # Sort by timestamp desc, take latest max_count
    deduped.sort(key=lambda e: e["timestamp"])
    return deduped[-max_count:]

@app.get("/api/telemetry")
async def get_telemetry():
    state = runtime.get_state()
    return {
        "state": state,
        "threads": runtime.get_thread_stats(),
        "engines": runtime.get_engine_stats(),
    }

@app.get("/api/telemetry/history")
async def get_telemetry_history(count: int = 100):
    return runtime.get_telemetry_history(count)

@app.get("/api/threads")
async def get_threads():
    return runtime.get_thread_stats()

@app.get("/api/engines")
async def get_engines():
    return runtime.get_engine_stats()

@app.get("/api/camera")
async def get_camera():
    return runtime.get_camera_stats()

@app.get("/api/cameras")
async def get_cameras():
    """List all camera slots (primary + secondary)."""
    if hasattr(runtime, 'get_cameras'):
        return runtime.get_cameras()
    # Fallback: only primary camera
    cam = runtime.get_camera_stats()
    return [{
        "slot": "PRIMARY",
        "camera_type": cam.get("camera_type", "SIMULATED"),
        "camera_open": cam.get("camera_open", False),
        "active": True,
        "frame_count": cam.get("frame_count", 0),
        "fps_actual": cam.get("fps_actual", 0),
        "width": cam.get("width", 320),
        "height": cam.get("height", 240),
        "label": "Forward (VO)",
        "has_vo": True,
    }]

@app.get("/api/camera/secondary/stats")
async def get_secondary_camera_stats():
    """Get secondary (thermal) camera stats."""
    if hasattr(runtime, 'get_secondary_camera_stats'):
        return runtime.get_secondary_camera_stats()
    return {"error": "Secondary camera not available"}

@app.post("/api/camera/secondary/capture")
async def capture_secondary():
    """Trigger on-demand capture from secondary (thermal) camera."""
    if hasattr(runtime, 'capture_secondary'):
        ok = runtime.capture_secondary()
        return {"success": ok}
    return {"success": False, "error": "Secondary camera not available"}

@app.get("/api/camera/features")
async def get_camera_features():
    """Get current VO feature positions."""
    try:
        if hasattr(runtime, 'get_features'):
            return runtime.get_features()
    except Exception as e:
        print(f"[API] /camera/features error: {e}", flush=True)
    return []

@app.get("/api/vo/profiles")
async def get_vo_profiles():
    """Get available hardware profiles for Visual Odometry."""
    if hasattr(runtime, 'get_vo_profiles'):
        return runtime.get_vo_profiles()
    return []

@app.post("/api/vo/profile/{profile_id}")
async def set_vo_profile(profile_id: int):
    """Set active VO hardware profile."""
    if hasattr(runtime, 'set_vo_profile'):
        ok = runtime.set_vo_profile(profile_id)
        return {"success": ok, "profile_id": profile_id}
    return {"success": False, "error": "VO profiles not available"}

@app.get("/api/mavlink")
async def get_mavlink():
    return runtime.get_mavlink_stats()

@app.get("/api/vo/trail")
async def get_vo_trail():
    """Get VO position trail for 3D visualization."""
    if hasattr(runtime, 'get_vo_trail'):
        return runtime.get_vo_trail()
    return []

# ─── Flight Log API ──────────────────────────────────────────

@app.get("/api/logs/status")
async def log_status():
    """Flight log status: recording, password set, sessions list."""
    return {
        "recording": flight_logger.is_recording,
        "write_failed": flight_logger.write_failed,
        "record_count": flight_logger.record_count,
        "session_file": flight_logger.session_file,
        "password_set": is_password_set(),
    }

@app.post("/api/logs/password")
async def set_log_password(req: LogPasswordRequest):
    """Set or update the flight log password."""
    if len(req.password) < 12:
        return {"success": False, "error": "Password must be at least 12 characters"}
    set_password(req.password)
    return {"success": True, "message": "Password set"}

@app.post("/api/logs/start")
async def start_log(req: LogPasswordRequest):
    """Start recording a flight log session."""
    if not verify_password(req.password):
        return {"success": False, "error": "Invalid password"}
    # Issue 5 fix: derive key once and store only the key, not the plaintext password
    flight_logger._key = _derive_key(req.password)
    flight_logger._write_failed = False
    flight_logger.start_session()
    return {"success": True, "file": flight_logger.session_file}

@app.post("/api/logs/stop")
async def stop_log():
    """Stop recording current flight log session."""
    flight_logger.stop_session()
    return {"success": True, "records": flight_logger.record_count}

@app.post("/api/logs/sessions")
async def list_sessions(req: LogPasswordRequest):
    """List all flight log sessions (requires password)."""
    if not verify_password(req.password):
        return {"success": False, "error": "Invalid password"}
    return {"success": True, "sessions": FlightLogger.list_sessions()}

@app.post("/api/logs/read")
async def read_session(req: LogSessionRequest):
    """Read and decrypt a specific flight log session."""
    if not verify_password(req.password):
        return {"success": False, "error": "Invalid password"}
    records = FlightLogger.read_session(req.filename, req.password)
    # Issue 10 fix: distinguish wrong password (None) from corrupted/empty file ([])
    if records is None:
        return {"success": False, "error": "Wrong password"}
    if not records:
        return {"success": False, "error": "Corrupted or empty file"}
    return {"success": True, "records": records, "count": len(records)}

# ─── Camera Frame Endpoint ──────────────────────────────────
# Returns latest camera frame as PNG (pure Python, no Pillow needed)

def _grayscale_to_png(data: bytes, width: int, height: int) -> bytes:
    """Encode raw grayscale bytes to PNG using only stdlib."""
    raw = b''.join(b'\x00' + data[y*width:(y+1)*width] for y in range(height))
    compressed = zlib.compress(raw, 1)  # Fast compression
    
    def chunk(tag, d):
        c = tag + d
        crc = zlib.crc32(c) & 0xffffffff
        return struct.pack('>I', len(d)) + c + struct.pack('>I', crc)
    
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0)
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', ihdr) +
            chunk(b'IDAT', compressed) +
            chunk(b'IEND', b''))

_frame_cache = {"png": b'', "frame_id": -1}

@app.get("/api/camera/frame")
async def get_camera_frame():
    """Return latest primary camera frame as PNG image."""
    if not hasattr(runtime, 'get_frame_data'):
        return Response(content=b'', media_type="image/png", status_code=204)
    
    frame_data = runtime.get_frame_data()
    if not frame_data or len(frame_data) == 0:
        return Response(content=b'', media_type="image/png", status_code=204)
    
    # Cache: only re-encode if frame changed
    cam = runtime.get_camera_stats()
    fid = cam.get("frame_count", 0)
    if fid != _frame_cache["frame_id"]:
        w = cam.get("width", 320) or 320
        h = cam.get("height", 240) or 240
        expected = w * h
        if len(frame_data) >= expected:
            _frame_cache["png"] = _grayscale_to_png(frame_data[:expected], w, h)
            _frame_cache["frame_id"] = fid
    
    return Response(
        content=_frame_cache["png"],
        media_type="image/png",
        headers={"Cache-Control": "no-cache", "X-Frame-Id": str(fid)}
    )

# Secondary (thermal) camera frame cache
_secondary_frame_cache = {"data": b'', "frame_id": -1, "media_type": "image/png"}

@app.get("/api/camera/secondary/frame")
async def get_secondary_camera_frame():
    """Return latest secondary (thermal) camera frame as JPEG or PNG."""
    if not hasattr(runtime, 'get_secondary_frame_data'):
        return Response(content=b'', media_type="image/png", status_code=204)
    
    frame_data = runtime.get_secondary_frame_data()
    if not frame_data or len(frame_data) == 0:
        return Response(content=b'', media_type="image/png", status_code=204)
    
    sec = runtime.get_secondary_camera_stats() if hasattr(runtime, 'get_secondary_camera_stats') else {}
    fid = sec.get("frame_count", 0)
    frame_fmt = sec.get("frame_format", "gray")
    
    if fid != _secondary_frame_cache["frame_id"]:
        if frame_fmt == "jpeg" and len(frame_data) > 100 and frame_data[:2] == b'\xff\xd8':
            _secondary_frame_cache["data"] = frame_data
            _secondary_frame_cache["media_type"] = "image/jpeg"
        else:
            w = sec.get("width", 256) or 256
            h = sec.get("height", 192) or 192
            expected = w * h
            if len(frame_data) >= expected:
                _secondary_frame_cache["data"] = _grayscale_to_png(frame_data[:expected], w, h)
                _secondary_frame_cache["media_type"] = "image/png"
        _secondary_frame_cache["frame_id"] = fid
    
    return Response(
        content=_secondary_frame_cache["data"],
        media_type=_secondary_frame_cache["media_type"],
        headers={"Cache-Control": "no-cache", "X-Frame-Id": str(fid)}
    )

@app.get("/api/performance")
async def get_performance():
    result = {}
    if hasattr(runtime, 'get_performance'):
        result["engine"] = runtime.get_performance()
    result["system"] = get_system_metrics()
    return result

@app.get("/api/simulator/config")
async def get_sim_config():
    if hasattr(runtime, 'get_sim_config'):
        return runtime.get_sim_config()
    return {"error": "Simulator config only available with native runtime"}

class SimConfigUpdate(BaseModel):
    wind_speed: Optional[float] = None
    wind_direction: Optional[float] = None
    sensor_noise: Optional[float] = None
    battery_drain: Optional[float] = None
    gravity: Optional[float] = None
    mass_kg: Optional[float] = None
    drag_coeff: Optional[float] = None
    turbulence: Optional[bool] = None

@app.post("/api/simulator/config")
async def set_sim_config(req: SimConfigUpdate):
    if hasattr(runtime, 'set_sim_config'):
        config = {k: v for k, v in req.dict().items() if v is not None}
        runtime.set_sim_config(config)
        return {"success": True, "config": runtime.get_sim_config()}
    return {"error": "Simulator config only available with native runtime"}

@app.post("/api/command")
async def send_command(req: CommandRequest):
    success = runtime.send_command(req.command, req.param1, req.param2)
    return CommandResponse(
        success=success,
        message=f"Command '{req.command}' {'executed' if success else 'failed'}",
        command=req.command
    )

# ─── WebSocket Telemetry Stream ───────────────────────────

class ConnectionManager:
    def __init__(self):
        self.active: list[WebSocket] = []

    async def connect(self, ws: WebSocket):
        await ws.accept()
        self.active.append(ws)

    def disconnect(self, ws: WebSocket):
        if ws in self.active:
            self.active.remove(ws)

manager = ConnectionManager()

@app.websocket("/api/ws/telemetry")
async def websocket_telemetry(ws: WebSocket):
    await manager.connect(ws)
    try:
        while True:
            # Build a consistent data snapshot (all reads happen together)
            state = runtime.get_state()
            threads = runtime.get_thread_stats()
            engines = runtime.get_engine_stats()
            events = _filter_events(runtime.get_events(60), 10)
            camera = runtime.get_camera_stats()
            mavlink = runtime.get_mavlink_stats()
            features = runtime.get_features() if hasattr(runtime, 'get_features') else []
            sensor_modes = runtime.get_sensor_modes()
            perf = runtime.get_performance() if hasattr(runtime, 'get_performance') else None
            sys_metrics = get_system_metrics()
            cameras = runtime.get_cameras() if hasattr(runtime, 'get_cameras') else []
            
            payload = {
                "type": "telemetry",
                "timestamp": time.time(),
                "runtime_mode": RUNTIME_MODE,
                "state": state,
                "threads": threads,
                "engines": engines,
                "recent_events": events,
                "camera": camera,
                "mavlink": mavlink,
                "features": features,
                "sensor_modes": sensor_modes,
                "system_metrics": sys_metrics,
                "cameras": cameras,
            }
            
            if perf:
                payload["performance"] = perf
            
            await ws.send_json(payload)
            await asyncio.sleep(0.1)  # 10 Hz
    except WebSocketDisconnect:
        manager.disconnect(ws)
    except Exception as e:
        # Issue 8 fix: log unexpected WebSocket errors instead of silently swallowing
        sys.stderr.write(f"[WS/telemetry] Unexpected error: {e}\n")
        sys.stderr.flush()
        manager.disconnect(ws)

@app.websocket("/api/ws/events")
async def websocket_events(ws: WebSocket):
    await ws.accept()
    last_count = 0
    try:
        while True:
            events = runtime.get_events(20)
            current_count = len(events)
            if current_count != last_count:
                await ws.send_json({
                    "type": "events",
                    "events": events
                })
                last_count = current_count
            await asyncio.sleep(0.2)
    except WebSocketDisconnect:
        pass
    except Exception as e:
        sys.stderr.write(f"[WS/events] Unexpected error: {e}\n")
        sys.stderr.flush()



# ─── Static Files (Dashboard) ────────────────────────────
# Serve built React frontend from /static directory
# This is used when running standalone on Pi (no separate frontend server)

STATIC_DIR = Path(__file__).parent / "static"

if STATIC_DIR.exists() and (STATIC_DIR / "index.html").exists():
    # Serve static assets (JS, CSS, images)
    app.mount("/static", StaticFiles(directory=str(STATIC_DIR / "static")), name="static-assets")

    # Catch-all: serve index.html for any non-API route (React SPA routing)
    @app.get("/{path:path}")
    async def serve_frontend(path: str):
        # Try to serve exact file first (favicon.ico, manifest.json, etc.)
        file_path = STATIC_DIR / path
        if file_path.is_file():
            return FileResponse(str(file_path))
        # Otherwise serve index.html (React handles routing)
        return FileResponse(str(STATIC_DIR / "index.html"))

    print(f"[JT-Zero API] Serving Dashboard from {STATIC_DIR}")
else:
    print(f"[JT-Zero API] No Dashboard found at {STATIC_DIR} — API-only mode")
