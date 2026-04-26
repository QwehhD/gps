// Setup for ESP32-S3 with GC9A01 display
#define USER_SETUP_ID 70

#define GC9A01_DRIVER

#define TFT_WIDTH  240
#define TFT_HEIGHT 240

// Customize pins untuk setup Anda
#define TFT_CS   13  // Chip Select
#define TFT_MOSI 11  // SPI Data
#define TFT_SCLK 12  // SPI Clock
#define TFT_MISO -1  // Not used (write-only)

#define TFT_DC   14  // Data/Command
#define TFT_RST  21  // Reset

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF

#define SMOOTH_FONT

// Use FSPI (SPI2) port
#define USE_FSPI_PORT

#define SPI_FREQUENCY  80000000   // 80 MHz for GC9A01
