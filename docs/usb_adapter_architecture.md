# USB Adapter Emulation — Architecture

Цель: ESP32-S3 прикидывается разными USB-адаптерами, не требуя подмены DLL
на хосте и не добавляя физических проводов. Один физический "порт" на плате
динамически меняет назначение пинов/шин в зависимости от выбранного режима.

## Модели эмуляции (приоритет по ценности для проекта)

| Адаптер | VID:PID | Транспорт | Для чего | Реализация |
|---|---|---|---|---|
| **CP2112** | `10C4:EA90` | USB HID (In/Out reports) | DJI Battery Killer, bqStudio-lite, любой CP2112 tool | Phase 1 — сейчас |
| **EV2300** | `0451:BEF3` | USB bulk vendor-specific | UBRT-2300, TI bqStudio, bq80xusb.dll | Phase 2 — план |
| **FTDI MPSSE (FT232H)** | `0403:6014` | USB bulk vendor | Universal I2C/SPI/JTAG/UART debugger | Phase 3 — опционально |
| **USB2TTL (уже есть)** | — (CDC) | USB CDC | ELRS/Betaflight/UART устройства | работает |
| **USB MSC (SD-карта)** | — (MSC) | Mass Storage | SD как флешка | быстрая win |

## Composite USB Device

ESP32-S3 TinyUSB поддерживает до 16 интерфейсов одновременно. Собираем:

```
┌──────────────── ESP32-S3 USB Device ────────────────┐
│ IAD #0: CDC (2 interfaces)   — USB Serial (flashing, logs, serial bridge) │
│ IAD #1: HID (1 interface)    — CP2112 HID (SMBus tools)                    │
│ IAD #2: Vendor (1 interface) — EV2300 bulk (phase 2)                       │
│ IAD #3: MSC (1 interface)    — SD card as flash drive (phase 3)            │
└────────────────────────────────────────────────────┘
```

Windows одновременно видит **COM5 + HID-CP2112 + USB-Storage**. Каждый
работает независимо. Это стандартный паттерн USB composite device
(Interface Association Descriptor).

## Режим работы → переключение на лету

### Текущие режимы (старые + новые)

| Mode | USB desc | Pins | Назначение |
|---|---|---|---|
| `CDC_ONLY` | CDC | — | Базовый (flash + отладка через COM) |
| `USB2TTL` | CDC | GPIO 43/44 UART | Прозрачный USB↔UART для ELRS/FC |
| `CP2112_HID` | CDC+HID | GPIO 11/10 I2C | DJI battery tools читают через Silicon Labs driver |
| `EV2300_VND` | CDC+Vendor | GPIO 11/10 I2C | UBRT-2300 через штатный TI driver |
| `CP2112+EV2300` | CDC+HID+Vendor | GPIO 11/10 I2C | **Оба режима разом** (разные tools — один порт) |
| `BRIDGE_SERIAL` | CDC | GPIO 11/10 I2C | Наш кастомный binary protocol (shim DLL, текущий USB2SMBus) |
| `BATTERY_LOCAL` | CDC | GPIO 11/10 I2C | Battery Tool на плате (текущее Battery Tool приложение) |
| `MSC_SD` | CDC+MSC | — | SD card как USB flash |
| `CRSF` | CDC | GPIO 43/44 UART | CRSF телеметрия |

### Ключ: USB descriptor выбирается на **boot**

USB device descriptor фиксируется при enumeration и не меняется без
re-enumerate. Поэтому:
- Выбор режима сохраняется в NVS (`Preferences`)
- Переключение из меню → `ESP.restart()` → загрузка с новым дескриптором
- Быстрое переключение между "под-режимами" (например CP2112 vs Battery Tool
  — у обоих одинаковый USB desc **CDC only** или **CDC+HID**) идёт без ребута

### Группировка по USB descriptor:

```
CDC_ONLY:         USB2TTL, CRSF, BRIDGE_SERIAL, BATTERY_LOCAL
CDC+HID:          CP2112_HID          ← включает все CDC_ONLY
CDC+VND:          EV2300_VND          ← включает все CDC_ONLY
CDC+HID+VND:      CP2112+EV2300       ← включает всё
CDC+MSC:          MSC_SD
CDC+HID+VND+MSC:  ALL                  ← "режим разработчика"
```

Фактически 4-5 вариантов USB descriptor, внутри них ≥10 под-режимов
с переключаемыми пинами. Это решает и "composite" и "pin reuse".

## Пин-менеджер (run-time reassignment)

```cpp
class PinPort {
    // Один абстрактный "порт" = пара GPIO.
    // Может быть одновременно в одном режиме.
    enum Mode { IDLE, UART, I2C_MASTER, SPI_MASTER, DIGITAL_IO };
    bool acquire(Mode m, const char* owner);
    void release();
};

extern PinPort portA;   // GPIO 43 + 44 (+3) — текущие UART/ELRS пины
extern PinPort portB;   // GPIO 11 + 10     — текущие battery I2C пины
extern PinPort portC;   // GPIO 48 + 47     — QMI8658 IMU (onboard)
```

Режим прикладного уровня (APP_*) запрашивает портам нужный режим. Менеджер
вызывает `Wire1.end()`, `Serial2.end()` перед сменой, потом `.begin()` с новым
назначением. Те же физические провода, разное поведение.

### Пример

USB2TTL → CRSF: оба на Port A (UART 43/44), но USB2TTL forwardит в USB CDC,
CRSF парсит пакеты локально. Разница — только в приложении, hardware
идентичен.

USB2TTL → CP2112: Port A меняет роль с UART на I2C. Но CP2112 эмулятор
использует Port B (там где battery подключена). Port A в это время может
работать как UART параллельно. Оба активны.

## Приоритет реализации

### Phase 1 (ЭТОТ sprint): CP2112 HID
- `platformio.ini` + TinyUSB HID
- HID report descriptor (CP2112 per AN495)
- Report handlers: 0x10/0x11/0x12/0x17 → Wire1 I2C
- Композитный CDC + HID (оба работают)
- Меню: выбор USB descriptor mode → сохранить в NVS → перезагрузка

### Phase 2: EV2300 vendor-bulk
- Требует реверс wire-протокола из `bq80xusb.dll` (он в `hardware/UBRT_setup/`
  мы его уже извлекли)
- EV2300 use bulk IN/OUT с кастомными packet формами (SMBus, HDQ)
- Драйвер на Windows штатный от TI → нужно имитировать байты 1:1
- Сложнее CP2112 но даёт UBRT-2300 без крэков

### Phase 3: SD MSC
- Arduino-ESP32 имеет `USB_MSC` готовый
- Expose /SD на карте как USB flash drive
- Полезно для логов / подтверждения работы

### Phase 4 (опционально): FTDI FT232H MPSSE
- Универсальный debugger
- Сложно — MPSSE это state machine с командами

## Как это всё уложить в меню + web

### Меню ESP32 (LCD)
```
1. USB2TTL            [short press: launch]
2. USB2SMBus          [submenu]
   ├── Battery Tool (local)
   ├── Bridge (serial)           ← текущий shim DLL режим
   └── CP2112 HID native         ← NEW, phase 1
   └── EV2300 native             ← NEW, phase 2
3. Servo Tester
4. Motor DShot
5. Battery Tool
6. WiFi / Web
7. CRSF Telem
8. USB Mode (CDC/HID/MSC/...)   [NEW: boot descriptor]
```

### Web UI (`/api/usb/mode`)
- GET `/api/usb/mode` → текущий, available, needs_reboot
- POST `/api/usb/mode` `{mode: "CP2112_HID"}` → сохраняет в NVS, возвращает
  "reboot to apply"
- GET `/api/usb/pins` → текущие роли портов
- POST `/api/usb/pins` → reassign без ребута (в пределах текущего USB desc)

## Что сохраняется в NVS

```
pref.usb_descriptor = 0/1/2/3/4   (CDC, CDC+HID, CDC+VND, CDC+HID+VND, CDC+MSC)
pref.boot_app = 0..APP_COUNT       (какой режим запускать автоматически)
pref.port_a_mode = UART|I2C|SPI|GPIO (сохранённый последний выбор)
pref.port_b_mode = ...
```

## Риски / неизвестности

1. **TinyUSB composite с 3+ IADs** — на ESP32-S3 работает, но descriptor size
   приблизится к лимиту USB (255 байт). HID report descriptor CP2112 ~200 байт.
2. **EV2300 protocol** — closed TI, нужна реверс-работа bq80xusb.dll
   (уже есть в `hardware/UBRT_setup/ev2300_real/`). Оценка: ещё 3-4 часа.
3. **Arduino-ESP32 vs ESP-IDF** — Arduino с ограниченным HID API, может
   потребоваться частично перейти на ESP-IDF для HID report-descriptor
   кастомизации. Или использовать библиотеку `ESP32-TinyUSB`.

## Следующие шаги

1. Сейчас: обновить platformio.ini, подключить TinyUSB
2. Реализовать скелет CP2112 HID: descriptor + минимальные Get/Set Feature
3. Хост-тест: Windows увидит как Silicon Labs CP2112 без драйверов shim
4. Transfer reports → Wire1 I2C handlers
5. Меню + web API для USB mode selection

---

## TODO (long-term, независимые фичи)

### OTA обновления через GitHub Releases
- Идея: релизы проекта лежат на GitHub (`.bin` артефакт из `pio run`)
- На ESP32 модуль "OTA Updater":
  - Проверяет `GET api.github.com/repos/<owner>/<repo>/releases/latest`
  - Сравнивает версию с локальной (compile-time `FW_VERSION`)
  - Если новее — качает `firmware.bin` из assets
  - Пишет через `Update.h` во второй partition, reboot
- Partition scheme: `default_16MB.csv` уже имеет `ota_0` и `ota_1` → два
  слота для OTA, rollback на failed boot
- Web UI: кнопка "Check updates" + progress bar, уведомление на LCD
- Безопасность: подпись ECDSA (опц.), HTTPS проверка cert
- CI: GitHub Actions собирает release на tag push, прикрепляет `.bin`

Оценка: 3-4 часа (CI + ESP OTA module + UI).

### Другие идеи (не приоритет)
- Telegram бот для алертов на батарее/дроне
- MQTT публикация telemetry
- INA219 current sensor (для точного charge/discharge track)
- Поддержка BLE (DJI дроны часто имеют BLE)
- RNDIS/ECM для ESP32 как USB-LAN gateway
