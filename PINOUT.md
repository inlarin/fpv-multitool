# ESP32-S3-LCD-1.47B — Полная распиновка

## Вид сверху (USB-C сверху, экран снизу)

```
                    ┌──── USB-C ────┐
                    │               │
              ┌─────┴───────────────┴─────┐
              │  [RST]           [BOOT]   │
              │                           │
    ──────────┤                           ├──────────
    Левая     │     ESP32-S3-LCD-1.47B    │    Правая
    гребёнка  │                           │    гребёнка
    ──────────┤      ┌─────────────┐      ├──────────
              │      │  SD Card    │      │
              │      │  Slot       │      │
              │      └─────────────┘      │
              │                           │
              │    ┌─────────────────┐    │
              │    │                 │    │
              │    │   1.47" LCD     │    │
              │    │   172 x 320    │    │
              │    │                 │    │
              │    └─────────────────┘    │
              └───────────────────────────┘
```

## Распределение GPIO

### Занято платой (НЕ ТРОГАТЬ)

| GPIO | Функция           | Примечание              |
|------|--------------------|-------------------------|
| 40   | LCD SCLK           | SPI Clock               |
| 45   | LCD MOSI           | SPI Data                |
| 42   | LCD CS             | SPI Chip Select         |
| 41   | LCD DC             | Data/Command            |
| 39   | LCD RST            | Display Reset           |
| 46   | LCD BL             | Backlight (PWM)         |
| 48   | I2C SDA            | QMI8658 IMU             |
| 47   | I2C SCL            | QMI8658 IMU             |
| 38   | RGB LED            | WS2812 NeoPixel         |
| 14   | SD CLK             | SD Card (SDMMC)         |
| 15   | SD CMD             | SD Card                 |
| 16   | SD D0              | SD Card                 |
| 17   | SD D2              | SD Card                 |
| 18   | SD D1              | SD Card                 |
| 21   | SD D3              | SD Card                 |
| 0    | BOOT Button        | Навигация по меню       |
| 19   | USB D-             | Native USB              |
| 20   | USB D+             | Native USB              |

### Назначено в FPV MultiTool

| GPIO | Функция           | Подключение             |
|------|--------------------|-------------------------|
| 1    | BAT ADC            | Делитель напряжения АКБ |
| 2    | SIGNAL OUT         | Servo PWM / DShot ESC   |
| 43   | UART TX (ELRS)     | → RX приёмника          |
| 44   | UART RX (ELRS)     | → TX приёмника          |
| 3    | ELRS BOOT          | → GPIO0 приёмника       |

### Свободные GPIO (на гребёнке)

| GPIO | ADC канал | Примечание              |
|------|-----------|-------------------------|
| 4    | ADC1_CH3  | Свободен                |
| 5    | ADC1_CH4  | Свободен                |
| 6    | ADC1_CH5  | Свободен                |
| 7    | ADC1_CH6  | Свободен                |
| 8    | ADC1_CH7  | Свободен                |
| 9    | ADC1_CH8  | Свободен                |
| 10   | ADC1_CH9  | Свободен                |
| 11   | ADC2_CH0  | Свободен                |
| 12   | ADC2_CH1  | Свободен                |
| 13   | ADC2_CH2  | Свободен                |
| 35   | —         | Свободен                |
| 36   | —         | Свободен                |
| 37   | —         | Свободен                |


## Схема подключения периферии

### Servo / Motor (GPIO 2)
```
  ESP32          Servo/ESC
  ─────          ─────────
  GPIO 2  ─────► Signal (белый/жёлтый)
  GND     ─────► GND    (чёрный/коричневый)
                 VCC    ← внешнее питание 5-8.4V
```

### ELRS приёмник (GPIO 43/44/3)
```
  ESP32          ELRS Receiver
  ─────          ─────────────
  GPIO 43 TX ──► RX
  GPIO 44 RX ◄── TX
  GPIO 3     ──► GPIO0 (BOOT)
  3.3V       ──► VCC
  GND        ──► GND
```

### DJI Battery (I2C: GPIO 48/47)
```
  ESP32          DJI Battery (8-pin)
  ─────          ───────────────────
  GPIO 48 SDA ──► Pin 4 (SDA)     ┐ 4.7K pull-up
  GPIO 47 SCL ──► Pin 5 (SCL)     ┘ к 3.3V
  GND         ──► Pin 1-3 (GND)

  ⚠ НЕ подключать PACK+ (Pin 6-8) к ESP32!
  
  DJI Battery Pinout (вид на контакты):
  ┌─────────────────────────┐
  │ 1  2  3  4  5  6  7  8 │
  │GND GND GND SDA SCL V+ V+ V+│
  └─────────────────────────┘
```

### Батарея платы (GPIO 1)
```
  BAT+ ──┤200K├──┬──► GPIO 1 (ADC)
                 │
                ┤100K├
                 │
  GND  ──────────┘

  V_bat = ADC × 3 × (3.3 / 4095)
```
