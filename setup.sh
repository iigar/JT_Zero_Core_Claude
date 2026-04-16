#!/bin/bash
# ============================================================================
# JT-Zero — Автоматичне налаштування Raspberry Pi
# ============================================================================
# Використання:
#   cd ~/jt-zero
#   chmod +x setup.sh
#   ./setup.sh
#
# Підтримувані платформи:
#   - Raspberry Pi Zero 2 W
#   - Raspberry Pi 3B / 3B+
#   - Raspberry Pi 4 / 400
#   - Raspberry Pi 5
#
# Вимоги:
#   - Raspberry Pi OS Lite (64-bit) — свіжа прошивка
#   - Інтернет (для apt install) АБО попередньо встановлені пакети
#   - Скрипт знаходиться в папці jt-zero (~/jt-zero/setup.sh)
# ============================================================================

set -e  # Зупинити при помилці

# ─── Кольори ────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color
BOLD='\033[1m'

# ─── Змінні ─────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JT_DIR="$SCRIPT_DIR"
CPP_DIR="$JT_DIR/jt-zero"
BACKEND_DIR="$JT_DIR/backend"
LOG_FILE="$JT_DIR/setup.log"
NEEDS_REBOOT=false
STEP=0
TOTAL_STEPS=8

# ─── Функції ────────────────────────────────────────────────
step() {
    STEP=$((STEP + 1))
    echo ""
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BOLD}  [$STEP/$TOTAL_STEPS] $1${NC}"
    echo -e "${CYAN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
}

ok()   { echo -e "  ${GREEN}[OK]${NC} $1"; }
warn() { echo -e "  ${YELLOW}[!]${NC} $1"; }
fail() { echo -e "  ${RED}[FAIL]${NC} $1"; exit 1; }
skip() { echo -e "  ${BLUE}[SKIP]${NC} $1"; }
info() { echo -e "  ${NC}$1"; }

# ─── Перевірка середовища ───────────────────────────────────
echo ""
echo -e "${BOLD}${CYAN}"
echo "  ╔══════════════════════════════════════════════════╗"
echo "  ║           JT-Zero Setup Script v1.0              ║"
echo "  ║      Автоматичне налаштування Raspberry Pi       ║"
echo "  ╚══════════════════════════════════════════════════╝"
echo -e "${NC}"

# Перевірка архітектури
ARCH=$(uname -m)
if [[ "$ARCH" != "aarch64" && "$ARCH" != "armv7l" ]]; then
    fail "Цей скрипт призначений для Raspberry Pi (aarch64/armv7l). Поточна архітектура: $ARCH"
fi

# Перевірка що скрипт запущений з правильної папки
if [ ! -d "$CPP_DIR" ]; then
    fail "Папка jt-zero/jt-zero не знайдена. Переконайтесь що скрипт знаходиться в корені проєкту."
fi

if [ ! -d "$BACKEND_DIR" ]; then
    fail "Папка jt-zero/backend не знайдена."
fi

PI_MODEL="Unknown"
if [ -f /proc/device-tree/model ]; then
    PI_MODEL=$(cat /proc/device-tree/model | tr -d '\0')
fi

info "Платформа: ${BOLD}$PI_MODEL${NC}"
info "Архітектура: ${BOLD}$ARCH${NC}"
info "Папка проєкту: ${BOLD}$JT_DIR${NC}"
info "Лог: ${BOLD}$LOG_FILE${NC}"

# Перенаправити детальний вивід у лог
exec > >(tee -a "$LOG_FILE") 2>&1

# ════════════════════════════════════════════════════════════
# КРОК 1: Системні пакети
# ════════════════════════════════════════════════════════════
step "Встановлення системних пакетів"

PACKAGES=(
    cmake
    g++
    make
    python3-dev
    python3-pip
    python3-venv
    pybind11-dev
    libatomic1
    i2c-tools
    v4l-utils
    git
)

# Перевірити які пакети вже встановлені
MISSING=()
for pkg in "${PACKAGES[@]}"; do
    if ! dpkg -l "$pkg" &>/dev/null; then
        MISSING+=("$pkg")
    fi
done

if [ ${#MISSING[@]} -eq 0 ]; then
    ok "Всі пакети вже встановлені"
else
    info "Потрібно встановити: ${MISSING[*]}"
    sudo apt update -qq >> "$LOG_FILE" 2>&1
    sudo apt install -y "${MISSING[@]}" >> "$LOG_FILE" 2>&1
    ok "Встановлено ${#MISSING[@]} пакетів"
fi

# ════════════════════════════════════════════════════════════
# КРОК 2: Налаштування інтерфейсів (UART, I2C, SPI, Camera)
# ════════════════════════════════════════════════════════════
step "Налаштування апаратних інтерфейсів"

CONFIG_FILE=""
if [ -f /boot/firmware/config.txt ]; then
    CONFIG_FILE="/boot/firmware/config.txt"
elif [ -f /boot/config.txt ]; then
    CONFIG_FILE="/boot/config.txt"
else
    warn "config.txt не знайдено — пропуск налаштування інтерфейсів"
fi

if [ -n "$CONFIG_FILE" ]; then
    info "Config: $CONFIG_FILE"
    
    # --- I2C ---
    if ! grep -q "^dtparam=i2c_arm=on" "$CONFIG_FILE" 2>/dev/null; then
        echo "dtparam=i2c_arm=on" | sudo tee -a "$CONFIG_FILE" > /dev/null
        ok "I2C увімкнено"
        NEEDS_REBOOT=true
    else
        skip "I2C вже увімкнено"
    fi
    
    # --- SPI ---
    if ! grep -q "^dtparam=spi=on" "$CONFIG_FILE" 2>/dev/null; then
        echo "dtparam=spi=on" | sudo tee -a "$CONFIG_FILE" > /dev/null
        ok "SPI увімкнено"
        NEEDS_REBOOT=true
    else
        skip "SPI вже увімкнено"
    fi
    
    # --- Camera ---
    if ! grep -q "^camera_auto_detect=1" "$CONFIG_FILE" 2>/dev/null; then
        echo "camera_auto_detect=1" | sudo tee -a "$CONFIG_FILE" > /dev/null
        ok "Camera auto-detect увімкнено"
        NEEDS_REBOOT=true
    else
        skip "Camera вже увімкнено"
    fi
    
    # --- UART (enable hardware, disable console) ---
    if ! grep -q "^enable_uart=1" "$CONFIG_FILE" 2>/dev/null; then
        echo "enable_uart=1" | sudo tee -a "$CONFIG_FILE" > /dev/null
        ok "Hardware UART увімкнено"
        NEEDS_REBOOT=true
    else
        skip "UART вже увімкнено"
    fi
    
    # Вимкнути serial console (звільнити UART для MAVLink)
    if grep -q "console=serial0" /boot/firmware/cmdline.txt 2>/dev/null; then
        sudo sed -i 's/console=serial0,[0-9]* //g' /boot/firmware/cmdline.txt
        ok "Serial console вимкнено (UART вільний для MAVLink)"
        NEEDS_REBOOT=true
    elif grep -q "console=serial0" /boot/cmdline.txt 2>/dev/null; then
        sudo sed -i 's/console=serial0,[0-9]* //g' /boot/cmdline.txt
        ok "Serial console вимкнено (UART вільний для MAVLink)"
        NEEDS_REBOOT=true
    else
        skip "Serial console вже вимкнено"
    fi

    # --- Hardware Watchdog ---
    # Якщо Pi зависне повністю (kernel panic, deadlock), hardware watchdog
    # примусово перезавантажить систему. Критично для безпілотника.
    if ! grep -q "^dtparam=watchdog=on" "$CONFIG_FILE" 2>/dev/null; then
        echo "dtparam=watchdog=on" | sudo tee -a "$CONFIG_FILE" > /dev/null
        ok "Hardware watchdog увімкнено (/dev/watchdog)"
        NEEDS_REBOOT=true
    else
        skip "Hardware watchdog вже увімкнено"
    fi
fi

# Встановити watchdog daemon (пінгує /dev/watchdog щоб Pi не перезавантажувався)
if ! command -v watchdog &> /dev/null; then
    info "Встановлення watchdog daemon..."
    sudo apt-get install -y watchdog >> "$LOG_FILE" 2>&1
    # Мінімальний конфіг: пінг /dev/watchdog кожні 10с, reboot якщо не відповідає 60с
    sudo tee /etc/watchdog.conf > /dev/null << 'WDEOF'
watchdog-device = /dev/watchdog
watchdog-timeout = 15
interval = 5
WDEOF
    sudo systemctl enable watchdog >> "$LOG_FILE" 2>&1
    ok "Watchdog daemon встановлено та увімкнено"
else
    skip "Watchdog daemon вже встановлено"
fi

# ════════════════════════════════════════════════════════════
# КРОК 3: Збірка C++ ядра
# ════════════════════════════════════════════════════════════
step "Збірка C++ ядра (cmake + make)"

# GCC 14 fix: додати #include <cstdlib> якщо потрібно
GCC_VERSION=$(g++ -dumpversion 2>/dev/null | cut -d. -f1)
if [ "$GCC_VERSION" -ge 14 ] 2>/dev/null; then
    if ! grep -q "#include <cstdlib>" "$CPP_DIR/main.cpp" 2>/dev/null; then
        sed -i '1s/^/#include <cstdlib>\n/' "$CPP_DIR/main.cpp"
        ok "Додано #include <cstdlib> для GCC $GCC_VERSION"
    fi
fi

cd "$CPP_DIR"
rm -rf build
mkdir build
cd build

info "cmake ..."
cmake -DCMAKE_BUILD_TYPE=Release .. >> "$LOG_FILE" 2>&1
ok "CMake налаштовано"

# Визначити кількість ядер (обмежити на Pi Zero через RAM)
CORES=$(nproc)
if echo "$PI_MODEL" | grep -qi "zero"; then
    # Pi Zero має мало RAM — 2 потоки максимум
    MAKE_JOBS=2
else
    MAKE_JOBS=$CORES
fi

info "Компіляція (make -j$MAKE_JOBS) ... це займе 5-15 хвилин"
make -j"$MAKE_JOBS" >> "$LOG_FILE" 2>&1

# Перевірити що .so зібрався
SO_FILE=$(find . -name "jtzero_native*.so" -type f | head -1)
if [ -n "$SO_FILE" ]; then
    ok "C++ ядро зібрано: $SO_FILE"
else
    fail "Файл jtzero_native*.so не знайдено. Дивіться $LOG_FILE"
fi

# Копіювати .so в backend
cp jtzero_native*.so "$BACKEND_DIR/"
ok "Скопійовано в backend/"

# ════════════════════════════════════════════════════════════
# КРОК 4: Python venv + залежності
# ════════════════════════════════════════════════════════════
step "Налаштування Python середовища"

cd "$BACKEND_DIR"

if [ ! -d "venv" ]; then
    python3 -m venv venv
    ok "Створено venv"
else
    skip "venv вже існує"
fi

source venv/bin/activate

# Вибрати правильний requirements файл
if [ -f "requirements-pi.txt" ]; then
    REQ_FILE="requirements-pi.txt"
else
    REQ_FILE="requirements.txt"
fi

pip install --upgrade pip >> "$LOG_FILE" 2>&1
pip install -r "$REQ_FILE" >> "$LOG_FILE" 2>&1
ok "Python залежності встановлені ($REQ_FILE)"

# Перевірити C++ модуль
if python3 -c "import jtzero_native; print('OK')" 2>/dev/null | grep -q "OK"; then
    ok "C++ модуль працює в Python"
else
    warn "C++ модуль не завантажився — сервер буде працювати в режимі симуляції"
fi

deactivate

# ════════════════════════════════════════════════════════════
# КРОК 5: Systemd сервіс
# ════════════════════════════════════════════════════════════
step "Створення systemd сервісу (автозапуск)"

SERVICE_FILE="/etc/systemd/system/jtzero.service"

# Визначити домашню директорію та користувача
CURRENT_USER=$(whoami)
USER_HOME=$(eval echo ~$CURRENT_USER)

sudo tee "$SERVICE_FILE" > /dev/null << SERVICEEOF
[Unit]
Description=JT-Zero Runtime
After=network.target
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=notify
User=$CURRENT_USER
WorkingDirectory=$BACKEND_DIR
Environment=PYTHONPATH=$JT_DIR
ExecStart=$BACKEND_DIR/venv/bin/uvicorn server:app --host 0.0.0.0 --port 8001
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

# Watchdog: якщо сервіс не пінгує systemd протягом 30с — він завис → перезапуск
# server.py пінгує через NOTIFY_SOCKET кожні ~10с з _vo_fallback_monitor
WatchdogSec=30
NotifyAccess=main

[Install]
WantedBy=multi-user.target
SERVICEEOF

sudo systemctl daemon-reload
sudo systemctl enable jtzero >> "$LOG_FILE" 2>&1
ok "Сервіс jtzero.service створено та увімкнено"

# ════════════════════════════════════════════════════════════
# КРОК 6: Права доступу до UART та I2C
# ════════════════════════════════════════════════════════════
step "Налаштування прав доступу"

# Додати користувача в групи для доступу до пристроїв
GROUPS_TO_ADD=(dialout i2c spi video gpio)
for grp in "${GROUPS_TO_ADD[@]}"; do
    if getent group "$grp" > /dev/null 2>&1; then
        if ! groups "$CURRENT_USER" | grep -qw "$grp"; then
            sudo usermod -aG "$grp" "$CURRENT_USER"
            ok "Додано в групу: $grp"
        else
            skip "Вже в групі: $grp"
        fi
    fi
done

# ════════════════════════════════════════════════════════════
# КРОК 7: Мережевий доступ до Dashboard
# ════════════════════════════════════════════════════════════
step "Перевірка мережі"

IP_ADDR=$(hostname -I 2>/dev/null | awk '{print $1}')
HOSTNAME=$(hostname)

if [ -n "$IP_ADDR" ]; then
    ok "IP адреса: $IP_ADDR"
    ok "Dashboard буде доступний: http://$IP_ADDR:8001"
    ok "Або: http://$HOSTNAME.local:8001"
else
    warn "IP адреса не визначена (Wi-Fi не підключений?)"
fi

# ════════════════════════════════════════════════════════════
# КРОК 8: Фінальна перевірка та запуск
# ════════════════════════════════════════════════════════════
step "Завершення"

echo ""
echo -e "${GREEN}${BOLD}"
echo "  ╔══════════════════════════════════════════════════╗"
echo "  ║          JT-Zero встановлено успішно!            ║"
echo "  ╚══════════════════════════════════════════════════╝"
echo -e "${NC}"

echo -e "  ${BOLD}Зведення:${NC}"
echo -e "    Платформа:    $PI_MODEL"
echo -e "    C++ ядро:     ${GREEN}зібрано${NC}"
echo -e "    Python venv:  ${GREEN}налаштовано${NC}"
echo -e "    Systemd:      ${GREEN}увімкнено${NC}"
echo -e "    UART:         ${GREEN}увімкнено (MAVLink)${NC}"
echo -e "    I2C/SPI:      ${GREEN}увімкнено (сенсори)${NC}"
echo -e "    Camera:       ${GREEN}auto-detect${NC}"

if [ -n "$IP_ADDR" ]; then
    echo ""
    echo -e "  ${BOLD}Dashboard:${NC}"
    echo -e "    http://$IP_ADDR:8001"
    echo -e "    http://$HOSTNAME.local:8001"
fi

echo ""
echo -e "  ${BOLD}Корисні команди:${NC}"
echo -e "    sudo systemctl status jtzero    — статус сервісу"
echo -e "    sudo systemctl restart jtzero   — перезапуск"
echo -e "    journalctl -u jtzero -f         — логи в реальному часі"
echo -e "    curl -s http://localhost:8001/api/health | python3 -m json.tool"

if $NEEDS_REBOOT; then
    echo ""
    echo -e "  ${YELLOW}${BOLD}ПОТРІБНЕ ПЕРЕЗАВАНТАЖЕННЯ!${NC}"
    echo -e "  ${YELLOW}Змінено налаштування UART/I2C/SPI/Camera.${NC}"
    echo ""
    read -p "  Перезавантажити зараз? (y/n): " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        echo -e "  ${CYAN}Перезавантаження через 3 секунди...${NC}"
        echo -e "  ${CYAN}Після ребуту JT-Zero запуститься автоматично.${NC}"
        sleep 3
        sudo reboot
    else
        echo -e "  ${YELLOW}Не забудьте перезавантажити: sudo reboot${NC}"
        echo -e "  ${YELLOW}Без ребуту UART/I2C можуть не працювати.${NC}"
    fi
else
    # Запустити сервіс зараз
    echo ""
    info "Запуск JT-Zero..."
    sudo systemctl restart jtzero
    sleep 5
    
    if systemctl is-active --quiet jtzero; then
        ok "JT-Zero працює!"
        echo ""
        # Швидкий тест API
        if curl -s --max-time 3 http://localhost:8001/api/health > /dev/null 2>&1; then
            ok "API відповідає на http://localhost:8001"
        else
            warn "API ще стартує — зачекайте 10-15 секунд"
        fi
    else
        warn "Сервіс не запустився. Перевірте: journalctl -u jtzero -n 30"
    fi
fi

echo ""
echo -e "${CYAN}Лог збірки: $LOG_FILE${NC}"
echo ""
