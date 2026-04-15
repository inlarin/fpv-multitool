# FPV MultiTool

Многофункциональный тестер для FPV-дронов на базе **ESP32-S3-LCD-1.47** с 1.47" IPS-дисплеем, веб-интерфейсом и оффлайн-меню на корпусной кнопке.

![FPV MultiTool — плата в сборе](hardware/photo-front.jpg)

## Что умеет

- **USB2TTL мост** — прошивка ELRS приёмников через USB (autobaud, serial monitor, подсказка по пинам)
- **Servo Tester** — PWM 500–2500 μs, 50/330 Hz, manual / center / sweep
- **Motor Tester** — ESC по DShot150/300/600 (RMT), arm/disarm, beep, направление, 3D-режим, ограничитель газа
- **DJI Battery service** — SMBus-диагностика Mavic / Mini / Air / Phantom / Spark:
  - Полные статусы: SafetyStatus, PFStatus, OperationStatus, DAStatus1
  - Автоопределение модели и химии
  - Unseal / Clear PF / Seal для моделей с публичными TI-ключами
  - I²C-сканер
- **WiFi + веб-интерфейс**:
  - Автостарт AP (`FPV-MultiTool` / `fpv12345`), QR-код на экране
  - Сканер сетей, сохранение STA-креденшелов в NVS
  - WebSocket-телеметрия всего: батарея, каналы, линк ELRS, прогресс прошивки
- **ELRS Flash по воздуху** — upload `.bin` / `.bin.gz` / `.elrs` в браузере, gzip-распаковка на ESP, прошивка через ROM-bootloader приёмника
- **CRSF / ELRS telemetry** — live RSSI/LQ/SNR, 16 каналов, FC-телеметрия, полное дерево параметров приёмника (type-aware write, COMMAND-lifecycle как в elrsv3.lua)
- **RGB status LED** — приоритетная подсветка состояния (AP/STA/CRSF link/flashing)

## Интерфейс

### Мобильный (390×844)

| Servo / PWM | Motor / DShot | DJI Battery |
|:---:|:---:|:---:|
| ![](docs/screenshots/mobile-servo.png) | ![](docs/screenshots/mobile-motor.png) | ![](docs/screenshots/mobile-battery.png) |

| CRSF / ELRS telemetry | System + WiFi scan | ELRS Flasher |
|:---:|:---:|:---:|
| ![](docs/screenshots/mobile-crsf.png) | ![](docs/screenshots/mobile-system.png) | ![](docs/screenshots/mobile-elrs.png) |

### Десктоп — адаптивная grid-раскладка

Контент центрирован, карточки ограничены 520px и перетекают в несколько колонок по мере ширины экрана:

**1366×768 — 3 колонки:**

![Desktop 1366px](docs/screenshots/desktop-1366-battery.png)

**2560×1440 (2K) — 3 колонки по центру:**

![Desktop 2K](docs/screenshots/desktop-2k-battery.png)

## Железо

- **Плата:** Waveshare ESP32-S3-LCD-1.47B (или diymore-клон с идентичной распиновкой)
- **MCU:** ESP32-S3R8, 240 MHz, 8 MB PSRAM, 16 MB Flash, Native USB CDC
- **Дисплей:** ST7789, 172×320 IPS, SPI, col_offset = 34
- **IMU:** QMI8658 6-axis по I²C (0x6B)
- **Прочее:** WS2812 RGB LED (GPIO38), micro-SD слот (SDMMC), USB-C, LiPo charging
- **Корпус:** OpenSCAD исходник в [`hardware/case.scad`](hardware/case.scad), STL — [`hardware/case.stl`](hardware/case.stl)
- **Схема подключения:** [`hardware/schematic.pdf`](hardware/schematic.pdf)

Дополнительные фото: [`hardware/photo-intro.jpg`](hardware/photo-intro.jpg), [`hardware/photo-inter.jpg`](hardware/photo-inter.jpg), [`hardware/photo-detail3.jpg`](hardware/photo-detail3.jpg), [`hardware/photo-detail4.jpg`](hardware/photo-detail4.jpg).

## Распиновка (кратко)

| Функция | GPIO |
|---|---|
| UART TX / RX (ELRS, CRSF) | 44 / 43 |
| BOOT ELRS (для прошивки RX) | 3 |
| Сигнал Servo / Motor | 2 |
| I²C SDA / SCL (IMU, DJI battery) | 48 / 47 |
| RGB LED (WS2812) | 38 |
| LCD MOSI / SCLK / CS / DC / RST / BL | 45 / 40 / 42 / 41 / 39 / 46 |
| SD CMD / CLK / D0–D3 | 15 / 14 / 16 / 18 / 17 / 21 |

Полная карта: [PINOUT.md](PINOUT.md), схемы подключения периферии: [WIRING.md](WIRING.md).

## Сборка и прошивка

Требуется [PlatformIO](https://platformio.org/) + ESP32-Arduino Core 3.x.

```bash
pio run              # build
pio run -t upload    # build + flash через USB-C
pio device monitor   # USB CDC serial (115200)
```

Платформа подтягивается автоматически, все библиотеки объявлены в [`platformio.ini`](platformio.ini).

## Управление

### Физическая кнопка BOOT

- **Click** — следующий пункт меню
- **Double-click** — предыдущий
- **Long press** — выбрать / назад

### Веб-интерфейс

После буста плата поднимает AP `FPV-MultiTool` (пароль `fpv12345`) — на экране появится QR-код с адресом `http://192.168.4.1`. Можно либо подключиться к AP, либо через вкладку **System** в UI прописать STA-креденшелы и перейти на домашний роутер.

## RGB LED

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
> ⚠ Не подавай `PACK+` (до 15.4 V на 4S) DJI-батареи на ESP32 — только SDA/SCL/GND.
> ⚠ Серво и ESC питай от отдельного БП, не от USB-шины платы.

## Лицензия

Private / personal use.
