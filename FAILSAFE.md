# JT-Zero — Failsafe Reference

> **Для оператора в полі.** Коротко: що відбувається, що видно в Mission Planner, що робити.

---

## 1. GPS Jammed / GPS Lost — VO Working

**Що відбувається:**
- ArduPilot EKF3 переходить на ExternalNav (JT-Zero VO) як основне джерело позиції
- VISION_POSITION_ESTIMATE продовжує надходити @ 25Hz
- Position_uncertainty зростає з часом (~±5m за 10хв польоту)

**Що видно в Mission Planner:**
- GPS статус: No Fix
- Через ~30с: `JT0: GPS DEGRADED unc=Xm VO only` (WARNING)
- Через ~хвилину активного польоту: `JT0: GPS LOST unc=Xm RTL ADVISED` (CRITICAL)

**Що робити:**
- Якщо unc < 6m і VO valid → можна продовжувати місію обережно
- Якщо unc > 8m → RTL або посадка в поточній точці
- Режим HOLD → зависання, uncertainty не росте, є час прийняти рішення

**Обмеження:**
- VO дрейфує ~±5m за 10хв на відкритій місцевості
- В лісі/міській забудові — краще (більше фіч), в пустелі/воді — гірше (менше фіч)

---

## 2. GPS Jammed + VO Lost (подвійна відмова)

**Причини:** темрява + хмарність, або різкий маневр, або CSI закрита.

**Що відбувається:**
- VO invalid → MAVLink припиняє надсилати VISION_POSITION_ESTIMATE (Fix #46)
- ArduPilot EKF3 через ~2с таймаут переходить на IMU + Baro only
- Горизонтальне утримання позиції втрачено, висота по барометру ОК

**Що видно в Mission Planner:**
- `JT0: VO FALLBACK ACTIVE` (якщо є thermal і яскравість < 20) — система намагається переключитись на thermal камеру
- EKF повідомлення: "ExternalNav: stopped aiding"
- Якщо thermal теж не допомагає — VO залишається invalid

**Що робити:**
- Негайний RTL або ручне управління
- **Не покладатись на автономне утримання позиції** — є тільки висота
- Посадка з ручним контролем або по GPS якщо джамінг тимчасовий

---

## 3. VO Fallback Active (CSI → Thermal Camera)

**Тригер:** середня яскравість CSI < 20 протягом 0.8с (темрява, туман, перекриття).

**Що відбувається:**
- C++ переключається на USB thermal як джерело VO
- Python ін'єктує кадри з thermal @ ~5fps (нижче за 15fps CSI)
- VO продовжує працювати, але: менша точність, нижча частота, вужчий FOV

**Що видно в Mission Planner:**
- `JT0: VO FALLBACK ACTIVE` (WARNING severity=4)
- ThermalPanel в Dashboard: помаранчеві квадрати фіч + статистика

**Обмеження в Fallback:**
- ~5fps замість 15fps → LK hints від IMU критичні
- FOV thermal ~25-50° замість 62° CSI → менше фіч при швидкому русі
- Очікувана точність нижча ~2-3x порівняно з CSI

**Відновлення:**
- Автоматичне при brightness >= 30 АБО confidence >= 0.20
- `JT0: VO CSI RECOVERED` при поверненні на CSI

---

## 4. Python Bridge Crash

**Що відбувається:**
- C++ ядро продовжує працювати незалежно
- MAVLink (VISION_POSITION_ESTIMATE @ 25Hz) продовжує надходити до FC
- Якщо crash під час VO Fallback: thermal ін'єкція зупиняється → VO invalid (бо немає нових кадрів)
- systemd перезапускає сервіс через 5с (Restart=always)

**Що видно в Mission Planner:**
- `JT0: MONITOR ERR — VO/GPS WATCH DEGRADED` (ERROR severity=3)
- Dashboard недоступний до перезапуску (~5-10с)

**Що робити:**
- Dashboard повернеться автоматично через ~10с
- Якщо crash стався під час VO Fallback → очікуй що VO буде invalid до перезапуску
- В польоті: HOLD або RTL поки сервіс не підніметься

---

## 5. Pi Zero Thermal Throttling

**Температури:**
- < 70°C — нормально
- 70-80°C — throttling починається, VO FPS може впасти
- > 80°C — важкий throttling, VO може стати ненадійним

**Тригери:** спекотна погода + корпус без вентиляції + інтенсивний VO

**Що видно:**
- CPU % в Dashboard зростає > 70% при throttling
- VO FPS падає нижче 10fps (норма 15fps)

**Що робити:**
- CPU alert в Dashboard при > 70% — знак проблеми
- При довгих місіях в спеку: перевір температуру корпусу
- Поточні ліміти: CPU ≤ 55% норма, alert при 70%

---

## 6. MAVLink Disconnect (Pi ↔ FC)

**Що відбувається:**
- FC більше не отримує VISION_POSITION_ESTIMATE → EKF3 таймаут
- ArduPilot переходить на GPS + IMU (або IMU-only якщо GPS jammed)
- JT-Zero продовжує роботу, спробує перепідключитись автоматично

**Що видно в Mission Planner:**
- MAVLink статус в Dashboard: DISCONNECTED
- EKF: "ExternalNav: stopped aiding"

**Причини:**
- Дефектний UART кабель (найчастіша причина на польоті — вібрація)
- Неправильний baud rate (JT-Zero auto-detect, але може не знайти)
- FC перезавантажився

**Що робити:**
- JT-Zero спробує переперепідключитись автоматично
- Якщо немає GPS — одразу RTL або посадка

---

## Швидка таблиця рішень

| Ситуація | VO | GPS | Дія |
|----------|----|----|-----|
| Норма | ✅ | ✅ | — |
| GPS jammed | ✅ | ❌ | Продовжуй, стеж за unc |
| GPS jammed, unc > 8m | ✅ | ❌ | RTL або посадка |
| Темрява, Fallback | ⚠️ thermal | ❌ | Обережно, знижена точність |
| VO + GPS обидва втрачені | ❌ | ❌ | Негайний ручний контроль / посадка |
| Pi throttling | ⚠️ | будь-який | Стеж за FPS, знизь висоту |
| Dashboard недоступний | невідомо | — | Чекай 10с, якщо не повернувся — перевір сервіс |

---

## Системні ліміти (Pi Zero 2W)

| Параметр | Норма | Alert | Критично |
|----------|-------|-------|----------|
| CPU | ≤ 55% | > 70% | throttling |
| RAM | ≤ 180MB | > 250MB | OOM kill |
| VO FPS | 15fps | < 10fps | VO invalid |
| Position uncertainty | < 2m | > 4m | > 8m |
| MAVLink heartbeats | > 0 | 0 за 5с | disconnect |
