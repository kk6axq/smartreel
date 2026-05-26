// =====================================================================
//  Waveshare ESP32-S3-Touch-LCD-4.3B  --  pin map
//
//  Source: Waveshare wiki schematic (rev that ships with 8 MB PSRAM).
//  Anything driven through the CH422G expander is flagged with EXIO_*.
// =====================================================================
#pragma once

#include <stdint.h>

// ---- LCD: 16-bit RGB565 parallel (DPI) ------------------------------
// Frame format from the 4.3B datasheet / Waveshare demo:
//   active   : 800 x 480
//   pclk     : 16 MHz. Lowering to 12 MHz puts the panel into a
//              stuck-white state on this board (the IDF clock divider
//              rounds to a frequency the panel doesn't like). The
//              wobble during screen transitions is addressed instead
//              by caching each screen so navigate() doesn't trigger
//              a big PSRAM rebuild burst.
//   hsync    : pulse 4,  back porch 8,  front porch 8
//   vsync    : pulse 4,  back porch 8,  front porch 8
#define LCD_H_RES                 800
#define LCD_V_RES                 480
#define LCD_PIXEL_CLOCK_HZ        (16 * 1000 * 1000)
#define LCD_HSYNC_PULSE_WIDTH     4
#define LCD_HSYNC_BACK_PORCH      8
#define LCD_HSYNC_FRONT_PORCH     8
#define LCD_VSYNC_PULSE_WIDTH     4
#define LCD_VSYNC_BACK_PORCH      8
#define LCD_VSYNC_FRONT_PORCH     8

// Sync / timing pins
#define LCD_PIN_HSYNC       46
#define LCD_PIN_VSYNC       3
#define LCD_PIN_DE          5
#define LCD_PIN_PCLK        7

// RGB565 data lines
#define LCD_PIN_R0          1   // R3 in 5:6:5 -> bit 11
#define LCD_PIN_R1          2
#define LCD_PIN_R2          42
#define LCD_PIN_R3          41
#define LCD_PIN_R4          40
#define LCD_PIN_G0          39
#define LCD_PIN_G1          0
#define LCD_PIN_G2          45
#define LCD_PIN_G3          48
#define LCD_PIN_G4          47
#define LCD_PIN_G5          21
#define LCD_PIN_B0          14
#define LCD_PIN_B1          38
#define LCD_PIN_B2          18
#define LCD_PIN_B3          17
#define LCD_PIN_B4          10

// ---- I2C bus (shared by GT911 touch + CH422G expander) ---------------
#define I2C_SDA_PIN          8
#define I2C_SCL_PIN          9
#define I2C_FREQ_HZ          400000

#define CH422G_I2C_ADDR      0x24    // 7-bit
#define GT911_I2C_ADDR_PRI   0x5D
#define GT911_I2C_ADDR_SEC   0x14

// GT911 INT pin is wired direct to the SoC; RST goes through CH422G.
#define TOUCH_INT_PIN        4

// ---- CH422G expander outputs ----------------------------------------
// Bit indices on the CH422G output port (8 bits total).
#define EXIO_TOUCH_RST_BIT   1
#define EXIO_LCD_BL_BIT      2
#define EXIO_LCD_RST_BIT     3
#define EXIO_USB_SEL_BIT     5

// ---- microSD (SDMMC 1-bit mode) -------------------------------------
// Waveshare 4.3B routes the microSD slot to the ESP32-S3's SD/MMC
// peripheral (slot 1), NOT to SPI. Pins are GPIO-matrix routed.
//   CMD  = 11
//   CLK  = 12
//   D0   = 13
// We use 1-bit mode (CMD/CLK/D0 only) to keep GPIOs 14/21 free for
// the LCD's B0/G5 lanes.
#define SD_MMC_CMD          11
#define SD_MMC_CLK          12
#define SD_MMC_D0           13

// ---- RS485 link to the Core PCB -------------------------------------
// The Waveshare 4.3B's onboard RS485 is an AUTOMATIC direction-control
// circuit (no MCU enable pin): direction is handled in hardware, so the
// differential bus follows the TX line. We therefore run the UART in
// plain UART_MODE_UART -- NOT RS485 half-duplex -- and assign no DE pin.
//
// Pins are the ESP32-S3 UART0 lines (GPIO43/44), free because the USB-
// CDC console owns "Serial". NOTE: TX=44 / RX=43 here is the *reverse*
// of the chip's default U0TXD=43 / U0RXD=44 -- the board routes them
// this way, so do not "correct" it.
#define RS485_PIN_TX        44
#define RS485_PIN_RX        43
// RS485_PIN_DE intentionally undefined: direction is automatic in HW.

// Baud rate. The auto-direction transistor/RC switch on this board
// cannot keep up at 921600 (Waveshare's reference demo runs 115200),
// so cap at 115200. This bounds RS485 firmware-update throughput.
#define RS485_BAUD          115200

// ---- Misc ------------------------------------------------------------
// Some board revs expose a pair of generic GPIO headers; left unused.
