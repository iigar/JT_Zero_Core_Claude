# Worklog — JT-Zero Claude Sessions

> Цей файл веде Claude (remote `claude` → `iigar/JT_Zero_Core_Claude`).
> Кожна сесія — окремий розділ з датою, описом змін, причинами та файлами.
> **Читати обов'язково на початку кожної сесії**, щоб розуміти стан проєкту.

---

## Сесія 2026-04-02 — Аудит безпеки бекенду + Frontend дизайн

### 1. Аудит GitHub репо та виправлення 10 проблем безпеки/надійності

**Мета:** провести аудит `origin` гілки `main22`, виправити критичні проблеми.

#### `backend/flight_log.py`

| # | Проблема | Що зроблено | Чому |
|---|----------|-------------|------|
| 1 | Фіксована сіль `SALT = b"jtzero-flight-log-v1"` | Замінено на `_get_or_create_salt()` — 16 байт `secrets.token_bytes()`, зберігається в `config.json` | Фіксована сіль робить PBKDF2 детермінованим — attacker з rainbow table зламає за секунди |
| 2 | PBKDF2 викликався на кожний запис у лог | Ключ виводиться один раз при `start_log`, зберігається в `_key`; `_encrypt_data_with_key()` використовує готовий ключ | Кожен запис займав ~100ms на Pi Zero через 100k ітерацій |
| 3 | `except Exception: return []` при помилці дешифрування | `except InvalidToken: return None`; caller розрізняє "неправильний пароль" vs "порожній файл" | Загальний except ковтав реальні помилки та приховував corruption |
| 4 | Path traversal у `read_session()` | `os.path.realpath()` порівняння з базовою директорією | `../../etc/passwd` в імені сесії давав read за межами дозволеної директорії |
| 5 | `_write_failed` не виставлявся при помилці запису | При IOError: `_running=False`, файл закривається, `_write_failed=True` | Без цього лог продовжував "записувати" в закритий fd мовчки |
| 6 | `psutil` імпортувався безумовно | Module-level `try/except ImportError` → `_PSUTIL_AVAILABLE` flag | `psutil` не встановлений у базовому venv; безумовний імпорт ламає старт |

#### `backend/server.py`

| # | Проблема | Що зроблено | Чому |
|---|----------|-------------|------|
| 7 | CORS `allow_origins=["*"]` | `allow_origins=_ALLOWED_ORIGINS` з env `JTZERO_ALLOWED_ORIGINS` (default: localhost:3000, localhost:8001, jtzero.local:8001); `allow_credentials=False`; методи обмежено до GET/POST | Wildcard CORS + credentials = будь-який сайт може читати телеметрію дрону |
| 8 | Мінімальна довжина пароля 6 символів | Змінено на 12 | 6 символів — брутфорс за хвилини на Pi Zero |
| 9 | WebSocket exceptions мовчки ковталися | `except Exception as e: sys.stderr.write(f"[WS/...] {e}")` | Без логування impossible діагностувати обриви зв'язку |
| 10 | `read_session` повертав `{"error":"Wrong password"}` навіть при порожньому файлі | `if records is None → "Wrong password"`, `if not records → "Corrupted or empty file"` | Оператор бачив одне і те ж повідомлення при двох різних причинах |

#### `backend/native_bridge.py`

| # | Проблема | Що зроблено | Чому |
|---|----------|-------------|------|
| 11 | `import numpy as np` з fallback гілкою | Замінено на `NUMPY_AVAILABLE = False` + видалено всю numpy-гілку | CLAUDE.md правило 5: numpy заборонений на Pi Zero; fallback все одно не використовувався |

**⚠ Breaking change:** новий рандомний salt робить старі `.jtzlog` файли та паролі недійсними.
Після деплою треба скинути пароль через `POST /api/logs/password`.

---

### 2. Git інфраструктура — два remote

**Мета:** відокремити роботу Claude від комітів Emergent AI.

- Додано remote `claude` → `https://github.com/iigar/JT_Zero_Core_Claude.git`
- Remote `origin` → `https://github.com/iigar/JT_Zero_Core.git` (Emergent, не чіпати)
- Перший push з `--force` (GitHub auto-створив README)

**Правило:** Claude пушить ТІЛЬКИ в `claude`. Ніколи в `origin`.

---

### 3. Skill `/push-claude`

**Файл:** `C:/Users/vlase/.claude/skills/push-claude/SKILL.md`

Skill для коміту та пушу змін у `claude` remote. Викликається командою `push-claude` або `/push-claude`.

---

### 4. Frontend дизайн — "Orbital Command / Cyberpunk Terminal / Glass Cockpit"

**Мета:** підсилити візуальну атмосферу дашборду без зміни логіки.

#### `frontend/src/index.css`

| Зміна | Причина |
|-------|---------|
| CSS custom properties (`--cyan`, `--void`, `--cyan-glow` і т.д.) | Централізувати кольори, уникнути дублювання hex по всьому CSS |
| `.hud-lines` — сітка 48×48px з rgba(0,240,255,0.025) | Cockpit HUD texture без перевантаження темного фону |
| `.sweep-overlay::after` — анімований сканер 12s цикл | Cyberpunk "radar sweep" — система виглядає живою і активною |
| `.panel-glass` — переписано: `overflow:hidden` + gradient sheen | `overflow:hidden` фіксує corner decorations; sheen дає скляний ефект |
| `.readout` — Share Tech Mono + tabular-nums | Числові значення (°C, V, Гц) виглядають як авіоніка |
| `.cursor-blink::after` — термінальний курсор ▋ | Термінальна естетика для заголовків |
| `.status-live::after` — пульсуюче кільце | "Живий" сигнал потребує анімованого feedback |
| `.emergency-flash` — мигання фону при EMERGENCY | Критичні стани мають захоплювати увагу периферійним зором |
| `.battery-bar` / `.battery-bar-fill` — горизонтальний meter | Вертикальна смужка 1.5×3px не передавала рівень; fill-width читається миттєво |
| `.panel-accent-top::after` — cyan лінія по верху панелі | Mil-spec стиль приладових панелей |
| `body::after` — vignette radial-gradient | Фокусує погляд на центрі, приховує краї сітки |
| Google Font: `Share Tech Mono` import | Шрифт для числових readout-значень |

#### `frontend/src/components/Header.js`

| Зміна | Причина |
|-------|---------|
| Висота фіксована `42px` через style | `py-2.5` давав непостійну висоту; фіксована гарантує стабільний layout |
| Верхня accent лінія (gradient fade cyan) | Відокремлює header від фону без жорсткого border |
| Логотип "JT-ZERO" на Share Tech Mono + textShadow glow | JetBrains Mono виглядав як звичайний UI; Share Tech Mono + glow = позивний |
| Zap іконка: `drop-shadow` filter (не box-shadow) | box-shadow не працює на SVG; drop-shadow огортає форму іконки |
| Горизонтальний battery-bar + fill + окремий рядок V/% | Оператор має бачити заряд одним поглядом; fill-width + колір читається без цифр |
| `BATTERY_COLOR` helper: text/fill/glow для кожного стану | 3 різних CSS-властивості потребують різних форматів кольору |
| `emergency-flash` на `<header>` при EMERGENCY | Весь header мигає — неможливо пропустити |

#### `frontend/src/App.js`

| Зміна | Причина |
|-------|---------|
| `hud-lines sweep-overlay` на root div | Підключити фонову сітку та sweep до всього екрану |
| `textShadow` glow на активному табі | Підкреслити активний таб без зміни layout |

---

---

## Сесія 2026-04-03 — VO+IMU fusion: 6 покращень

### Файли змінено
- [jt-zero/include/jt_zero/camera.h](jt-zero/include/jt_zero/camera.h)
- [jt-zero/camera/camera_pipeline.cpp](jt-zero/camera/camera_pipeline.cpp)
- [jt-zero/core/runtime.cpp](jt-zero/core/runtime.cpp)

### Fix 1 — imu_consistency: правильне ΔV порівняння

**Проблема:** `actual_dvx = raw_vx - kf_vx_` — це post-update Kalman residual, не ΔV.
Порівнювати з `imu_ax * dt` (яке є ΔV) математично некоректно.

**Рішення:** `kf_vx_prev_` / `kf_vy_prev_` зберігаються ДО predict-кроку:
```
ΔV_VO  = raw_vx - kf_vx_prev_  (зміна швидкості по VO за кадр)
ΔV_IMU = imu_ax * dt            (очікувана зміна по IMU)
```
Scaling: `0.5` → `5.0` (при 1 m/s² розбіжності за 66ms delta = 0.066 m/s).

### Fix 2 — Complementary filter для roll/pitch/yaw

**Проблема:** roll/pitch тільки з акселерометра. `yaw += gyro_z * dt` — голий інтеграл.

**Рішення (sensor_loop, static locals, T1-only):**
`cf_roll = 0.98 * (cf_roll + gx*dt) + 0.02 * atan2(ay, az)`
Yaw: `(gz - gyro_z_bias) * dt`. Bias: EMA при `!armed && |gyro| < 0.05 rad/s`,
BIAS_ALPHA=0.0005 (~30s settling).

### Fix 3 — Gyro bias estimation в hover

**Рішення:** при `is_hovering` і `|gyro_z| < 0.3 rad/s`:
`hover_.gyro_z_bias += (gyro_z - bias) * 0.005`. Bias додано до HoverState.
drift_rate у hover тепер: `rate_from_optical_flow - gyro_z_bias`.

### Fix 4 — IMU prediction step у Kalman

**Проблема:** predict = тільки `P += Q`. State відставав при прискореннях.
**Додатково:** `set_imu_hint()` ніколи не викликався з runtime → imu_hint_valid_ = false завжди.

**Рішення:** `kf_vx_ += imu_ax_ * dt` перед Kalman update (якщо hint valid).
`camera_.set_imu_hint(acc_x, acc_y, gyro_z)` додано в `camera_loop()` ДО `tick()`.

### Fix 5 — position_uncertainty з KF covariance

**Проблема:** `uncertainty = distance * 0.03 * (1 - conf*0.5)` — ad hoc формула.

**Рішення:** `pose_var_x_ += kf_vx_var_ * dt²` → `uncertainty = sqrt(pose_var_x + pose_var_y)`.
Decay ×0.995/кадр при confidence > 0.7; growth ×4 при dead-reckoning.

### Fix 6 — IMU pre-integration для LK initial-flow hints

**Проблема:** при ±10° повороті між кадрами (66ms) LK шукає фічі в старих позиціях → track loss.

**Архітектура:**
- `PreIntState {dgx, dgy, dgz}` + `std::mutex preint_mtx_` в VisualOdometry
- T1 (200Hz): `camera_.accumulate_gyro(gx, gy, gz-bias, dt)` → mutex-protected
- T6: `shift_x = focal * dgz`, `shift_y = -focal * dgy` → `hint_dx[]`, `hint_dy[]`
- LKTracker::track: нові nullable `const float* hint_dx, hint_dy`; `flow_x = hint ? hint[f] : 0`
- CameraPipeline: нові публічні методи `set_imu_hint()` і `accumulate_gyro()`

---

---

## Сесія 2026-04-22/23 — Mobile UI breakpoint + Fix #54 camera exposure

### 1. Mobile UI — Variant A завершено

Замінено `sm:` → `lg:` у 3 файлах (breakpoint 640px → 1024px).
Тепер будь-який екран < 1024px отримує mobile layout (icons only, stack, без sidebar).
Landscape телефон більше не показує desktop UI.

- `frontend/src/App.js` — всі sm: → lg:
- `frontend/src/components/Header.js` — всі sm: → lg:
- `frontend/src/index.css` — @media max-width 639px → 1023px
- Commit: `cd0ed47`, CI збілдив автоматично

### 2. Fix #54 — rpicam-vid фіксована витримка + фіксований gain (Bug Fix #54 в CLAUDE.md)

**Проблема 1:** auto-exposure → bright=181 при яскравому освітленні → conf=0.00.
**Проблема 2 (нова):** auto gain → frame-to-frame pixel intensity зміни → LK gradient mismatch → conf нестабільний.
Підтверджено на Pi: bright 17→70 (фонарик ON) → conf 0.17→0.13; bright 74→17 (OFF) → conf стрибок 0.39.
Також: AGC викликав повільний дрейф conf 0.27→0.17 за 4хв нерухомості.

**Рішення фінальне:** `--shutter 8000 --gain 1.0` (8ms + unity gain).
- Фіксована витримка → нема motion blur, нема AEC
- Фіксований gain → pixel intensities стабільні між кадрами → LK коректний
- Тест: bright=1 над паркетом, conf=**61%**, INL=127/180 — стабільно
- gain=1.0 (нативна чутливість) — для денного польоту достатньо; ніч = thermal fallback

**Файл:** `jt-zero/camera/camera_drivers.cpp:130`
**Commits:** `4c369cb`, `0959f58`, `b2733fc`

### 3. Підтверджений результат (2026-04-24)

- Дрон на підлозі над паркетом у кімнаті, нерухомо, камера вниз
- bright=1 (gain=1.0 без підсилення, нічна кімната), але conf=**61%**, INL=127
- Conf стабільний ±0.02 протягом 12+ хвилин (до: дрейф 0.27→0.17)
- Зміна освітлення (фонарик): conf змінюється лише 0.30-0.35 (до: 0.13-0.39 стрибки)
- Паркет з деревним зерном — ідеальна поверхня для VO: FAST знаходить 180 фіч

---

## Сесія 2026-04-24 (ніч) — LOITER indoor тести + серія фіксів

### Виконані фікси (всі задеплоєні)

| Fix | Проблема | Рішення | Commit |
|-----|----------|---------|--------|
| #54 final | Auto gain (AGC) транзієнти між кадрами → conf 0.17→0.13 при фонарику | `--gain 1.0` (unity, fixed) | b2733fc |
| #55a | vo_valid per-frame flip → EKF3 дрейф при короткочасних conf дипах | Debounce: false після 5 кадрів (333ms), true після 2 | dada015 |
| #55b | Поріг 0.32 вимикав VO весь час польоту | 0.32 → 0.15 | 15f32ff |
| #55c | imu_consistency флорував на 0.1 під час руху → conf=0.11 | Прибрати з raw_confidence формули | 91260ad |
| #56 | ground_dist=1.0м при 500мм польоті → scale 2× → осциляція | Поріг 2.0→0.1м (baro при 500мм = 0.5м, точніше) | 6c5563a |

### Результати conf після фіксів

| Стан | До | Після |
|------|-----|-------|
| Нерухомість | 0.27→0.17 (дрейф) | 0.51-0.64 (стабільно) |
| Активний польот | 0.11-0.13 (нижче порогу) | 0.20-0.54 (vo_valid=true) |

### Залишилась проблема: LOITER осциляція

**Симптом:** "сильно несе то в один бік, то геть в інший" + висота нестабільна.

**Діагноз:**
1. **VISO_DELAY_MS не встановлено** (найймовірніша причина) — EKF3 думає VO дані поточні, але вони на ~100мс старі (Pi Zero обробка). Це викликає систематичне неузгодження швидкостей IMU↔VO → EKF3 рахує позицію неправильно → LOITER коригує у неправильний бік → осциляція.
2. **Накопичена позиція під час Stabilize** — при переключенні у LOITER pose_x_, pose_y_ вже мають похибку від фази підйому. Workaround: SET HOMEPOINT перед LOITER.
3. **Висота** — EK3_SRC1_POSZ=1 (баро). Indoor баро шумний від prop wash. Без rangefinder нормально.

**ArduPilot параметри для налаштування:**
```
VISO_DELAY_MS = 100   ← КРИТИЧНО, зараз напевно 0 (default)
PSC_POSXY_P   = 0.5   ← зменшити з дефолтного для плавнішої корекції
LOIT_SPEED    = 500   ← зменшити (5 м/с)
```

**Workaround до параметрів:** SET HOMEPOINT одразу перед переключенням у LOITER.

**Наступна діагностична правка (не зроблено):** додати pose_x_, pose_y_ у VO Monitor рядок — щоб бачити чи позиція дрейфує під час польоту.

---

## Сесія 2026-04-24 (день) — EKF3 cycling fix + RC ch12 + VO Monitor pose

### Виконані задачі

| Fix | Проблема | Рішення | Commit |
|-----|----------|---------|--------|
| pose_x/y у VO Monitor | Не було видно чи позиція дрейфує | Додано `pose_x=X.Xm pose_y=X.Xm` у лог кожні 5с | b753bb0 |
| SET HOMEPOINT ch8→ch12 | ch8 зайнятий, вільний ch12 | `_rc_reset_channel = 11` (0-based) | ce4f38b |
| EKF3 cycling (582м) | pose_x_ накопичується між польотами → EKF3 отримує 582м → innovation fail → цикл | Auto-reset pose при ARM (disarmed→armed edge) + STATUSTEXT "JT0: VO RESET ON ARM" | 45162a1 |

### Налаштовані параметри ArduPilot (через Mission Planner)
- `VISO_DELAY_MS = 100` (було 80)
- `PSC_POSXY_P = 0.5`
- `LOIT_SPEED = 500`

### Git workflow уточнення
Pi тягне з `origin` = `JT_Zero_Core_Claude` (мій репо). `claude` remote — дублікат. Workflow: пушу в `claude` remote → Pi робить `git pull` (з origin = той самий репо) → отримує зміни.

---

## Відкриті задачі

| Пріоритет | Задача |
|-----------|--------|
| **NEXT** | Тест LOITER після всіх фіксів (VISO_DELAY_MS=100, PSC=0.5, auto-reset on ARM) |
| HIGH | IMU preint std::mutex в T1 @ 200Hz (hot path) |
| MED | Repo hygiene: прибрати `*.so`, `jt-zero/build/` з git tracking |
| LOW | Pi deploy: скинути пароль після нового salt |
| ~~NEXT~~ | ~~VISO_DELAY_MS=100~~ — ЗАКРИТО (встановлено 100, було 80) |
| ~~NEXT~~ | ~~pose_x/y у VO Monitor~~ — ЗАКРИТО |
| ~~HIGH~~ | ~~C++ thread safety~~ — ЗАКРИТО |
| ~~HIGH~~ | ~~AGC instability~~ — ЗАКРИТО (gain=1.0) |
| ~~HIGH~~ | ~~imu_consistency в confidence~~ — ЗАКРИТО |
| ~~HIGH~~ | ~~ground_dist поріг~~ — ЗАКРИТО (0.1м) |
| ~~HIGH~~ | ~~EKF3 cycling (582м)~~ — ЗАКРИТО (auto-reset on ARM) |
