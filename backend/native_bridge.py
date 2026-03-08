"""
JT-Zero Native Bridge
Wraps the compiled C++ runtime (jtzero_native) with the same interface
as the Python simulator, enabling seamless switching.
"""

import time
import sys
import os

# Try to import native module
try:
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import jtzero_native as _native
    NATIVE_AVAILABLE = True
    BUILD_INFO = dict(_native.get_build_info())
except ImportError:
    NATIVE_AVAILABLE = False
    BUILD_INFO = None


class NativeRuntime:
    """Adapter wrapping C++ Runtime with simulator-compatible interface."""
    
    def __init__(self):
        if not NATIVE_AVAILABLE:
            raise RuntimeError("jtzero_native module not found")
        
        self._rt = _native.Runtime()
        self._rt.set_simulator_mode(True)
        self._start_time = time.time()
        self.running = False
    
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
        return self._rt.send_command(cmd, param1, param2)
    
    def get_state(self) -> dict:
        return dict(self._rt.get_state())
    
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
        return dict(self._rt.get_camera())
    
    def get_mavlink_stats(self) -> dict:
        return dict(self._rt.get_mavlink())
    
    def get_performance(self) -> dict:
        return dict(self._rt.get_performance())
