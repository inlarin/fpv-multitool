# FPV MultiTool

Многофункциональный тестер для FPV-дронов и **research-платформа для DJI/Autel smart batteries**. Работает на двух разных платах ESP32-S3, сохраняя одну прошивку, один веб-UI, и общий feature set.

![FPV MultiTool — плата в сборе](hardware/photo-front.jpg)

## Две платы, одна прошивка

| | **Waveshare ESP32-S3-LCD-1.47B** | **WT32-SC01 Plus** |
|---|---|---|
| Дисплей | ST7789 172×320 SPI | ST7796 320×480 i80 8-bit |
| Touch | — (status-only LCD + IMU auto-rotate) | FT6336 ёмкостной |
| Локальный UI | Web-only (LCD показывает IP/статус) | LVGL springboard на тачскрине + web |
| SD card | onboard SDMMC | onboard SDMMC |
| Питание | LiPo onboard + USB-C | внешнее (5V) или USB-C |
| Корпус | компактный (карманный) | панельный (стационарный) |
| Env | `esp32s3` | `wt32_sc01_plus` |

Всё что shared (battery / servo / motor / CRSF / ELRS / web) работает одинаково на обеих. Board-specific только UI слой и пинаут. Подробности в [docs/dev/PARALLEL_BOARDS.md](docs/dev/PARALLEL_BOARDS.md).

**GitHub Releases собирают обе прошивки автоматически** через Actions workflow — каждый тег `v*` генерирует `firmware-esp32s3.bin` и `firmware-wt32_sc01_plus.bin`. Плата сама знает, какой asset тянуть, через `/api/ota/check`.

## Что умеет

### FPV Tools
- **Servo Tester** — PWM 500–2500 μs, 50/330 Hz, manual / center / sweep
- **Motor Tester** — ESC по DShot150/300/600 (RMT), arm/disarm, beep, направление, 3D-режим, ограничитель газа
- **ELRS Flasher** — `.bin` / `.bin.gz` / `.elrs` через web, gzip-распаковка на ESP, прошивка через ROM-bootloader приёмника. **Sticky DFU session** + **esptool stub uploader** для 8 chip families (ESP8266 / ESP32 / C3 / S2 / S3 / C6 / H2 / C2) с post-flash MD5 verify
- **CRSF / ELRS telemetry** — live RSSI/LQ/SNR, 16 каналов, FC-телеметрия, полное дерево параметров приёмника (type-aware write, COMMAND-lifecycle как в elrsv3.lua). Auto-pause на 19 endpoints когда battery / servo / motor стартуют
- **RC Sniffer** — SBUS / iBus / PPM / CRSF frame parser
- **USB2TTL bridge** — прозрачный мост между CDC и UART1 для сторонних утилит

### Battery Lab — auto-detect

Три sub-режима автоматически переключаются по chip type:

- **DJI Battery** — оригинальные DJI Mavic / Mini / Air / Phantom / Spark:
  - Полные статусы: SafetyStatus, PFStatus, OperationStatus, DAStatus1/2, GaugeStatus1-3, CBStatus, ManufacturingStatus
  - Автоопределение модели по DeviceName + chipType
  - Unseal wizard с профилями ключей (TI default, RU_MAV, custom)
  - HMAC-SHA1 challenge-response unseal (для bq40z30x+)
  - MAC Command Runner (58 команд из BQ40z307 каталога: SealDevice, ROMMode, SecurityKeys, CalibrationMode, LifetimeDataReset, BlackBoxRecorderReset и др.)
  - **Data Flash Editor** (100+ BQ9003/BQ40z307 параметров из Killer.ini) с edit/export
  - Service workflow: unseal → clear PF (standard + DJI PF2) → seal
  - На SC01 Plus: отдельный **Service screen** на тачскрине (unseal / clearPF / cycle reset / seal в один тап)

- **DJI Clone** — research tools для клонов (PTL, SH366000-family, и др.):
  - **Vendor Register Viewer** — live polling 0xD0–0xFF с highlight изменений
  - **Clone Explorer** — scan SBS/MAC space, seal bypass tester, write-verify
  - **Challenge Harvester** — bulk collect challenge samples + on-the-fly entropy analysis
  - **MAC response brute** — background scan 0x0000–0xFFFF с filter на non-default ответы
  - **DF dump** — попытки чтения DataFlash 0x4000–0x7FFF через MAC
  - **Timing attack framework** — измерение HMAC verify времени для статистики
  - **Publish protocol capture** — transition brute для извлечения telemetry packet'ов
  - **Persistent background logger** — long-term мониторинг publishes

- **Generic SBS** — стандартные SBS-батареи (любые non-DJI smart packs)

### Catalog Flasher (SC01 Plus only)
SD-card каталог прошивок с touch-навигацией: выбор → детали → flash progress. Сейчас в активной разработке (Phase 1 = SD browser работает, flash pipeline на подходе).

### Ещё есть
- **SMBus Transaction Log** — ring buffer всех I²C операций на батарейной шине с export .txt
- **CP2112 HID emulator** — плата как SiLabs CP2112 для DJI Battery Killer / bqStudio (автопереключение USB режима)
- **I²C Preflight Diagnostics** — проверка SDA/SCL, bus scan
- **Battery Fleet Compare** — side-by-side сравнение JSON snapshot'ов нескольких батарей
- **Live SVG chart** — voltage/current 3-минутный ring buffer
- **Killer.ini live-import** — загрузить INI → override DF map в runtime
- **Port B mode picker** — переключение GPIO 10/11 между I2C / UART / PWM / GPIO с live-mismatch модалом если фича просит не тот режим
- **Safety stack** (SC01 Plus) — OTA rollback gate, boot-loop counter, safe-mode shell, network watchdog, coredump endpoint, factory partition, safeboot recovery firmware на отдельной AP `FPV-Recovery`

### System
- **OTA** — web upload + GitHub Release pull (per-board asset, automatic version check). Manual HTTPClient + PSRAM staging + Update.write
- **USB mode selector** — CDC / USB2TTL pump / USB2I2C (CP2112 HID) / Vendor descriptor (SC01)
- **Light/Dark theme** + workspace persistence (localStorage)
- **WiFi config** — AP mode с QR-кодом → scan + STA. На SC01 Plus также Settings screen с touch-keyboard

## Battery research

Если интересуют DJI Mavic 3 / PTL clone / Autel battery protocols — у проекта есть отдельная research-площадка:

- **[BATTERY_RESEARCH.md](BATTERY_RESEARCH.md)** — blog-style write-up: PTL BA01WM260 clone reverse engineering, расшифрованный telemetry publish protocol, HMAC-SHA1 challenge-response, NXP A1006 auth chip identification, Mavic 3 community status
- **[research/](research/)** — 50+ технических доков: Autel firmware matrix (XOR-key cracked), DJI battery pinouts (все модели), USB emulation strategy, BQ DataFlash schemas, CAN controller feasibility, BQ9006 DJI FPV battery dump analysis
- **[research/dji-battery/o-gs/](research/dji-battery/o-gs/)** — кэш o-gs/dji-firmware-tools toolkit + auto-generated `bq40z307_decoder.h` (58 MAC commands + 4×32-bit PF/Safety bit decoders)
- **[scripts/clone_research/](scripts/clone_research/)** — Python toolkit для тяжёлых scan/brute через USB2I2C (1000+ tx/sec vs ~30/sec через web)

Issues и PR с дополнительными находками **welcome**.

## Интерфейс

### Веб-интерфейс — мобильный (390×844)

| Servo / PWM | Motor / DShot | DJI Battery |
|:---:|:---:|:---:|
| ![](docs/screenshots/mobile-servo.png) | ![](docs/screenshots/mobile-motor.png) | ![](docs/screenshots/mobile-battery.png) |

| CRSF / ELRS telemetry | System + WiFi scan | ELRS Flasher |
|:---:|:---:|:---:|
| ![](docs/screenshots/mobile-crsf.png) | ![](docs/screenshots/mobile-system.png) | ![](docs/screenshots/mobile-elrs.png) |

### Веб-интерфейс — десктоп

Адаптивная grid-раскладка, карточки 520px с перетеканием в несколько колонок:

**1366×768 — 3 колонки:**

![Desktop 1366px](docs/screenshots/desktop-1366-battery.png)

**2560×1440 (2K):**

![Desktop 2K](docs/screenshots/desktop-2k-battery.png)

## Железо

### Waveshare ESP32-S3-LCD-1.47B (или diymore-клон)
- **MCU:** ESP32-S3R8, 240 MHz, 8 MB OPI PSRAM, 16 MB Flash, Native USB CDC
- **Дисплей:** ST7789, 172×320 IPS, SPI, col_offset = 34
- **IMU:** QMI8658 6-axis по I²C (0x6B) — используется для auto-rotate LCD
- **Прочее:** WS2812 RGB LED (GPIO38), micro-SD слот (SDMMC 4-bit), USB-C, LiPo charging
- **Корпус:** OpenSCAD исходник в [`hardware/case.scad`](hardware/case.scad), STL — [`hardware/case.stl`](hardware/case.stl)
- **Схема подключения:** [`hardware/schematic.pdf`](hardware/schematic.pdf)
- **Распиновка:** [`include/pin_config.h`](include/pin_config.h) (truth source), [PINOUT.md](PINOUT.md), [WIRING.md](WIRING.md)

### WT32-SC01 Plus
- **MCU:** ESP32-S3-WROOM, 240 MHz, 8 MB QSPI PSRAM, 16 MB Flash
- **Дисплей:** ST7796, 320×480 IPS, parallel 8080-8bit + DMA через LovyanGFX
- **Touch:** FT6336 ёмкостной по I²C (0x38) с runtime calibration в NVS
- **SD card:** SDMMC 1-bit (CLK/CMD/D0 = 39/40/38) на 4 MHz
- **Software stack:** LovyanGFX 1.2.x + LVGL 9.2.2 (**pinned exactly** — 9.5.0 имеет glyph alpha-blending регрессию)
- **Распиновка:** [`include/pin_config_sc01_plus.h`](include/pin_config_sc01_plus.h)

## Распиновка (Waveshare, кратко)

| Функция | GPIO |
|---|---|
| UART TX / RX (ELRS, CRSF) | 44 / 43 |
| BOOT ELRS (для прошивки RX) | 3 |
| Сигнал Servo / Motor | 2 |
| I²C SDA / SCL (IMU, DJI battery) | 48 / 47 |
| Port B SDA / SCL (battery, headers) | 11 / 10 |
| RGB LED (WS2812) | 38 |
| LCD MOSI / SCLK / CS / DC / RST / BL | 45 / 40 / 42 / 41 / 39 / 46 |
| SD CMD / CLK / D0–D3 | 15 / 14 / 16 / 18 / 17 / 21 |

Для SC01 Plus распиновка иная — см. [`include/pin_config_sc01_plus.h`](include/pin_config_sc01_plus.h).

## Сборка и прошивка

Требуется [PlatformIO](https://platformio.org/) + ESP32-Arduino Core 3.x.

```bash
# Waveshare ESP32-S3-LCD-1.47B
pio run -e esp32s3
pio run -e esp32s3 -t upload

# WT32-SC01 Plus
pio run -e wt32_sc01_plus
pio run -e wt32_sc01_plus -t upload

# Monitor
pio device monitor   # USB CDC serial (115200)
```

**⚠ ВНИМАНИЕ:** обе платы имеют одинаковый USB ID `303A:1001`. Прошивка не той платы → unbootable image (несовместимые PSRAM controllers). **Перед `pio run -t upload` всегда сверять MAC** — Waveshare `3C:DC:75:6E:CE:A8`, SC01 Plus `88:56:A6:80:EB:48`. OTA по IP — безопасно.

OTA вручную:
```bash
curl -F "update=@.pio/build/esp32s3/firmware.bin" http://<BOARD_IP>/api/ota
```

Платформа подтягивается автоматически, библиотеки в [`platformio.ini`](platformio.ini).

### Python research toolkit

Для тяжёлых scan/brute задач по battery (1000+ tx/sec vs web ~30 tx/sec):

```bash
# 1. Переключить плату в USB2I2C mode
curl -X POST -F mode=2 http://<IP>/api/usb/mode
curl -X POST http://<IP>/api/usb/reboot

# 2. Установить deps
pip install hidapi

# 3. Scripts
cd scripts/clone_research/
python cp2112.py                                # self-test
python scan_sbs_full.py                         # ~5s, все 256 SBS regs
python scan_mac_full.py --from 0 --to 0xFFFF    # ~3 min, 64K MAC subs
python challenge_harvester.py --count 10000     # bulk sample
python response_analyze.py samples.csv          # statistics

# 4. Обратно в CDC mode
curl -X POST -F mode=0 http://<IP>/api/usb/mode
curl -X POST http://<IP>/api/usb/reboot
```

## Управление

### Web (обе платы)

После буста плата поднимает AP `FPV-MultiTool` (пароль `fpv12345`) — на Waveshare LCD появляется QR с адресом, на SC01 Plus — Settings экран с WiFi scan и touch-keyboard. Можно либо подключиться к AP, либо через UI прописать STA-креденшелы.

### SC01 Plus — touch UI
LVGL springboard с экранами: Battery, Servo, Motor, ELRS, Catalog, Settings. Touch-калибровка хранится в NVS, поворот пользовательский (через Settings).

### Waveshare — status-only LCD
LCD показывает имя устройства, IP/AP, режим Port B, версию прошивки, и индикатор активности через RGB LED. Меню навигация на самой плате убрана — всё конфигурируется через Web UI.

## RGB LED (Waveshare)

| Состояние | Индикация |
|---|---|
| Прошивка ELRS в процессе | Жёлтый быстрый пульс |
| CRSF-линк активен | Бирюзовое дыхание |
| CRSF запущен, нет линка | Красный мигает |
| WiFi STA подключен | Зелёное дыхание |
| WiFi AP | Фиолетовое дыхание |
| Idle | Еле заметный белый уголёк |

## Безопасность

> ⚠ **Всегда снимай пропеллеры перед тестом мотора.**
> ⚠ Не подавай `PACK+` (до 25.2 V на 6S DJI FPV) на ESP32 — только SDA/SCL/GND.
> ⚠ Серво и ESC питай от отдельного БП, не от USB-шины платы.
> ⚠ Battery writes: seal обычно защищает от порчи, но **DJI PF2 clear (MAC 0x4062) необратим**.
> ⚠ Перед `pio run -t upload`: сверяй MAC платы с targeted env (см. выше).

## Лицензия

Private / personal use. Findings в [BATTERY_RESEARCH.md](BATTERY_RESEARCH.md) — свободно для ссылок и цитирования.
