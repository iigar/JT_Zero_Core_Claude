#!/bin/bash
# ============================================================================
# JT-Zero — Швидке оновлення
# ============================================================================
# Використання:  cd ~/jt-zero && ./update.sh
# ============================================================================

set -e

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
RED='\033[0;31m'
BOLD='\033[1m'
NC='\033[0m'

JT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$JT_DIR/jt-zero"
BACKEND_DIR="$JT_DIR/backend"
FRONTEND_DIR="$JT_DIR/frontend"

# ─── Визначення платформи ───────────────────────────────────
RAM_MB=$(free -m | awk '/Mem:/{print $2}')
CORES=$(nproc)
PI_MODEL="Unknown"
[ -f /proc/device-tree/model ] && PI_MODEL=$(cat /proc/device-tree/model | tr -d '\0')

if [ "$RAM_MB" -lt 600 ]; then
    JOBS=2
elif [ "$RAM_MB" -lt 1500 ]; then
    JOBS=3
else
    JOBS=$CORES
fi

echo -e "${CYAN}${BOLD}JT-Zero Update${NC}"
echo -e "  Pi:    $PI_MODEL"
echo -e "  RAM:   ${RAM_MB}MB, Cores: ${CORES}, Jobs: ${JOBS}"
echo ""

# ─── [1/5] Git pull ──────────────────────────────────────────
cd "$JT_DIR"
if [ -d ".git" ]; then
    echo -e "${CYAN}[1/5]${NC} git pull..."
    git pull || echo -e "${YELLOW}  git pull не вдався, продовжуємо...${NC}"
else
    echo -e "${YELLOW}[1/5]${NC} Немає .git — пропуск"
fi

# ─── [2/5] Збірка C++ ───────────────────────────────────────
echo -e "${CYAN}[2/5]${NC} Збірка C++ (make -j${JOBS})..."
cd "$CPP_DIR"
mkdir -p build && cd build

# Перевірити чи CMake кеш вказує на правильний шлях
NEED_CMAKE=false
if [ ! -f Makefile ]; then
    NEED_CMAKE=true
elif [ -f CMakeCache.txt ]; then
    CACHED_SRC=$(grep "CMAKE_HOME_DIRECTORY" CMakeCache.txt 2>/dev/null | cut -d= -f2)
    if [ "$CACHED_SRC" != "$CPP_DIR" ]; then
        echo -e "  ${YELLOW}CMake кеш застарів ($CACHED_SRC != $CPP_DIR), перебілд...${NC}"
        rm -rf *
        NEED_CMAKE=true
    fi
fi

if [ "$NEED_CMAKE" = true ]; then
    echo -e "  Запуск cmake..."
    cmake -DCMAKE_BUILD_TYPE=Release .. 2>&1 | tail -5
fi

# Якщо cache містить TSan або Debug флаги — примусовий перебілд у Release.
# TSan .so не може бути завантажений Python сервісом (libtsan.so.2 залежність).
if grep -q "fsanitize=thread\|CMAKE_BUILD_TYPE:STRING=RelWithDebInfo\|CMAKE_BUILD_TYPE:STRING=Debug" CMakeCache.txt 2>/dev/null; then
    echo -e "  ${YELLOW}Виявлено TSan/Debug в cmake cache — скидаю до Release...${NC}"
    rm -rf *
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_CXX_FLAGS="" \
          -DCMAKE_EXE_LINKER_FLAGS="" .. 2>&1 | tail -5
fi

make -j"$JOBS" 2>&1 | tail -5

# ─── [3/5] Копіювання .so ───────────────────────────────────
echo -e "${CYAN}[3/5]${NC} Копіювання модуля..."
if ls jtzero_native*.so 1>/dev/null 2>&1; then
    cp jtzero_native*.so "$BACKEND_DIR/"
    echo -e "  ${GREEN}OK${NC}"
else
    echo -e "  ${YELLOW}Модуль не знайдено (pybind11?)${NC}"
fi

# ─── [4/5] Фронтенд ──────────────────────────────────────────
echo -e "${CYAN}[4/5]${NC} Фронтенд..."

# Python deps (Pillow needed for VO Fallback JPEG→grayscale)
# IMPORTANT: Service runs inside venv — must install there, not system Python!
VENV_DIR="$BACKEND_DIR/venv"
VENV_PIP="$VENV_DIR/bin/pip"
VENV_PYTHON="$VENV_DIR/bin/python3"

PILLOW_OK=false
if [ -x "$VENV_PYTHON" ]; then
    "$VENV_PYTHON" -c "from PIL import Image; print('ok')" 2>/dev/null && PILLOW_OK=true
else
    python3 -c "from PIL import Image; print('ok')" 2>/dev/null && PILLOW_OK=true
fi

if [ "$PILLOW_OK" = false ]; then
    echo -e "  ${YELLOW}Встановлення Pillow...${NC}"
    # Method 1: venv pip (PEP 668 does NOT block pip inside venv)
    if [ -x "$VENV_PIP" ]; then
        "$VENV_PIP" install Pillow cryptography 2>&1 | tail -3 && PILLOW_OK=true
    fi
    # Method 2: apt (for non-venv setups)
    if [ "$PILLOW_OK" = false ]; then
        sudo apt-get install -y python3-pil 2>/dev/null && PILLOW_OK=true
    fi
    # Method 3: pip --break-system-packages (last resort)
    if [ "$PILLOW_OK" = false ]; then
        pip3 install --break-system-packages pillow 2>/dev/null && PILLOW_OK=true
    fi
    if [ "$PILLOW_OK" = true ]; then
        echo -e "  ${GREEN}Pillow встановлено${NC}"
    else
        echo -e "  ${RED}Pillow не вдалося встановити — VO Fallback injection буде через djpeg${NC}"
    fi
else
    echo -e "  ${GREEN}Pillow OK (venv)${NC}"
fi

# Стратегія: pre-built (з git) > локальний білд (якщо є npm)
if [ -f "$BACKEND_DIR/static/index.html" ]; then
    # ── Pre-built фронтенд вже є в git (GitHub Actions або ручний білд) ──
    echo -e "  ${GREEN}Pre-built frontend знайдено в backend/static/ (з git)${NC}"
    echo -e "  Node.js/npm не потрібен"
elif [ -d "$FRONTEND_DIR" ] && [ -f "$FRONTEND_DIR/package.json" ]; then
    # ── Fallback: локальний білд ──
    echo -e "  ${YELLOW}Pre-built не знайдено, спроба локального білду...${NC}"
    cd "$FRONTEND_DIR"
    
    if command -v npm &>/dev/null || command -v yarn &>/dev/null; then
        if command -v yarn &>/dev/null; then
            PKG_INSTALL="yarn install --production=false"
            PKG_BUILD="yarn build"
        else
            PKG_INSTALL="npm install --prefer-offline --no-audit --no-fund --no-optional"
            PKG_BUILD="npm run build"
        fi
        
        # Swap для Pi Zero
        SWAP_CREATED=false
        if [ "$RAM_MB" -lt 600 ] && ! swapon --show 2>/dev/null | grep -q "swapfile"; then
            echo -e "  ${YELLOW}Створення swap 1GB...${NC}"
            sudo dd if=/dev/zero of=/swapfile bs=1M count=1024 status=none 2>/dev/null
            sudo chmod 600 /swapfile && sudo mkswap /swapfile >/dev/null 2>&1
            sudo swapon /swapfile 2>/dev/null && SWAP_CREATED=true
        fi
        
        # Install + build
        if [ ! -d "node_modules" ] || [ ! -f "node_modules/.bin/react-scripts" ]; then
            echo -e "  npm install..."
            NODE_OPTIONS="--max-old-space-size=384" $PKG_INSTALL 2>&1 | tail -5
        fi
        
        if [ -f "node_modules/.bin/react-scripts" ]; then
            echo -e "  npm build..."
            export REACT_APP_BACKEND_URL=""
            NODE_OPTIONS="--max-old-space-size=384" $PKG_BUILD 2>&1 | tail -5
            if [ -f "build/index.html" ]; then
                rm -rf "$BACKEND_DIR/static"
                cp -r build "$BACKEND_DIR/static"
                echo -e "  ${GREEN}Frontend збілдено!${NC}"
            else
                echo -e "  ${RED}Білд не вдався${NC}"
            fi
        else
            echo -e "  ${RED}npm install не вдався (OOM? RAM=${RAM_MB}MB)${NC}"
            echo -e "  ${YELLOW}Рішення: пуш змін в git — GitHub Actions збілдить автоматично${NC}"
            echo -e "  ${YELLOW}Потім: git pull — і frontend з'явиться в backend/static/${NC}"
        fi
        
        # Cleanup swap
        if [ "$SWAP_CREATED" = true ]; then
            sudo swapoff /swapfile 2>/dev/null && sudo rm -f /swapfile
        fi
    else
        echo -e "  ${RED}Node.js не знайдено і pre-built відсутній${NC}"
        echo -e "  ${YELLOW}Рішення: пуш змін в git — GitHub Actions збілдить автоматично${NC}"
    fi
else
    echo -e "  ${YELLOW}Фронтенд не знайдено${NC}"
fi

# ─── [5/5] Перезапуск сервісу ────────────────────────────────
echo -e "${CYAN}[5/5]${NC} Перезапуск сервісу..."
sudo systemctl restart jtzero

echo ""
echo -e "  Очікування запуску (15с)..."
sleep 15

if curl -s --max-time 5 http://localhost:8001/api/health > /dev/null 2>&1; then
    curl -s http://localhost:8001/api/mavlink | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f\"  State:  {d['state']}\")
print(f\"  Baud:   {d.get('transport_info','?')}\")
print(f\"  HB:     {d.get('heartbeats_received',0)}\")
print(f\"  Msgs:   TX={d['messages_sent']} RX={d['messages_received']}\")
print(f\"  FC:     {d['fc_type']} {d['fc_firmware']}\")
print(f\"  CRC err:{d.get('crc_errors',0)}\")
" 2>/dev/null || echo -e "  ${YELLOW}MAVLink API недоступний${NC}"
    
    # Перевірити камери
    curl -s http://localhost:8001/api/cameras | python3 -c "
import sys,json
try:
    cams=json.load(sys.stdin)
    print(f'  Cameras: {len(cams)}')
    for c in cams:
        status = 'ACTIVE' if c.get('active') else 'OFF'
        print(f\"    {c['slot']}: {c.get('label','?')} [{status}]\")
except: pass
" 2>/dev/null

    # Перевірити VO Fallback
    curl -s http://localhost:8001/api/camera | python3 -c "
import sys,json
try:
    d=json.load(sys.stdin)
    src = d.get('vo_source', 'CSI_PRIMARY')
    conf = d.get('vo_confidence', 0)
    print(f'  VO Source: {src} (conf={conf:.0%})')
    if src == 'THERMAL_FALLBACK':
        print(f'    Reason: {d.get(\"vo_fallback_reason\",\"\")}')
        print(f'    Duration: {d.get(\"vo_fallback_duration\",0):.1f}s')
except: pass
" 2>/dev/null

    echo ""
    echo -e "  ${GREEN}${BOLD}Оновлення завершено!${NC}"
else
    echo -e "  ${YELLOW}Сервіс ще стартує. Перевірте: sudo systemctl status jtzero${NC}"
fi
