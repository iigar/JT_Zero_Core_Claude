# JT-Zero: Система Visual Odometry для дронів

## Що це і навіщо?

JT-Zero — це **companion computer система** для дронів. Вона вирішує конкретну проблему: **стабільний політ без GPS** (у приміщеннях, підвалах, тунелях, складах).

Як це працює: CSI камера дивиться вперед, аналізує переміщення ландшафту і обчислює позицію дрона. Польотний контролер (ArduPilot) використовує ці дані замість GPS для стабілізації і навігації.

**Коштує:** ~$60 (Pi Zero 2W + камера + дроти) замість $500+ за промислові рішення.

---

## Документація

| Документ | Що описує |
|----------|-----------|
| **[SYSTEM.md](SYSTEM.md)** | Алгоритм VO, характеристики, архітектура, обмеження |
| **[DEPLOYMENT.md](DEPLOYMENT.md)** | Покрокова установка (setup.sh — автоматично). Два способи: GitHub або офлайн |
| **[COMMANDS.md](COMMANDS.md)** | Всі команди: update.sh, API (curl), діагностика, troubleshooting |
| **[FC_CONNECTION.md](FC_CONNECTION.md)** | Підключення до польотного контролера |
| **[LONG_RANGE_FLIGHT.md](LONG_RANGE_FLIGHT.md)** | Конфігурація для 5+ км польотів (VO+IMU, без GPS) |

---

## Швидкий старт

```bash
# На Pi через SSH:
sudo apt update && sudo apt install -y git
git clone https://github.com/iigar/JT_Zero_Core_Claude.git ~/jt-zero
cd ~/jt-zero
chmod +x setup.sh && ./setup.sh
# setup.sh робить ВСЕ: deps, UART, I2C, C++ build, Python venv, systemd, reboot
```

**Після перезавантаження:** Dashboard на `http://jtzero.local:8001`

### Оновлення:
```bash
cd ~/jt-zero && git pull && ./update.sh
```

---

## Встановлення без GitHub

### Варіант 1: ZIP з сайту

1. На комп'ютері: `https://github.com/iigar/JT_Zero_Core_Claude/archive/refs/heads/main.zip`
2. `scp ~/Downloads/JT_Zero_Core_Claude-main.zip pi@jtzero.local:~/`
3. На Pi: `unzip JT_Zero_Core_Claude-main.zip && mv JT_Zero_Core_Claude-main jt-zero && cd jt-zero && ./setup.sh`

### Варіант 2: USB флешка (без мережі)

1. Скачайте ZIP на флешку
2. Вставте в Pi через OTG адаптер
3. `sudo mount /dev/sda1 /mnt && cp /mnt/*.zip ~/ && sudo umount /mnt`
4. `unzip *.zip && mv JT_Zero_Core_Claude-main jt-zero && cd jt-zero && ./setup.sh`

---

## Архітектура

```
┌─────────────────┐      ┌──────────────────┐      ┌────────────────┐
│   CSI Camera    │─────>│  C++ Core        │─────>│  Flight        │
│   (Forward VO)  │ MIPI │  - Feature Det.  │ UART │  Controller    │
│                 │      │  - Visual Odom.  │      │  (ArduPilot)   │
└─────────────────┘      │  - MAVLink TX/RX │      └────────────────┘
                         └────────┬─────────┘
┌─────────────────┐              │ pybind11
│  USB Thermal    │──────>┌──────┴──────────┐
│  (Down, scan)   │ V4L2  │  FastAPI Backend │
└─────────────────┘       │  + WebSocket     │
                          └────────┬─────────┘
                          ┌────────┴─────────┐
                          │  React Dashboard  │
                          │  (7 вкладок)      │
                          └──────────────────┘
```

## Підтримувані камери (CSI — авто-детекція)

| Сенсор | Камера | Роздільність | FOV |
|--------|--------|-------------|-----|
| OV5647 | Pi Camera v1 | 5MP | 62° |
| IMX219 | Pi Camera v2 | 8MP | 62° |
| IMX477 | Pi HQ Camera | 12.3MP | lens |
| IMX708 | Pi Camera v3 | 12MP | 66° |
| OV9281 | Global Shutter | 1MP | 80° |
| IMX296 | Pi GS Camera | 1.6MP | 49° |
| OV64A40 | Arducam 64MP | 64MP | 84° |
| IMX290 | STARVIS | 2MP | 82° |
| *будь-яка* | GENERIC | auto | auto |

Будь-яка libcamera-сумісна камера працює автоматично.

## Стек технологій

| Компонент | Технологія |
|-----------|-----------|
| Ядро | C++17, lock-free, 8 потоків реального часу |
| Зв'язка C++/Python | pybind11 |
| Backend | FastAPI, WebSocket, uvicorn |
| Frontend | React 19, Recharts, Tailwind CSS, Three.js |
| CI/CD | GitHub Actions (auto-build frontend) |
| Протокол | MAVLink v2 (CRC-validated) |
| Платформи | Raspberry Pi Zero 2W, Pi 4, Pi 5 |
| Камери | 8 CSI моделей + GENERIC, USB thermal (Caddx 256) |

## Можливості

- Visual Odometry з точністю ±5-20 см
- **Далекий політ: до 5+ км з RTL (VO+IMU, без GPS)**
- Ефективна швидкість: до 2-3 м/с
- Робоча висота: 0.3-10 м (оптимально 1-3 м)
- Частота VO: ~12 Hz (ArduPilot EKF приймає)
- **FAST + Shi-Tomasi детектори** (каскад для термальних камер)
- **LK трекер з Sobel градієнтами і білінійною інтерполяцією**
- Kalman-фільтрована швидкість + outlier rejection
- Confidence-based covariance для EKF
- **Мульти-камера:** CSI (PRIMARY/VO вперед) + USB thermal (SECONDARY/вниз)
- **8 CSI сенсорів** авто-детекція + GENERIC fallback для будь-яких інших
- **GitHub Actions CI/CD** — frontend білдиться автоматично, Pi не потребує Node.js
- **setup.sh** — повна установка одною командою
- **update.sh** — оновлення: git pull + C++ build + frontend + restart
- 7-вкладковий Dashboard з реальним часом

## Перевірено на залізі

- **Pi Zero 2W + IMX219** — CONNECTED, VO Valid:True
- **Pi Zero 2W + IMX290 STARVIS** — CONNECTED, DET:180, TRACK:44, Valid:True
- **Pi 4 + IMX219 + Caddx thermal** — CONNECTED, multi-camera working
- **Matek H743** — EKF3 ExternalNav confirmed
