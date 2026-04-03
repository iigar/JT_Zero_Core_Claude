# JT-Zero — Повна інструкція встановлення на Raspberry Pi

Ця інструкція написана для початківців. Вона покриває ВСЕ: від чистого Pi до працюючої системи. Навіть якщо ви ніколи не працювали з Linux чи Raspberry Pi — просто виконуйте команди по порядку.

**Підтримувані Pi:** Zero 2 W, 3B+, 4, 5
**Час встановлення:** ~15 хвилин

---

## Що потрібно

| Компонент | Обов'язково? | Опис |
|-----------|-------------|------|
| Raspberry Pi Zero 2 W | Так | Або Pi 3B+, Pi 4, Pi 5 |
| SD-карта 8+ ГБ | Так | Рекомендовано 16 ГБ |
| Кабель micro-USB + блок живлення 5V 2.5A | Так | Для живлення Pi |
| Комп'ютер у тій самій Wi-Fi мережі | Так | Для SSH та Dashboard |
| CSI камера | Рекомендовано | Pi Camera v2, IMX290, або інша (8 моделей + GENERIC) |
| Польотний контролер (ArduPilot 4.3+) | Рекомендовано | Matek H743, Pixhawk, SpeedyBee, Cube |

---

## Етап 1: Підготовка SD-карти

### 1.1. Завантажте Raspberry Pi Imager

На вашому комп'ютері (Windows / Mac / Linux) перейдіть на сайт:

```
https://www.raspberrypi.com/software/
```

Завантажте та встановіть програму.

### 1.2. Запишіть образ ОС

1. Запустіть Raspberry Pi Imager
2. **"Choose Device"** → виберіть вашу модель Pi
3. **"Choose OS"** → **"Raspberry Pi OS (other)"** → **"Raspberry Pi OS Lite (64-bit)"**
   - Lite = без графічного інтерфейсу, швидша та легша
4. **"Choose Storage"** → виберіть вашу SD-карту

### 1.3. Налаштуйте доступ (ДУЖЕ ВАЖЛИВО!)

Натисніть **іконку шестерні** (або Ctrl+Shift+X):

| Налаштування | Значення |
|-------------|----------|
| Hostname | `jtzero` (або `jtzero-1`, `jtzero-2` для кількох Pi) |
| Enable SSH | Так, "Use password authentication" |
| Username | `pi` |
| Password | ваш пароль (запам'ятайте!) |
| Wi-Fi SSID | назва вашої Wi-Fi мережі |
| Wi-Fi Password | пароль від Wi-Fi |
| Wi-Fi Country | UA |

Натисніть **"Save"**, потім **"Write"**. Зачекайте поки запишеться.

### 1.4. Вставте SD-карту в Pi і увімкніть живлення

Зачекайте 1-2 хвилини — Pi завантажиться і підключиться до Wi-Fi.

---

## Етап 2: Підключення до Pi через SSH

### На Windows:
Відкрийте **PowerShell** (пошук → "PowerShell")

### На Mac / Linux:
Відкрийте **Terminal**

### Підключіться:

```bash
ssh pi@jtzero.local
```

Введіть пароль, який задали в Imager.

**Якщо `jtzero.local` не працює:**
- Зайдіть у роутер (зазвичай `192.168.1.1` у браузері)
- Знайдіть пристрій `jtzero` і його IP-адресу
- Підключіться: `ssh pi@192.168.1.XX`

**Якщо побачите `pi@jtzero:~ $`** — ви всередині Pi!

**Якщо SSH каже "REMOTE HOST IDENTIFICATION HAS CHANGED"** — це нормально після перевстановлення Pi OS. На вашому комп'ютері виконайте:
```bash
ssh-keygen -R jtzero.local
```
І підключіться знову.

---

## Етап 3: Автоматичне встановлення (РЕКОМЕНДОВАНО)

**Одна команда — все автоматично:**

```bash
sudo apt update && sudo apt install -y git
git clone https://github.com/iigar/JT_Zero_Core_Claude.git ~/jt-zero
cd ~/jt-zero
chmod +x setup.sh
./setup.sh
```

**Що робить `setup.sh` автоматично:**
1. Встановлює системні пакети (cmake, g++, python3, pybind11, i2c-tools)
2. Вмикає I2C, SPI, Serial Port, Camera
3. Збирає C++ ядро (make -j2 на Pi Zero, -j4 на Pi 4)
4. Копіює C++ модуль в backend
5. Створює Python venv та встановлює FastAPI
6. Налаштовує systemd сервіс (автозапуск)
7. Перезавантажує Pi

**Після перезавантаження** (1-2 хвилини):
```bash
ssh pi@jtzero.local
sudo systemctl status jtzero
```

Має бути зелений **`active (running)`**.

**Dashboard:** відкрийте в браузері `http://jtzero.local:8001`

**Готово!** Переходьте до Етапу 5 (підключення камери).

---

## Етап 3Б: Без GitHub (офлайн, через архів)

Цей спосіб працює навіть якщо на Pi НЕМАЄ інтернету.

### На комп'ютері з інтернетом:

1. Скачайте: `https://github.com/iigar/JT_Zero_Core_Claude/archive/refs/heads/main.zip`
2. Скопіюйте на Pi:

**Windows (PowerShell):**
```powershell
scp $env:USERPROFILE\Downloads\JT_Zero_Core_Claude-main.zip pi@jtzero.local:~/
```

**Mac/Linux:**
```bash
scp ~/Downloads/JT_Zero_Core_Claude-main.zip pi@jtzero.local:~/
```

### На Pi:

```bash
cd ~
unzip JT_Zero_Core_Claude-main.zip
mv JT_Zero_Core_Claude-main jt-zero
cd ~/jt-zero
chmod +x setup.sh
./setup.sh
```

### Альтернатива: USB флешка (якщо Pi НЕ в мережі)

1. Скачайте ZIP на USB флешку
2. Вставте флешку в Pi через micro-USB OTG адаптер
3. На Pi:
```bash
sudo mkdir -p /mnt/usb && sudo mount /dev/sda1 /mnt/usb
cp /mnt/usb/JT_Zero_Core_Claude-main.zip ~/
sudo umount /mnt/usb
cd ~ && unzip JT_Zero_Core_Claude-main.zip && mv JT_Zero_Core_Claude-main jt-zero
cd ~/jt-zero && chmod +x setup.sh && ./setup.sh
```

---

## Етап 4: Оновлення системи

### З GitHub (рекомендовано):

```bash
cd ~/jt-zero
git pull
./update.sh
```

**Що робить `update.sh` автоматично:**
1. `git pull` — отримує нові зміни
2. Збирає C++ ядро (тільки змінені файли)
3. Копіює модуль
4. Перевіряє frontend (pre-built з git, npm НЕ потрібен)
5. Перезапускає сервіс
6. Показує статус (MAVLink, камери)

### Без GitHub (архівом):

1. На комп'ютері: скачайте новий ZIP з GitHub
2. На Pi:
```bash
scp ~/Downloads/JT_Zero_Core_Claude-main.zip pi@jtzero.local:~/
# На Pi:
cd ~ && unzip -o JT_Zero_Core_Claude-main.zip
rm -rf jt-zero && mv JT_Zero_Core_Claude-main jt-zero
cd ~/jt-zero && ./update.sh
```

---

## Етап 5: Підключення камери

### CSI камера (основна, для Visual Odometry)

**Підтримувані моделі (авто-детекція):**

| Сенсор | Камера | Роздільність | FOV | Особливість |
|--------|--------|-------------|-----|-------------|
| OV5647 | Pi Camera v1 | 5MP | 62° | Найдешевша (~$5) |
| IMX219 | Pi Camera v2 | 8MP | 62° | Найпопулярніша (~$25) |
| IMX477 | Pi HQ Camera | 12.3MP | залежить від лінзи | Змінні об'єктиви |
| IMX708 | Pi Camera v3 | 12MP | 66° | Автофокус |
| OV9281 | Global Shutter | 1MP | 80° | Ідеальна для VO |
| IMX296 | Pi GS Camera | 1.6MP | 49° | Global shutter |
| OV64A40 | Arducam 64MP | 64MP | 84° | Максимальна роздільність |
| IMX290 | STARVIS | 2MP | 82° | Нічне бачення |
| *будь-яка інша* | GENERIC | auto | auto | Будь-яка libcamera-сумісна |

#### Фізичне підключення

1. **Вимкніть Pi** (відключіть живлення)
2. На Pi Zero 2W: маленький білий роз'єм — **22-pin міні-CSI**
   - **ВАЖЛИВО:** Стандартний шлейф Pi 3/4 (15-pin) НЕ підходить! Потрібен "Pi Zero Camera Cable"
3. На Pi 4/5: стандартний **15-pin CSI** роз'єм
4. Підніміть фіксатор (чорна планка)
5. Вставте шлейф контактами вниз
6. Закрийте фіксатор
7. Увімкніть Pi

#### Перевірка

```bash
rpicam-hello --list-cameras
```

Має показати камеру, наприклад:
```
0 : imx219 [3280x2464 10-bit]
```

#### Якщо камера не знайдена

```bash
# Перевірити boot config
grep camera /boot/firmware/config.txt
```

Для стандартних камер (v1, v2, v3, HQ) має бути:
```
camera_auto_detect=1
```

Для нестандартних камер (IMX290, IMX462 тощо) потрібен ручний overlay:
```bash
sudo nano /boot/firmware/config.txt
# Змінити: camera_auto_detect=0
# Додати в кінці:
# dtoverlay=imx290,clock-frequency=37125000
sudo reboot
```

Після ребуту `rpicam-hello --list-cameras` має показати камеру. JT-Zero автоматично її підхопить.

### USB Thermal камера (допоміжна, для термального сканування)

Просто підключіть USB камеру. Для Pi Zero 2W потрібен micro-USB OTG адаптер.

```bash
# Перевірка
v4l2-ctl --list-devices
# Має з'явитися UVC пристрій
```

JT-Zero автоматично призначить USB камеру як SECONDARY (термальну). CSI камера завжди PRIMARY (VO).

---

## Етап 6: Підключення до польотного контролера

Детальна інструкція: **[FC_CONNECTION.md](FC_CONNECTION.md)**

### Дроти (3 штуки):

```
Pi Pin 8  (TX)  ──> FC RX (UART порт)
Pi Pin 10 (RX)  <── FC TX
Pi Pin 6  (GND) ─── FC GND
```

### UART порти по контролерах:

| FC | UART | Serial |
|----|------|--------|
| Matek H743-SLIM V3 | UART6 | SERIAL6 |
| SpeedyBee F405 V4 | UART4 | SERIAL4 |
| Pixhawk 2.4.8 | TELEM2 | SERIAL2 |
| Cube Orange+ | TELEM2 | SERIAL2 |

### Параметри ArduPilot (Mission Planner):

```
SERIALx_PROTOCOL = 2    (MAVLink2)
SERIALx_BAUD = 115      (115200 — JT-Zero визначить автоматично)
VISO_TYPE = 1            (MAVLink vision)
EK3_SRC1_POSXY = 6      (ExternalNav)
EK3_SRC1_VELXY = 6      (ExternalNav)
EK3_SRC1_POSZ = 1       (Baro — якщо немає rangefinder)
```

---

## Етап 7: Підключення сенсорів (опціонально)

### I2C сенсори (MPU6050, BMP280)

```
  Pi GPIO Header
  ─────────────────────
  3V3  (1) (2)  5V
  SDA  (3) (4)  5V        ← MPU6050 + BMP280: SDA
  SCL  (5) (6)  GND       ← MPU6050 + BMP280: SCL
  GP4  (7) (8)  TX (UART) ← GPS RX / FC RX
  GND  (9) (10) RX (UART) ← GPS TX / FC TX
```

Перевірка:
```bash
sudo i2cdetect -y 1
# 0x68 = MPU6050, 0x76 = BMP280
```

**Не обов'язково:** JT-Zero працює без зовнішніх сенсорів. IMU, Baro, GPS дані отримуються від польотного контролера через MAVLink.

---

## Перевірка після встановлення

```bash
# 1. Статус сервісу
sudo systemctl status jtzero

# 2. MAVLink з'єднання
curl -s http://localhost:8001/api/mavlink | python3 -m json.tool

# 3. Камери
curl -s http://localhost:8001/api/cameras | python3 -m json.tool

# 4. VO статус (рухайте камеру!)
curl -s http://localhost:8001/api/camera | python3 -c "
import sys,json; d=json.load(sys.stdin)
print(f'Camera:  {d.get(\"csi_sensor_name\", d.get(\"camera_type\",\"?\"))}')
print(f'DET:     {d.get(\"vo_features_detected\",0)}')
print(f'TRACK:   {d.get(\"vo_features_tracked\",0)}')
print(f'CONF:    {d.get(\"vo_confidence\",0):.0%}')
print(f'Valid:   {d.get(\"vo_valid\")}')
"

# 5. Dashboard в браузері
# http://jtzero.local:8001
```

---

## Вирішення проблем

### "Cannot connect to jtzero.local"
- Pi і комп'ютер мають бути в одній Wi-Fi мережі
- Спробуйте IP адресу: на Pi виконайте `hostname -I`

### "REMOTE HOST IDENTIFICATION HAS CHANGED"
Нова Pi OS = нові SSH ключі. На вашому комп'ютері:
```bash
ssh-keygen -R jtzero.local
ssh-keygen -R <IP-Pi>
```

### Сервер не запускається
```bash
sudo journalctl -u jtzero -n 50 --no-pager
```

### Камера не знайдена
```bash
rpicam-hello --list-cameras    # CSI
v4l2-ctl --list-devices        # USB
dmesg | grep -i camera         # Kernel logs
```

### Git — зламані об'єкти (після збою живлення)
```bash
find .git/objects/ -type f -empty -delete
git fsck --full
git pull
```

### ArduPilot Pre-Arm помилки

| Помилка | Рішення |
|---------|---------|
| "Rangefinder 1: No Data" | `RNGFND1_TYPE = 0` |
| "Battery below minimum arming" | Зарядіть або `BATT_ARM_VOLT = 0` (тільки тест!) |
| "VisOdom: not healthy" | `curl http://localhost:8001/api/mavlink` |

---

## Порядок дій після встановлення

1. Відкрийте Dashboard: `http://jtzero.local:8001`
2. Перевірте вкладку **Settings** → Hardware Diagnostics
3. Перевірте вкладку **MAVLink** — статус має бути **CONNECTED**
4. У Mission Planner перевірте Pre-Arm Messages
5. **Перший тест: БЕЗ ПРОПЕЛЕРІВ!**

---

## Структура файлів проєкту

```
~/jt-zero/
├── jt-zero/              # C++ ядро
│   ├── camera/           # Camera drivers + VO pipeline
│   ├── core/             # Runtime (8 threads)
│   ├── mavlink/          # MAVLink parser + transport
│   └── include/          # Headers
├── backend/              # FastAPI сервер
│   ├── server.py         # REST API + WebSocket
│   ├── simulator.py      # Python симулятор
│   └── static/           # Pre-built React frontend
├── frontend/             # React (білдиться GitHub Actions)
├── setup.sh              # Повна установка
├── update.sh             # Швидке оновлення
├── commands_reminder.md  # Шпаргалка команд
├── CLAUDE.md             # Технічна довідка
└── ABOUT_PROJECT.md      # Про проєкт
```
