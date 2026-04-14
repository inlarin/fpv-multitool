# FPV MultiTool

Многофункциональный тестер для FPV-дронов на базе ESP32-S3 с 1.47" LCD.

## Что умеет

- **USB2TTL мост** — прошивка ELRS приёмников через USB (autobaud, serial monitor, wiring help)
- **Servo Tester** — генерация PWM 500-2500μs, 50/330Hz, manual/center/sweep режимы
- **Motor Tester** — управление ESC по DShot150/300/600 через RMT, arm/disarm, beep ESC
- **Battery Tool** — сервис DJI батарей по SMBus:
  - Полная диагностика (SafetyStatus, PFStatus, OperationStatus, DAStatus1)
  - Model detection (Mavic 1-4, Mini 1-3, Air, Phantom, Spark)
  - Unseal / Clear PF / Seal (для старых моделей с TI ключом)
  - I2C scanner
- **WiFi / Web интерфейс**:
  - Автостарт AP mode при включении (SSID `FPV-MultiTool`, pass `fpv12345`)
  - QR-код на экране для быстрого подключения
  - STA mode с сохранением креденшелов
  - Веб-интерфейс со всеми функциями + телеметрией
- **ELRS Flash через WiFi** — загрузка `.bin` / `.bin.gz` / `.elrs` в браузере, распаковка gzip, прошивка через ESP ROM bootloader
- **CRSF / ELRS** — live телеметрия, 16 каналов, параметры приёмника, команды (bind, reboot)

## Железо

- Плата: **Waveshare ESP32-S3-LCD-1.47B** (или diymore клон)
- ESP32-S3R8: 240MHz, 8MB PSRAM, 16MB Flash, Native USB
- Дисплей ST7789 172x320 IPS
- QMI8658 6-axis IMU, RGB LED, micro SD slot

## Сборка

```bash
pio run -t upload
```

Требует PlatformIO + espressif32 platform (Arduino Core 3.x).

## Распиновка

Полная схема подключения: см. [PINOUT.md](PINOUT.md) и [WIRING.md](WIRING.md), либо [wiring_diagram.html](wiring_diagram.html) для интерактивной визуализации.

Основные GPIO:
- **GPIO 43/44** — UART TX/RX для ELRS и CRSF
- **GPIO 3** — BOOT ELRS (для прошивки)
- **GPIO 2** — сигнал Servo/Motor
- **GPIO 47/48** — I2C (QMI8658 + DJI Battery)
- **5V / GND / 3.3V** — питание

## Навигация по меню

Кнопка BOOT на плате:
- **Click** — следующий пункт
- **Double-click** — предыдущий
- **Long press** — выбрать

## Безопасность

⚠ Всегда снимай пропеллеры перед тестом мотора
⚠ Не подключай PACK+ (15.4V) DJI батареи к ESP32 — только SDA/SCL/GND
⚠ Серво/ESC питай от внешнего источника, не от платы

## Лицензия

Private / personal use.
