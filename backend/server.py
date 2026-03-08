"""
JT-Zero Ground Station API Server
FastAPI backend with WebSocket telemetry streaming.
Auto-detects native C++ runtime, falls back to Python simulator.
"""

import os
import asyncio
import time
from contextlib import asynccontextmanager
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
from typing import Optional

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
    yield
    runtime.stop()

app = FastAPI(
    title="JT-Zero Ground Station",
    version="1.0.0",
    lifespan=lifespan
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)

# ─── Models ───────────────────────────────────────────────

class CommandRequest(BaseModel):
    command: str
    param1: Optional[float] = 0
    param2: Optional[float] = 0

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

@app.get("/api/state")
async def get_state():
    return runtime.get_state()

@app.get("/api/events")
async def get_events(count: int = 50):
    return runtime.get_events(count)

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

@app.get("/api/mavlink")
async def get_mavlink():
    return runtime.get_mavlink_stats()

@app.get("/api/performance")
async def get_performance():
    if hasattr(runtime, 'get_performance'):
        return runtime.get_performance()
    return {"error": "Performance metrics only available with native runtime"}

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
            state = runtime.get_state()
            threads = runtime.get_thread_stats()
            engines = runtime.get_engine_stats()
            events = runtime.get_events(10)
            camera = runtime.get_camera_stats()
            mavlink = runtime.get_mavlink_stats()
            
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
            }
            
            # Add performance data if native
            if hasattr(runtime, 'get_performance'):
                payload["performance"] = runtime.get_performance()
            
            await ws.send_json(payload)
            await asyncio.sleep(0.1)  # 10 Hz
    except WebSocketDisconnect:
        manager.disconnect(ws)
    except Exception:
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
    except Exception:
        pass
