#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// If using Duplex I2S mode (shared BCLK/WS), comment out AUDIO_I2S_METHOD_SIMPLEX
#define AUDIO_I2S_METHOD_SIMPLEX

#ifdef AUDIO_I2S_METHOD_SIMPLEX
// Simplex I2S: separate clocks for microphone and speaker
#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_5
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_4
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_7
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_1
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_2
#else
// Duplex I2S: shared BCLK and WS
#define AUDIO_I2S_GPIO_WS       GPIO_NUM_5
#define AUDIO_I2S_GPIO_BCLK     GPIO_NUM_4
#define AUDIO_I2S_GPIO_DIN      GPIO_NUM_6
#define AUDIO_I2S_GPIO_DOUT     GPIO_NUM_7
#endif

// Onboard WS2812 RGB LED and Buttons
#define BUILTIN_LED_GPIO        GPIO_NUM_21
#define BOOT_BUTTON_GPIO        GPIO_NUM_0   // Onboard BOOT button
#define CHAT_BUTTON_GPIO        GPIO_NUM_14  // External Button 1 (Chat/Action)
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_15  // External Button 2 (Vol +)
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_16  // External Button 3 (Vol -)

// Battery ADC (GPIO 3 = ADC1 Channel 2; safe with Wi-Fi)
#define BATTERY_ADC_PIN         GPIO_NUM_3
#define BATTERY_ADC_UNIT        ADC_UNIT_1
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_2
#define BATTERY_CHARGING_PIN    GPIO_NUM_NC

// Default Display Preset (ST7789 1.69" 240x280)
#define DEFAULT_DISPLAY_PRESET  "st7789_240x280"

// ST7789 SPI Display Pins & Dimensions
#define DISPLAY_SDA_PIN         GPIO_NUM_8
#define DISPLAY_SCL_PIN         GPIO_NUM_9
#define DISPLAY_DC_PIN          GPIO_NUM_10
#define DISPLAY_RST_PIN         GPIO_NUM_11
#define DISPLAY_CS_PIN          GPIO_NUM_12
#define DISPLAY_BL_PIN          GPIO_NUM_13

#define DISPLAY_WIDTH           240
#define DISPLAY_HEIGHT          280
#define DISPLAY_OFFSET_X        0
#define DISPLAY_OFFSET_Y        20
#define DISPLAY_MIRROR_X        false
#define DISPLAY_MIRROR_Y        false
#define DISPLAY_SWAP_XY         false

#endif // _BOARD_CONFIG_H_
