# JT-Zero Runtime

Автономна система візуальної навігації для дронів на базі Raspberry Pi Zero 2 W.

## Навіщо це?

Дрони не можуть стабільно літати у приміщеннях без GPS. JT-Zero вирішує цю проблему: камера Pi аналізує переміщення поверхні і передає позицію на польотний контролер через MAVLink. Дрон літає стабільно навіть без GPS, використовуючи лише камеру за $15.

## Що входить

- **C++ ядро** — обробка відео та сенсорів в реальному часі (8 потоків)
- **Python сервер** — FastAPI бекенд з WebSocket стрімінгом
- **React Dashboard** — 7-вкладковий моніторинг у браузері
- **MAVLink** — повна двостороння інтеграція з ArduPilot
- **Multi-Camera** — CSI (Visual Odometry) + USB Thermal (сканування)

## Архітектура

```
┌─────────────────┐      ┌──────────────────┐      ┌────────────────┐
│   CSI Camera    │─────>│  C++ Core        │─────>│  Flight        │
│   (IMX219/      │ MIPI │  - Feature Det.  │ UART │  Controller    │
│    IMX290/...)  │      │  - Visual Odom.  │      │  (ArduPilot)   │
└─────────────────┘      │  - MAVLink TX/RX │      └────────────────┘
                         └────────┬─────────┘
┌─────────────────┐              │ pybind11
│  USB Thermal    │──────>┌──────┴──────────┐
│  (Caddx 256)    │ V4L2  │  FastAPI Backend │
└─────────────────┘       │  - WebSocket     │
                          │  - REST API      │
                          └────────┬─────────┘
                                   │ HTTP/WS
                          ┌────────┴─────────┐
                          │  React Dashboard  │
                          │  (браузер)        │
                          └──────────────────┘
```

## Підтримувані камери (CSI)

| Сенсор | Камера | Роздільність | FOV |
|--------|--------|-------------|-----|
| OV5647 | Pi Camera v1 | 5MP | 62° |
| IMX219 | Pi Camera v2 | 8MP | 62° |
| IMX477 | Pi HQ Camera | 12.3MP | lens |
| IMX708 | Pi Camera v3 | 12MP | 66° |
| OV9281 | Global Shutter | 1MP | 80° |
| IMX296 | Pi GS Camera | 1.6MP | 49° |
| OV64A40 | Arducam 64MP | 64MP | 84° |
| IMX290 | STARVIS (low-light) | 2MP | 82° |
| *будь-який* | GENERIC fallback | auto | auto |

## Швидкий старт

```bash
# Клонувати (Claude версія — IMU-VO fusion + cyberpunk HUD)
git clone https://github.com/iigar/JT_Zero_Core_Claude.git ~/jt-zero
cd ~/jt-zero

# Встановити (deps, UART, build, systemd)
chmod +x setup.sh && ./setup.sh

# Оновити
git pull && ./update.sh
```

**Frontend** білдиться автоматично через GitHub Actions. На Pi Node.js/npm **не потрібен**.

## Встановлення без GitHub

```bash
# Завантажте ZIP на будь-який комп'ютер
# https://github.com/iigar/JT_Zero_Core_Claude/archive/refs/heads/main.zip

# Скопіюйте на Pi
scp JT_Zero_Core_Claude-main.zip pi@jtzero.local:~/
ssh pi@jtzero.local
unzip JT_Zero_Core_Claude-main.zip && mv JT_Zero_Core_Claude-main jt-zero
cd jt-zero && chmod +x setup.sh && ./setup.sh
```

## Документація

| Файл | Опис |
|------|------|
| [ABOUT_PROJECT.md](ABOUT_PROJECT.md) | Детальний опис проєкту |
| [CLAUDE.md](CLAUDE.md) | Технічна довідка (для розробників та AI) |
| [commands_reminder.md](commands_reminder.md) | Шпаргалка команд |
| [jt-zero/SYSTEM.md](jt-zero/SYSTEM.md) | Алгоритм VO, платформи, режими |
| [jt-zero/DEPLOYMENT.md](jt-zero/DEPLOYMENT.md) | Встановлення на Pi |
| [jt-zero/COMMANDS.md](jt-zero/COMMANDS.md) | API, діагностика |
| [jt-zero/FC_CONNECTION.md](jt-zero/FC_CONNECTION.md) | Підключення до польотного контролера |
| [jt-zero/LONG_RANGE_FLIGHT.md](jt-zero/LONG_RANGE_FLIGHT.md) | Гайд автономного польоту на 5км |

## Стек технологій

| Компонент | Технологія |
|-----------|-----------|
| Ядро | C++17, lock-free, real-time (8 threads) |
| Зв'язка C++/Python | pybind11 |
| Backend | FastAPI, WebSocket, uvicorn |
| Frontend | React 19, Recharts, Tailwind CSS |
| Протокол | MAVLink v2 (CRC-validated) |
| CI/CD | GitHub Actions (auto-build frontend) |
| Платформа | Raspberry Pi Zero 2W / Pi 4 / Pi 5 |

## Стан IMU-VO fusion

| Компонент | Що реалізовано |
|-----------|---------------|
| Attitude | Complementary filter α=0.98 (roll/pitch), gyro bias estimate (yaw) |
| Kalman | IMU prediction step (`kf_v += a·dt`) + VO measurement update |
| LK Tracking | IMU pre-integration hints: seeds initial flow from inter-frame rotation |
| Uncertainty | `sqrt(pose_var_x + pose_var_y)` з KF covariance (не ad-hoc) |
| Bias | On-ground (`!armed`) + hover gyro_z bias EMA estimation |

## Статус

- VO працює на реальному залізі (Pi Zero 2W + IMX290/IMX219)
- MAVLink підключений до ArduPilot (Matek H743)
- EKF3 ExternalNav підтверджено
- 15 fps VO, 25Hz MAVLink, 0 CRC помилок
- Повний IMU-VO fusion pipeline активний (6 покращень 2026-04-03)
