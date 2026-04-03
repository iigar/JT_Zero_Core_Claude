# JT-Zero — Команди (шпаргалка)

## Git — Щоденна робота

```bash
# Репозиторій: https://github.com/iigar/JT_Zero_Core_Claude (гілка: main)

# Отримати останні зміни
git pull

# Подивитися на якій гілці зараз
git branch

# Подивитися всі гілки (локальні + remote)
git branch -a

# Подивитися останні 5 комітів
git log --oneline -5

# Подивитися що змінилось (ще не закомічено)
git status

# Подивитися різницю у файлах
git diff

# Закомітити і запушити свої зміни
git add -A
git commit -m "опис змін"
git push
```

## Оновлення Pi

```bash
# Повне оновлення (git + C++ + frontend + restart)
cd ~/jt-zero
git pull
./update.sh

# Тільки перезапуск сервісу (без перекомпіляції)
sudo systemctl restart jtzero

# Перевірити статус сервісу
sudo systemctl status jtzero

# Логи сервісу (останні 50 рядків)
journalctl -u jtzero -n 50 --no-pager

# Логи в реальному часі (Ctrl+C щоб вийти)
journalctl -u jtzero -f
```

## Перевірка API

```bash
# Здоров'я сервера
curl -s http://localhost:8001/api/health | python3 -m json.tool

# MAVLink статус
curl -s http://localhost:8001/api/mavlink | python3 -m json.tool

# Камери
curl -s http://localhost:8001/api/cameras | python3 -m json.tool

# VO статус
curl -s http://localhost:8001/api/vo | python3 -m json.tool

# Системні метрики (CPU, RAM, температура)
curl -s http://localhost:8001/api/system | python3 -m json.tool
```

## Git — Claude репо

```bash
# Репозиторій: https://github.com/iigar/JT_Zero_Core_Claude
# Гілка: main (єдина, не треба перемикати)

# Отримати оновлення від Claude
cd ~/jt-zero
git pull origin main

# Перевірити поточний стан
git log --oneline -5
```

## Перша установка (новий Pi)

```bash
# Клонувати репозиторій (Claude версія з IMU-VO fusion + cyberpunk HUD)
git clone https://github.com/iigar/JT_Zero_Core_Claude.git ~/jt-zero

# Запустити повну установку (залежності, UART, I2C, systemd)
cd ~/jt-zero
chmod +x setup.sh
./setup.sh

# Після перезавантаження — перевірити
sudo systemctl status jtzero
curl -s http://localhost:8001/api/health
```

## Збірка компонентів окремо

```bash
# Тільки C++ (без frontend, без restart)
cd ~/jt-zero/jt-zero/build
make -j2          # Pi Zero
make -j4          # Pi 4
cp jtzero_native*.so ../../backend/

# Тільки frontend (якщо є npm на Pi 4)
cd ~/jt-zero/frontend
npm install
REACT_APP_BACKEND_URL="" npm run build
rm -rf ../backend/static
cp -r build ../backend/static
```

## Діагностика проблем

```bash
# Сервіс не стартує
sudo systemctl status jtzero
journalctl -u jtzero -n 100 --no-pager

# Перевірити UART (MAVLink з FC)
ls -la /dev/serial0 /dev/ttyAMA0 /dev/ttyS0 2>/dev/null
# Має бути /dev/serial0 -> ttyAMA0

# Перевірити I2C
sudo i2cdetect -y 1

# Перевірити CSI камеру
rpicam-hello --list-cameras
libcamera-hello --list-cameras

# Перевірити USB камеру
v4l2-ctl --list-devices
ls /dev/video*

# Вільне місце на SD
df -h /

# RAM та swap
free -h

# Температура CPU
vcgencmd measure_temp

# Git — зламані об'єкти (після збою живлення)
find .git/objects/ -type f -empty -delete
git fsck --full
git pull
```

## Мережа та доступ

```bash
# IP адреса Pi
hostname -I

# Доступ до дашборду з іншого пристрою
# Відкрити в браузері: http://<IP-Pi>:8001

# SSH на Pi
ssh pi@<IP-Pi>

# Копіювання файлу на Pi
scp файл.txt pi@<IP-Pi>:~/jt-zero/
```

## Після перевстановлення Pi OS / дублювання SD карти

```bash
# SSH видає "REMOTE HOST IDENTIFICATION HAS CHANGED" —
# нова Pi OS = нові SSH ключі. Це нормально, не атака.
# На Windows (PowerShell):
ssh-keygen -R jtzero.local
ssh-keygen -R <IP-Pi>

# На Linux/Mac:
ssh-keygen -R jtzero.local
ssh-keygen -R <IP-Pi>

# Потім підключитись знову:
ssh pi@jtzero.local
# Прийняти новий ключ: yes

# Після входу — встановити JT-Zero (Claude версія):
git clone https://github.com/iigar/JT_Zero_Core_Claude.git ~/jt-zero
cd ~/jt-zero
chmod +x setup.sh && ./setup.sh
```

## Масштабування (кілька Pi з однаковою SD)

```bash
# Якщо клонуєте SD карту на інші Pi:
# 1. Кожен Pi отримає нові SSH ключі при першому boot
# 2. На вашому комп'ютері очистіть старі ключі:
ssh-keygen -R <старий-IP>
ssh-keygen -R <hostname>.local

# 3. Змініть hostname на кожному Pi (щоб не конфліктували):
sudo raspi-config  # → System Options → Hostname
# Наприклад: jtzero-1, jtzero-2, jtzero-3

# 4. Після зміни hostname:
sudo reboot
ssh pi@jtzero-1.local  # кожен Pi має унікальне ім'я
```

## Оновлення після нових змін від Claude

```bash
# Нові покращення від Claude з'являються в main гілці
# https://github.com/iigar/JT_Zero_Core_Claude

# На Pi:
cd ~/jt-zero
git pull origin main
./update.sh
```
