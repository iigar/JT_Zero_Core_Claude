"""
JT-Zero Ground Station API Server
FastAPI backend with WebSocket telemetry streaming
"""

import os
import json
import asyncio
import time
from contextlib import asynccontextmanager
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
from typing import Optional
from simulator import JTZeroSimulator

# Runtime simulator instance
simulator = JTZeroSimulator()

@asynccontextmanager
async def lifespan(app: FastAPI):
    simulator.start()
    yield
    simulator.stop()

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
        "simulator": True,
        "uptime": simulator.get_state().get("uptime_sec", 0)
    }

@app.get("/api/state")
async def get_state():
    return simulator.get_state()

@app.get("/api/events")
async def get_events(count: int = 50):
    return simulator.get_events(count)

@app.get("/api/telemetry")
async def get_telemetry():
    state = simulator.get_state()
    return {
        "state": state,
        "threads": simulator.get_thread_stats(),
        "engines": simulator.get_engine_stats(),
    }

@app.get("/api/telemetry/history")
async def get_telemetry_history(count: int = 100):
    return simulator.get_telemetry_history(count)

@app.get("/api/threads")
async def get_threads():
    return simulator.get_thread_stats()

@app.get("/api/engines")
async def get_engines():
    return simulator.get_engine_stats()

@app.post("/api/command")
async def send_command(req: CommandRequest):
    success = simulator.send_command(req.command, req.param1, req.param2)
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

    async def broadcast(self, data: dict):
        dead = []
        for ws in self.active:
            try:
                await ws.send_json(data)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self.disconnect(ws)

manager = ConnectionManager()

@app.websocket("/api/ws/telemetry")
async def websocket_telemetry(ws: WebSocket):
    await manager.connect(ws)
    try:
        while True:
            state = simulator.get_state()
            threads = simulator.get_thread_stats()
            engines = simulator.get_engine_stats()
            events = simulator.get_events(10)
            
            await ws.send_json({
                "type": "telemetry",
                "timestamp": time.time(),
                "state": state,
                "threads": threads,
                "engines": engines,
                "recent_events": events,
            })
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
            events = simulator.get_events(20)
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
