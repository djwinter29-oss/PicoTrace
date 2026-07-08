/**
 * @file tusb_config.h
 * @brief TinyUSB device-stack configuration for the PicoTrace firmware image.
 */

#ifndef PICO_TRACE_TUSB_CONFIG_H
#define PICO_TRACE_TUSB_CONFIG_H

/** @brief TinyUSB MCU target used for PicoTrace firmware builds. */
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_RP2040
#endif

/** @brief TinyUSB root-hub port mode used by PicoTrace. */
#ifndef CFG_TUSB_RHPORT0_MODE
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#endif

/** @brief TinyUSB OS abstraction layer selection for bare-metal PicoTrace firmware. */
#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS OPT_OS_NONE
#endif

/** @brief Number of CDC interfaces enabled in the TinyUSB device stack. */
#define CFG_TUD_CDC 1
/** @brief Number of HID interfaces enabled in the TinyUSB device stack. */
#define CFG_TUD_HID 1
/** @brief Number of vendor interfaces enabled in the TinyUSB device stack. */
#define CFG_TUD_VENDOR 1

/** @brief TinyUSB CDC receive buffer size in bytes. */
#ifndef CFG_TUD_CDC_RX_BUFSIZE
#define CFG_TUD_CDC_RX_BUFSIZE 128
#endif

/** @brief TinyUSB CDC transmit buffer size in bytes. */
#ifndef CFG_TUD_CDC_TX_BUFSIZE
#define CFG_TUD_CDC_TX_BUFSIZE 128
#endif

/** @brief TinyUSB CDC endpoint buffer size in bytes. */
#ifndef CFG_TUD_CDC_EP_BUFSIZE
#define CFG_TUD_CDC_EP_BUFSIZE 64
#endif

/** @brief TinyUSB HID endpoint buffer size in bytes. */
#ifndef CFG_TUD_HID_EP_BUFSIZE
#define CFG_TUD_HID_EP_BUFSIZE 64
#endif

/** @brief TinyUSB vendor receive buffer size in bytes. */
#ifndef CFG_TUD_VENDOR_RX_BUFSIZE
#define CFG_TUD_VENDOR_RX_BUFSIZE 64
#endif

/** @brief TinyUSB vendor transmit buffer size in bytes. */
#ifndef CFG_TUD_VENDOR_TX_BUFSIZE
#define CFG_TUD_VENDOR_TX_BUFSIZE 4096
#endif

#endif