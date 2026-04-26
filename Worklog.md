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

## Сесія 2026-04-24 (вечір) — EKF3 cycling повністю усунено

### Виконані фікси

| Fix | Проблема | Рішення | Commit |
|-----|----------|---------|--------|
| ODOMETRY-only | #102 + #331 = подвійний fusion → innovation gate вдвічі жорсткіший → cycling | Прибрано VISION_POSITION_ESTIMATE, тільки ODOMETRY | 6a2e25e |
| Yaw-only quaternion | Roll/pitch надсилались назад у FC → feedback loop → horizon нахил при yaw | Quaternion тільки yaw (roll=pitch=0) | 6a2e25e |
| MAVLink pose reset | mavlink.vo_pose_x_ і camera.pose_x_ — два незалежні акумулятори. reset_vo() скидав тільки camera → 36м/582м знову | reset_vo_pose() в MAVLinkInterface, викликається разом з camera_.reset_vo() | 1653b79 |
| Armed-only send | VO дрейфує 45м/3хв на нерухомому дроні (bright=9, scale=1m/400px). EKF3 отримував 45м → cycling | should_send = vo.valid && state.armed | f3cb8e7 |

### Налаштовані параметри ArduPilot
- `EK3_SRC1_VELXY = 0` (було 6) — прибрано velocity fusion
- `EK3_SRC1_YAW = 1` (compass) — підтверджено

### Підтверджений результат
- `pose_x=-0.02m pose_y=-0.19m` після ARM ✅
- Жодного "stopped aiding" cycling ✅
- "JT0: VO RESET ON ARM" в MP Messages ✅

---

## Сесія 2026-04-25 (ніч) — Вібраційна ізоляція + camera sensitivity + Fix #57

### Контекст
LOITER blocker: вібрація мотора → conf=0.09 в польоті. Рішення: foam між камерою і кронштейном.

### Виконані зміни

| Зміна | Деталі | Commit |
|-------|--------|--------|
| Camera env vars | JTZERO_SHUTTER=30000μs, JTZERO_GAIN=3.0 (systemctl set-environment, без rebuild) | — |
| Fix #57 | IMU dead-zone 0.10 м/с² у Kalman predict — усуває x drift від IMU bias | ca0c9ab, 3eea4d7 |

### Результати порівняння ізоляції

| Ізоляція | Stationary drift | Drift з моторами | Conf без моторів | Conf з моторами |
|----------|-----------------|-----------------|-----------------|----------------|
| Без ізоляції | — | ~conf=0.09 (BLOCKER) | — | — |
| М'який пінопласт ~10мм | 0.12м/хв | ~4.7м/хв | 0.54-0.60 | 0.44-0.50 |
| **Пеноплекс (щільний)** | **~0.01м/хв** ✅ | **~0.35м/хв** ✅ | **0.59-0.69** ✅ | **0.22-0.51** ✅ |

Пеноплекс дав **13× покращення drift при моторах**.

### Залишкові проблеми

| Проблема | Деталі |
|----------|--------|
| ~~**bright spike**~~ | ~~bright=2→23→2 → pose +13м~~ — ЗАКРИТО Fix #58 |
| y VO bias | ~0.01м/5с залишковий y drift = нахил камери або floor texture bias — прийнятно для EKF3 |
| **Motor drift 0.23 м/хв** | EKF3 отримує дрейф VO як абсолютну позицію → LOITER коригує → дрон летить в напрямку drift |

### Pi стан (2026-04-25)
- HEAD = 3eea4d7 (очікує rebuild)
- JTZERO_SHUTTER=30000, JTZERO_GAIN=3.0 активні (env var)
- Пеноплекс змонтований

---

## Сесія 2026-04-25 (продовження)

### LOITER тест — результати

| Метрика | Значення |
|---------|----------|
| bright=2 (норма) | тінь від корпусу дрона — ОЖИДУВАНЕ |
| bright spike (00:05:23) | bright 2→23 → pose +13м → LOITER fail |
| Drift при моторах | ~0.23 м/хв (x), ~0.10 м/хв (y) |
| Stationary drift | ~0.10 м/хв (тільки y — floor texture bias) |
| LOITER поведінка | "носить по сторонах" — EKF3 слідує за VO drift |

**Підтверджено:** bright=2 — нормальна умова (тінь дрона на підлозі). Не проблема.

### Fix #58: Bright Spike Filter

**Причина відмови:** bright spike (один кадр) → LK tracker втрачає фічі → хибні вектори → Kalman інтегрує 5с → +13м jump → LOITER fail catastrophically.

**Зміни:**
- `camera_pipeline.cpp:tick()` — brightness обчислюється ДО `vo_.process()`
- EMA rolling average (`bright_rolling_avg_`, alpha=0.1)
- Spike: `brightness > avg * 4.0 && avg >= 1.0` → suppress 45 frames (3с)
- `camera.h` — додано `bright_rolling_avg_`, `spike_suppress_frames_`

**Верифікація для нашого випадку:**
- `bright_rolling_avg_ ≈ 2.0` (тінь), `SPIKE_MIN_AVG=1.0` → активно
- Поріг: `2.0 * 4.0 = 8.0`
- Spike: `23 > 8` → triggered, 3с suppress ✅

**ВАЖЛИВО:** `SPIKE_MIN_AVG=1.0` (не 3.0!) — інакше при avg=2 фільтр вимкнено.

### Наступна задача: Motor drift — LOITER все ще нестабільний

Drift 0.23 м/хв з моторами → EKF3 думає дрон рухається → LOITER коригує → фізичний рух.

**Варіанти:** потрібне обговорення з Ігорем.

---

## Сесія 2026-04-26 — Fix #60 VO velocity bias auto-calibration + push #58/#59

### Задеплоєно

**Fix #58 + #59** (bright/dark spike filters, commits `ad34183`/`5c60c77`) → запушено в `claude` remote. Pi ще потребує `git pull + rebuild`.

### Fix #60 — VO velocity bias auto-calibration в hover

**Проблема:** LOITER Y drift 0.55 м/хв з моторами (0.10 м/хв статика). Причини:
- Камера нахилена ~5° pitch → raw_vy ≠ 0 навіть у hover
- Floor texture bias (флорентійський ламінат) дає аналогічний ефект
- Hover decay `kf_vy_ *= 0.85f` (Fix #53a) безпорадний — VO update `kf_vy_ += K*(raw_vy - kf_vy_)` одразу повертає bias в state

**Рішення (mirror Fix #38 gyro_z_bias model):**
- `vx_bias_`, `vy_bias_` — EMA members у VisualOdometry (persist через `reset()`)
- Hover gate: `is_hovering && hover_duration >= 5s && |raw_v| < 0.5 m/s`
- EMA α=0.005 (~30с settling), subtract ПЕРЕД Kalman: `kf_vx_ += Kx*(raw_vx - vx_bias_ - kf_vx_)`
- STATUSTEXT `"JT0: VO BIAS CALIB X=0.XXX Y=0.XXX"` при стабілізації (Python polling)

**RC ch12 differentiation:**
- disarmed: full reset (pose + bias) → "JT0: FULL CALIB RESET"
- armed: pose only → "JT0: SET HOMEPOINT (in-flight)"

**Файли змінено:**
- `jt-zero/include/jt_zero/camera.h` — private `vx_bias_/vy_bias_`, public accessors/clear, CameraPipelineStats + CameraPipeline wrappers
- `jt-zero/camera/camera_pipeline.cpp` — Phase 2 subtract, Phase 5 EMA, get_stats() fill
- `jt-zero/api/python_bindings.cpp` — `vx_bias`/`vy_bias` у camera dict
- `jt-zero/core/runtime.cpp` — команда `"clear_bias"`
- `backend/native_bridge.py` — `_vo_bias_tick()`, `_check_rc_vo_reset()` differentiation
- `backend/server.py` — VO Monitor лог + vx_bias/vy_bias

---

## Сесія 2026-04-27 — Fix #61: bias calibration в реальному LOITER польоті

### Аналіз логів Fix #60

З логів `uvicorn[976]` та `uvicorn[1409]` (тест 2026-04-26):
- `vx_bias=0.000 vy_bias=0.000` весь час — Fix #60 **ніколи не спрацював**
- Причина: `hover_.is_hovering` потребує `micro_motion_avg < 0.5px` за 30+ кадрів
- LOITER контролер постійно коригує → VO бачить 1-3px → hover НІКОЛИ не детектується в польоті
- На темній підлозі (bright=1-2): hover детектується, але `raw_vx≈0` (LK нічого не трекує)
  → bias "калібрується" до 0, знищуючи будь-яку попередню калібровку

Позитивне: Y drift в **темряві = 0** (pose заморожений hover decay) ✓ — Fix #57 deadzone працює.

### Fix #61 — двошаровий bias EMA

**Проблема Fix #60:** single hover gate несумісна з LOITER flight.

**Рішення:** два шляхи, обидва вимагають `frame_brightness_ >= BIAS_MIN_BRIGHTNESS (=4)`:
- **Fast path** (α=0.005, ~30с): hover підтверджений + brightness OK (попередня поведінка, але з brightness gate)
- **Slow path** (α=0.001, ~67с): будь-який valid VO + brightness OK → спрацьовує під час LOITER

`BIAS_MIN_BRIGHTNESS=4`: темна підлога bright=1-2 < 4 → OFF (bias не псується), польот bright=5-20 ≥ 4 → ON.

Settling time slow path: 1/0.001 × (1/15fps) ≈ 67с → за 1-2хв LOITER vy_bias ≈ 80% реального значення.

**Файли змінено:**
- `jt-zero/include/jt_zero/camera.h` — додано `VEL_BIAS_ALPHA_SLOW=0.001f`, `BIAS_MIN_BRIGHTNESS=4`
- `jt-zero/camera/camera_pipeline.cpp:843-868` — двошаровий блок замість single hover gate

**Очікуємо в наступному тесті:**
- `vx_bias` і `vy_bias` починають накопичуватись в перші 30с польоту (bright≥4)
- За ~2хв LOITER: STATUSTEXT `"JT0: VO BIAS CALIB X=... Y=..."` в Mission Planner
- Y drift в LOITER значно менший (< 0.1 м/хв замість 0.55)

---

## Відкриті задачі

| Пріоритет | Задача |
|-----------|--------|
| **NEXT** | Pi rebuild + тест Fix #61. Команди на Pi: `cd ~/jt-zero && git pull && cd jt-zero/build && make -j4 && cp jtzero_native*.so ~/jt-zero/backend/ && sudo systemctl restart jtzero` |
| **NEXT** | Verify: в польоті `journalctl -u jtzero \| grep "vx_bias"` — має показати ненульове значення після 30-60с |
| **HIGH** | Очікуємо STATUSTEXT `"JT0: VO BIAS CALIB"` за ~2хв LOITER |
| MED | Repo hygiene: прибрати `*.so`, `jt-zero/build/` з git tracking |
| LOW | Pi deploy: скинути пароль після нового salt |
| ~~HIGH~~ | ~~Bright spike → 13м~~ — ЗАКРИТО Fix #58 |
| ~~BLOCKER~~ | ~~Вібраційна ізоляція~~ — ЗАКРИТО (пеноплекс: 13× покращення) |
| ~~HIGH~~ | ~~EKF3 cycling~~ — ЗАКРИТО |
| ~~HIGH~~ | ~~Horizon нахил при yaw~~ — ЗАКРИТО |
| ~~HIGH~~ | ~~C++ thread safety~~ — ЗАКРИТО |
| ~~HIGH~~ | ~~AGC instability~~ — ЗАКРИТО |
| ~~HIGH~~ | ~~imu_consistency в confidence~~ — ЗАКРИТО |
| ~~HIGH~~ | ~~ground_dist поріг~~ — ЗАКРИТО |
