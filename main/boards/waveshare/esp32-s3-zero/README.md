# Waveshare ESP32-S3-Zero

Waveshare ESP32-S3-Zero is a mini-sized development board based on the ESP32-S3FH4R2 chip (Dual-core Xtensa LX7 @ 240MHz, 4MB embedded Flash, 2MB embedded Quad PSRAM).

## Hardware Specifications

- **MCU**: ESP32-S3FH4R2
- **Flash**: 4MB Quad SPI Flash
- **PSRAM**: 2MB Quad SPI PSRAM
- **Onboard WS2812 RGB LED**: `GPIO 21` (used by `SingleLed` for device status)
- **Onboard BOOT Button**: `GPIO 0` (Click to toggle chat; click while starting to enter Wi-Fi configuration mode)
- **USB**: Native USB Type-C

## Pin Assignments

### Audio (I2S)

Standard external digital microphone (e.g. INMP441) and DAC/Amp (e.g. MAX98357A):

| Function | Pin | Note |
|---|---|---|
| Speaker BCLK | `GPIO 1` | I2S Bit Clock (Speaker) |
| Speaker LRCK | `GPIO 2` | I2S Word Select (Speaker) |
| Microphone SCK | `GPIO 4` | I2S Bit Clock (Microphone) |
| Microphone WS | `GPIO 5` | I2S Word Select (Microphone) |
| Microphone DIN | `GPIO 6` | I2S Data In from Mic |
| Speaker DOUT | `GPIO 7` | I2S Data Out to Amp |

### Display Options (Dynamic NVS or Variant)

The board dynamically detects display settings configured via the Web Wi-Fi portal (or compile-time default):

#### Option A: OLED Display (I2C SSD1306 / SH1106)
| Function | Pin | Note |
|---|---|---|
| I2C SDA | `GPIO 8` | Data line |
| I2C SCL | `GPIO 9` | Clock line |

#### Option B: Color TFT LCD (SPI ST7789)
Supports 7-Pin (without CS) and 8-Pin ST7789 (240x240, 240x280, 240x320):
| Function | Pin | Note |
|---|---|---|
| SPI SCLK (SCL) | `GPIO 9` | Clock line |
| SPI MOSI (SDA) | `GPIO 8` | Data line |
| DC / Command | `GPIO 10` | Data / Command control |
| RES / Reset | `GPIO 11` | Hardware reset |
| CS / Chip Select | `GPIO 12` | Unused / grounded for 7-pin modules |
| BL / Backlight | `GPIO 13` | PWM brightness control |

Pins can also be customized via the web Wi-Fi portal (under Hardware/Pins tab).

## Build Variants

1. **`esp32-s3-zero`** (Default):
   Unified build supporting dynamic display switching via Web Portal (Headless, OLED SSD1306/SH1106, or ST7789 TFT SPI).

   ```sh
   python scripts/build.py waveshare/esp32-s3-zero --name esp32-s3-zero
   ```
