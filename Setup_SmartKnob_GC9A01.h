// SmartKnob ESP32-S3 + round 240x240 GC9A01 (SPI)
// Panel SCL = SCLK, SDA = MOSI. Not I2C.
#define USER_SETUP_ID 247
#define USER_SETUP_INFO "SmartKnob_GC9A01"

#define GC9A01_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 240

#define TFT_SCLK 5
#define TFT_MOSI 6
#define TFT_DC   7
#define TFT_CS   8
#define TFT_RST  9

#define LOAD_GLCD
#define SPI_FREQUENCY 40000000

// ESP32-S3: default FSPI is 0 in Arduino-ESP32 3.x and tft.init() StoreProhibited at 0x10
#define USE_HSPI_PORT
