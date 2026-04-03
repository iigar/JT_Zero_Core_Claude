# JT-Zero — Довідник команд

Всі команди для збірки, запуску, діагностики та взаємодії з системою JT-Zero.

---

## 1. Керування сервісом на Pi

Після встановлення JT-Zero працює як системний сервіс (systemd). Ці команди виконуються на Pi через SSH.

| Дія | Команда |
|-----|---------|
| Статус | `sudo systemctl status jtzero` |
| Запустити | `sudo systemctl start jtzero` |
| Зупинити | `sudo systemctl stop jtzero` |
| Перезапустити | `sudo systemctl restart jtzero` |
| Увімкнути автозапуск | `sudo systemctl enable jtzero` |
| Вимкнути автозапуск | `sudo systemctl disable jtzero` |

### Логи

```bash
# Логи в реальному часі (Ctrl+C для виходу)
journalctl -u jtzero -f

# Останні 50 рядків
sudo journalctl -u jtzero -n 50 --no-pager

# Логи за останню годину
sudo journalctl -u jtzero --since "1 hour ago"

# Логи з помилками
sudo journalctl -u jtzero -p err
```

---

## 2. Збірка C++ ядра

### Перша збірка (або повна перезбірка)

```bash
cd ~/jt-zero/jt-zero
rm -rf build
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j4
```

**Пояснення:**
- `rm -rf build` — видаляє стару збірку
- `mkdir build && cd build` — створює папку для компіляції
- `cmake ..` — аналізує CMakeLists.txt і генерує Makefile
- `make -j4` — компілює, використовуючи 4 ядра Pi
- Займає 5-10 хвилин на Pi Zero 2W

**Результат (якщо успішно):**
```
[100%] Built target jtzero_native
```

### Швидка перезбірка (після зміни коду)

```bash
cd ~/jt-zero/jt-zero/build
make -j4
```

Перекомпілює тільки файли що змінилися. Зазвичай 30-60 секунд.

### Копіювання зібраного модуля в backend

```bash
cp ~/jt-zero/jt-zero/build/jtzero_native*.so ~/jt-zero/backend/
sudo systemctl restart jtzero
```

---

## 3. Оновлення проєкту

### Автоматичне оновлення (рекомендовано)

```bash
cd ~/jt-zero
git pull
./update.sh
```

**`update.sh` автоматично:**
1. `git pull` (якщо є нові зміни)
2. Перекомпілює C++ (тільки змінені файли)
3. Копіює модуль в backend
4. Перевіряє frontend (pre-built з git, npm НЕ потрібен)
5. Перезапускає сервіс
6. Показує статус (MAVLink, камери)

### Оновлення з архіву (без GitHub)

```bash
# Завантажте ZIP з Claude репо:
# https://github.com/iigar/JT_Zero_Core_Claude/archive/refs/heads/main.zip

# На комп'ютері: скопіюйте на Pi через SCP
scp ~/Downloads/JT_Zero_Core_Claude-main.zip pi@jtzero.local:~/

# На Pi:
cd ~
unzip -o JT_Zero_Core_Claude-main.zip
rm -rf jt-zero && mv JT_Zero_Core_Claude-main jt-zero
cd ~/jt-zero && ./update.sh
```

### Оновлення Python залежностей

```bash
cd ~/jt-zero/backend
source venv/bin/activate
pip install -r requirements-pi.txt
sudo systemctl restart jtzero
```

### Dashboard (frontend)

Frontend білдиться автоматично через **GitHub Actions** при пуші змін. На Pi Node.js/npm **не потрібен** — pre-built файли приходять через `git pull`.

---

## 4. Перевірка через API (curl)

Ці команди можна виконувати на Pi або з будь-якого комп'ютера у тій самій мережі.

**На Pi:** використовуйте `http://localhost:8001`
**З комп'ютера:** використовуйте `http://jtzero.local:8001` або `http://<IP>:8001`

### Стан системи

```bash
# Здоров'я сервера
curl -s http://localhost:8001/api/health | python3 -m json.tool

# Короткий статус
curl -s http://localhost:8001/api/health | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'Status: {d[\"status\"]}')
print(f'Mode: {d[\"mode\"]} (native=C++, simulator=Python)')
print(f'Uptime: {d[\"uptime\"]}s')
"
```

### Телеметрія (сенсори, позиція, батарея)

```bash
curl -s http://localhost:8001/api/state | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'--- Attitude ---')
print(f'Roll:  {d.get(\"roll\",0):.2f} deg')
print(f'Pitch: {d.get(\"pitch\",0):.2f} deg')
print(f'Yaw:   {d.get(\"yaw\",0):.2f} deg')
print(f'--- Battery ---')
print(f'Voltage: {d.get(\"battery_voltage\",0):.2f} V')
print(f'Percent: {d.get(\"battery_percent\",0):.0f}%')
print(f'--- Baro ---')
print(f'Pressure: {d.get(\"baro\",{}).get(\"pressure\",0):.1f} hPa')
print(f'Altitude: {d.get(\"baro\",{}).get(\"altitude\",0):.1f} m')
print(f'Temp:     {d.get(\"baro\",{}).get(\"temperature\",0):.1f} C')
print(f'--- GPS ---')
print(f'Lat: {d.get(\"gps\",{}).get(\"lat\",0):.6f}')
print(f'Lon: {d.get(\"gps\",{}).get(\"lon\",0):.6f}')
print(f'Sats: {d.get(\"gps\",{}).get(\"satellites\",0)}')
"
```

### MAVLink з'єднання

```bash
curl -s http://localhost:8001/api/mavlink | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'--- MAVLink ---')
print(f'State:   {d[\"state\"]}')
print(f'FC:      {d[\"fc_firmware\"]} ({d[\"fc_type\"]})')
print(f'Armed:   {d[\"fc_armed\"]}')
print(f'--- Messages ---')
print(f'Sent:    {d[\"messages_sent\"]}')
print(f'Recv:    {d[\"messages_received\"]}')
print(f'Errors:  {d[\"errors\"]}')
print(f'Link Q:  {d[\"link_quality\"]:.0%}')
print(f'--- VO Messages Sent ---')
print(f'Vision:  {d[\"vision_pos_sent\"]}')
print(f'Odom:    {d[\"odometry_sent\"]}')
print(f'Flow:    {d[\"optical_flow_sent\"]}')
"
```

### Сенсори та режими

```bash
# Режими сенсорів (mavlink / hardware / simulated)
curl -s http://localhost:8001/api/sensors | python3 -c "
import sys,json; d=json.load(sys.stdin)
for k in ['imu','baro','gps','rangefinder','optical_flow']:
    print(f'{k:15s} = {d.get(k,\"?\")}')
hw = d.get('hw_info',{})
print(f'--- Hardware ---')
print(f'I2C: {\"Yes\" if hw.get(\"i2c_available\") else \"No\"}')
print(f'IMU: {hw.get(\"imu_model\",\"none\")} ({\"detected\" if hw.get(\"imu_detected\") else \"not found\"})')
print(f'Baro: {hw.get(\"baro_model\",\"none\")} ({\"detected\" if hw.get(\"baro_detected\") else \"not found\"})')
"
```

### Діагностика обладнання

```bash
# Кешована діагностика (швидко)
curl -s http://localhost:8001/api/diagnostics | python3 -c "
import sys,json; d=json.load(sys.stdin)
s=d['summary']
print(f'Platform:  {s[\"platform\"]}')
print(f'Camera:    {s[\"camera\"]}')
print(f'I2C:       {s[\"i2c_devices\"]} devices')
print(f'MAVLink:   {\"Connected\" if s[\"mavlink_connected\"] else \"Disconnected\"}')
print(f'Overall:   {s[\"overall\"]}')
"

# Нове сканування (повільніше, але свіжі дані)
curl -s -X POST http://localhost:8001/api/diagnostics/scan | python3 -m json.tool
```

### Системні метрики (CPU, RAM, температура)

```bash
curl -s http://localhost:8001/api/performance | python3 -c "
import sys,json; d=json.load(sys.stdin)
s=d.get('system',{})
cpu=s.get('cpu',{})
mem=s.get('memory',{})
print(f'CPU:  {cpu.get(\"total_percent\",0):.1f}%')
print(f'RAM:  {mem.get(\"used_mb\",0):.0f} / {mem.get(\"total_mb\",0):.0f} MB')
print(f'Temp: {s.get(\"temperature\",0):.1f} C')
print(f'Disk: {s.get(\"disk\",{}).get(\"used_gb\",0):.1f} / {s.get(\"disk\",{}).get(\"total_gb\",0):.1f} GB')
"
```

### Камера

```bash
# Список камер (CSI + USB)
curl -s http://localhost:8001/api/cameras | python3 -m json.tool

# Статистика основної камери (VO)
curl -s http://localhost:8001/api/camera | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'Type:     {d.get(\"camera_type\",\"?\")}')
print(f'Sensor:   {d.get(\"csi_sensor_name\",\"?\")}')
print(f'Status:   {\"Open\" if d.get(\"camera_open\") else \"Closed\"}')
print(f'FPS:      {d.get(\"fps_actual\",0):.1f}')
print(f'Frames:   {d.get(\"frame_count\",0)}')
print(f'Features: {d.get(\"vo_features_tracked\",0)}/{d.get(\"vo_features_detected\",0)}')
print(f'Conf:     {d.get(\"vo_confidence\",0):.0%}')
print(f'VO valid: {d.get(\"vo_valid\",False)}')
"

# Зберегти кадр з камери
curl -s http://localhost:8001/api/camera/frame -o frame.png

# Список мульти-камер (PRIMARY + SECONDARY)
curl -s http://localhost:8001/api/cameras | python3 -c "
import sys,json; cams=json.load(sys.stdin)
for c in cams:
    print(f'{c[\"slot\"]:10s} {c[\"label\"]:25s} [{\"ACTIVE\" if c[\"active\"] else \"OFF\"}] {c.get(\"csi_sensor\",\"\")}')
"
```

### USB термальна камера (діагностика)

```bash
# Перевірити які камери підключені
v4l2-ctl --list-devices

# Перевірити підтримувані формати (YUYV/MJPG, роздільності)
v4l2-ctl --list-formats-ext -d /dev/video0

# Перевірити поточний формат
v4l2-ctl --get-fmt-video -d /dev/video0

# Детальна перевірка через API
curl -s http://localhost:8001/api/camera | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'Type:      {d.get(\"camera_type\",\"?\")}')
print(f'Open:      {d.get(\"camera_open\")}')
print(f'Size:      {d.get(\"width\",0)}x{d.get(\"height\",0)}')
print(f'FPS:       {d.get(\"fps_actual\",0):.1f}')
print(f'Frames:    {d.get(\"frame_count\",0)}')
print(f'Detected:  {d.get(\"vo_features_detected\",0)}')
print(f'Tracked:   {d.get(\"vo_features_tracked\",0)}')
print(f'Inliers:   {d.get(\"vo_inlier_count\",0)}')
print(f'VO valid:  {d.get(\"vo_valid\")}')
print(f'Confidence:{d.get(\"vo_confidence\",0):.3f}')
"

# Перевірити features позиції
curl -s http://localhost:8001/api/camera/features | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'Feature count: {len(d)}')
if len(d)>0: print(f'First: x={d[0][\"x\"]:.0f} y={d[0][\"y\"]:.0f} tracked={d[0][\"tracked\"]}')
"
```

**Типові значення для Caddx Thermal 256:**
- Type: USB, Size: 480x320, FPS: ~15-25
- Detected: 100-180, Tracked: 16-59, Valid: True
- Confidence: 0.18-0.29 (статична сцена)

### Команди дрону

```bash
# Arm (увімкнути мотори) -- ОБЕРЕЖНО!
curl -X POST http://localhost:8001/api/command \
  -H "Content-Type: application/json" \
  -d '{"command":"arm"}'

# Disarm (вимкнути мотори)
curl -X POST http://localhost:8001/api/command \
  -H "Content-Type: application/json" \
  -d '{"command":"disarm"}'

# Takeoff на 3 метри
curl -X POST http://localhost:8001/api/command \
  -H "Content-Type: application/json" \
  -d '{"command":"takeoff","param1":3.0}'

# Landing
curl -X POST http://localhost:8001/api/command \
  -H "Content-Type: application/json" \
  -d '{"command":"land"}'

# RTL (повернення на точку зльоту)
curl -X POST http://localhost:8001/api/command \
  -H "Content-Type: application/json" \
  -d '{"command":"rtl"}'

# Hold (зависання на місці)
curl -X POST http://localhost:8001/api/command \
  -H "Content-Type: application/json" \
  -d '{"command":"hold"}'

# Emergency Stop (аварійна зупинка)
curl -X POST http://localhost:8001/api/command \
  -H "Content-Type: application/json" \
  -d '{"command":"emergency"}'
```

---

## 5. Діагностика обладнання на Pi

### I2C шина (сенсори)

```bash
# Сканування I2C пристроїв
sudo i2cdetect -y 1

# Відомі адреси:
# 0x68 = MPU6050 / ICM42688P (IMU)
# 0x69 = MPU6050 (альтернативна адреса)
# 0x76 = BMP280 / DPS310 (Барометр)
# 0x77 = BMP280 (альтернативна адреса)
# 0x1E = HMC5883L (Компас)
```

### UART / MAVLink

```bash
# Перевірити що UART доступний
ls -la /dev/ttyAMA0
# Має бути: crw-rw---- 1 root dialout ...

# Перевірити що pi в групі dialout
groups pi
# Якщо ні:
sudo usermod -a -G dialout pi && sudo reboot

# Перевірити UART трафік (сирі байти)
sudo cat /dev/ttyAMA0 | xxd | head -20
# Якщо бачите дані -- MAVLink комунікація працює
```

### Камера

```bash
# Список підключених камер
rpicam-hello --list-cameras

# Тестовий знімок
rpicam-still -o test.jpg
ls -la test.jpg

# Video devices
ls -la /dev/video*
v4l2-ctl --list-devices

# Перевірити boot config
grep camera /boot/firmware/config.txt
```

### Системні ресурси

```bash
# Температура CPU (ділити на 1000 для градусів)
cat /sys/class/thermal/thermal_zone0/temp

# RAM
free -m

# Диск
df -h

# Процеси (інтерактивно)
htop

# IP адреса
hostname -I
```

### Мережа

```bash
# IP адреса Pi
hostname -I

# Перевірити підключення до мережі
ping -c 3 google.com

# Dashboard у браузері (з комп'ютера у тій самій мережі):
# http://jtzero.local:8001
# або http://<IP_АДРЕСА>:8001
```

---

## 6. Troubleshooting (вирішення проблем)

### Сервер не стартує

```bash
# 1. Перевірити логи
sudo journalctl -u jtzero -n 50 --no-pager

# 2. Тест імпорту вручну
cd ~/jt-zero/backend
source venv/bin/activate
python3 -c "from server import app; print('Server OK')"

# 3. Тест C++ модуля
python3 -c "import jtzero_native; print('C++ OK')"

# 4. Перевірити venv
ls ~/jt-zero/backend/venv/bin/uvicorn
```

### MAVLink не підключається

```bash
# 1. Перевірити UART device
ls -la /dev/ttyAMA0

# 2. Перевірити групу dialout
groups pi

# 3. Перевірити baud rate в конфігурації FC
# Має бути SERIAL4_BAUD = 115 (115200)

# 4. Перевірити проводку:
#    Pi TX (GPIO14) → FC RX
#    Pi RX (GPIO15) → FC TX
#    GND → GND
```

### Камера не працює

```bash
# CSI камера:
# 1. Перевірити підключення
rpicam-hello --list-cameras

# 2. Перевірити boot config
grep camera /boot/firmware/config.txt
# Має бути: camera_auto_detect=1

# 3. Перевірити dmesg
dmesg | grep -i "camera\|csi\|imx\|ov5647"

# USB камера:
# 1. Перевірити підключення
v4l2-ctl --list-devices
ls -la /dev/video*

# 2. Перевірити підтримувані формати (YUYV потрібен для JT-Zero)
v4l2-ctl --list-formats-ext -d /dev/video0

# 3. Перевірити MMAP потік
sudo journalctl -u jtzero -n 30 --no-pager | grep -i "USB\|camera\|MMAP"
# Має бути: [USB] Camera opened: /dev/video0 480x320 YUYV (MMAP streaming)

# 4. Перевірити API
curl -s http://localhost:8001/api/camera | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'Type: {d.get(\"camera_type\",\"?\")} Open: {d.get(\"camera_open\")} Size: {d.get(\"width\",0)}x{d.get(\"height\",0)}')
"
```

### VO не працює (VisOdom: not healthy)

```bash
# 1. Перевірити що VO повідомлення відправляються
curl -s http://localhost:8001/api/mavlink | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'Vision sent: {d[\"vision_pos_sent\"]}')
print(f'Odometry sent: {d[\"odometry_sent\"]}')
"
# Обидва лічильники мають рости кожні кілька секунд

# 2. Перевірити параметри ArduPilot:
# VISO_TYPE = 1
# EK3_SRC1_POSXY = 6
# EK3_SRC1_VELXY = 6

# 3. Перевірити якість tracking
curl -s http://localhost:8001/api/camera | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'Det:   {d.get(\"vo_features_detected\",0):3d}')
print(f'Track: {d.get(\"vo_features_tracked\",0):3d}')
print(f'Inl:   {d.get(\"vo_inlier_count\",0):3d}')
print(f'Valid: {d.get(\"vo_valid\",False)}')
print(f'Conf:  {d.get(\"vo_confidence\",0):.3f}')
"
# Для CSI: Track >= 30 features
# Для USB thermal: Track >= 10 features, Conf > 0.1

# 4. Якщо Track = 0 на USB термальній камері:
# - Переконайтеся що FAST threshold не завеликий (25 = нормально)
# - Shi-Tomasi fallback автоматично активується при < 15 FAST features
# - Перевірте що камера НЕ дивиться на однорідну поверхню
```

### ArduPilot Pre-Arm помилки

```bash
# "Rangefinder 1: No Data"
# → В Mission Planner встановити: RNGFND1_TYPE = 0

# "Battery 1 below minimum arming voltage"
# → Зарядити батарею, або для тесту: BATT_ARM_VOLT = 0
# УВАГА: не літайте з BATT_ARM_VOLT = 0!

# "VisOdom: not healthy"
# → Перевірити що JT-Zero запущений і VO лічильники ростуть (див. вище)
```

---

## 7. Корисні однорядкові команди

```bash
# Повний статус одним рядком
curl -s http://localhost:8001/api/health | python3 -c "import sys,json;d=json.load(sys.stdin);print(f'{d[\"status\"]} | {d[\"mode\"]} | uptime {d[\"uptime\"]}s')"

# MAVLink стан одним рядком
curl -s http://localhost:8001/api/mavlink | python3 -c "import sys,json;d=json.load(sys.stdin);print(f'{d[\"state\"]} | TX:{d[\"messages_sent\"]} RX:{d[\"messages_received\"]} | VO:{d[\"vision_pos_sent\"]} Odom:{d[\"odometry_sent\"]}')"

# CPU + RAM + Temp одним рядком
curl -s http://localhost:8001/api/performance | python3 -c "import sys,json;d=json.load(sys.stdin);s=d.get('system',{});print(f'CPU:{s.get(\"cpu\",{}).get(\"total_percent\",0):.0f}% RAM:{s.get(\"memory\",{}).get(\"used_mb\",0):.0f}MB Temp:{s.get(\"temperature\",0):.0f}C')"
```
