# Technology Stack

**Analysis Date:** 2026-04-12

## Languages

**Primary:**
- C++17 — Core drone runtime (`jt-zero/` tree): engines, sensors, camera pipeline, MAVLink transport
- Python 3.11 — FastAPI backend (`backend/`), pybind11 bridge, simulator, flight logging
- JavaScript (React 19) — Ground station dashboard (`frontend/src/`)

**Secondary:**
- CMake — Build system for C++ and pybind11 module
- CSS (Tailwind v3) — Frontend styling

## Runtime

**Environment:**
- Target hardware: Raspberry Pi Zero 2W — ARM Cortex-A53 (aarch64), 512 MB RAM
- Python 3.11 (CPython, aarch64 ABI) — confirmed by `.so` artifact name `jtzero_native.cpython-311-aarch64-linux-gnu.so`
- Node.js 20 — frontend build (CI only; not deployed to Pi)

**Package Manager:**
- Python: pip — lockfile is `backend/requirements.txt` (pinned full tree) and `backend/requirements-pi.txt` (minimal Pi subset)
- Frontend: npm — lockfile `frontend/package-lock.json` present

## Frameworks

**Core:**
- FastAPI 0.110.1 — HTTP REST + WebSocket API server (`backend/server.py`)
- Starlette 0.37.2 — ASGI foundation (transitive, used directly for `StaticFiles`, `Response`)
- Uvicorn 0.25.0 — ASGI server, production runtime on Pi

**Testing:**
- pytest 9.0.2 — Python unit/integration tests (`backend/tests/`)
- No C++ test framework detected

**Build/Dev:**
- CMake ≥ 3.16 — C++ build (`jt-zero/CMakeLists.txt`)
- react-scripts 5.0.1 — Frontend dev server and production build (Create React App)
- GitHub Actions — CI for frontend rebuild and static asset commit

## Key Dependencies

**C++ Libraries:**
- pybind11 3.0.2 — C++↔Python bridge; compiled as `jtzero_native` shared module (`jt-zero/api/python_bindings.cpp`)
  - Note: pybind11 wrapper is compiled with `-fexceptions -frtti`; core library uses `-fno-exceptions -fno-rtti` for embedded targets
- ARM NEON intrinsics (`<arm_neon.h>`) — SIMD-accelerated VO hot paths in `jt-zero/camera/neon_accel.h`; scalar fallback for x86 dev
- POSIX / Linux: `pthread`, `V4L2` (`<linux/videodev2.h>`), UART termios, UDP sockets — all gated `#ifdef __linux__`
- libcamera / rpicam-vid — CSI camera capture via subprocess (`rpicam-vid`, `rpicam-hello`); not linked directly

**Python — Critical:**
- pybind11 3.0.2 — also required on Python side to locate the compiled `.so`
- Pillow 12.1.1 — JPEG→grayscale decoding in VO fallback chain; PNG encoding fallback
- cryptography 46.0.5 — AES-256/Fernet flight log encryption, PBKDF2 key derivation (`backend/flight_log.py`)
- psutil 7.2.2 — CPU/RAM metrics (`backend/system_metrics.py`)
- websockets 16.0 — WebSocket support for uvicorn
- python-dotenv 1.2.1 — `.env` loading

**Python — Present but Weight-Sensitive on Pi:**
- numpy 2.4.2 — listed in full `requirements.txt` but **explicitly excluded from Pi path** (see comment in `native_bridge.py`: "numpy removed — too heavy for Pi Zero")
- Pillow + pure-Python fallback chain is used instead on Pi Zero

**Python — Infrastructure (dev/CI only):**
- boto3 1.42.58 — AWS SDK (not used in runtime paths)
- openai 1.99.9, google-genai 1.65.0, litellm 1.80.0 — AI SDKs (not used in runtime paths; likely for tooling)
- motor 3.3.1, pymongo 4.5.0 — MongoDB async client (not used in runtime paths)
- stripe 14.4.0 — payment SDK (not used in runtime paths)

**Frontend:**
- React 19.2.4 + react-dom — UI framework
- @react-three/fiber 9.5.0 + @react-three/drei 10.7.7 + three 0.183.2 — 3D VO trail visualization
- recharts 3.8.0 — telemetry charts
- lucide-react 0.577.0 — icons
- Tailwind CSS 3 + PostCSS — utility-first styling

## Configuration

**Environment:**
- `JT_ZERO_SIMULATE=1` — force Python simulator mode even when on Pi hardware
- `JTZERO_ALLOWED_ORIGINS` — comma-separated CORS origins (default: `localhost:3000`, `localhost:8001`, `jtzero.local:8001`)
- Flight log password stored as SHA-256 hash in `~/jt-zero/config.json`; encryption key derived via PBKDF2

**Build:**
- `jt-zero/CMakeLists.txt` — main build file
- `jt-zero/toolchain-pi-zero.cmake` — cross-compilation toolchain for aarch64 (host: Ubuntu/Debian with `gcc-aarch64-linux-gnu`)
- Compiler flags: `-O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections`; Release adds `-O3 -flto`
- Pi Zero 2W optimization: `-mcpu=cortex-a53 -mtune=cortex-a53 -ffast-math`

## Platform Requirements

**Development (x86/x64 host):**
- CMake ≥ 3.16, GCC/Clang with C++17
- Python 3.11+, pip
- Node.js 20, npm
- `aarch64-linux-gnu-gcc` / `g++` for cross-compilation
- pybind11 (`pip install pybind11`) for building Python module
- On non-Pi: runtime auto-switches to Python simulator (no hardware access)

**Production (Raspberry Pi Zero 2W):**
- Raspberry Pi OS Bookworm or Trixie (64-bit, aarch64)
- libcamera stack (`rpicam-vid`, `rpicam-hello`) for CSI camera
- `v4l2-ctl` (from `v4l-utils`) for USB/thermal camera capture
- Python 3.11, uvicorn serving FastAPI on port 8001
- Prebuilt `jtzero_native.cpython-311-aarch64-linux-gnu.so` installed to `backend/`
- Frontend served as static files from `backend/static/` (built by CI, committed to repo)
- Flight logs written to `~/jt-zero/logs/` (SD card)

---

*Stack analysis: 2026-04-12*
