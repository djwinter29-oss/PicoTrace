/**
 * @file i2c_monitor_config.h
 * @brief User-tunable hardware and buffer configuration for the passive I2C monitor.
 */

#ifndef I2C_MONITOR_CONFIG_H
#define I2C_MONITOR_CONFIG_H

#include <stdint.h>

/** @brief Number of logical I2C monitor channels in the current board mapping. */
#define I2C_MONITOR_CHANNEL_COUNT 4u
/** @brief Number of raw DMA buffers owned by each logical I2C monitor channel. */
#define I2C_MONITOR_BUFFER_COUNT 2u
/** @brief Number of 32-bit words stored in each raw DMA buffer. */
#define I2C_MONITOR_BUFFER_WORDS 64u
/** @brief Default I2C oversampling rate used by simple control paths. */
#define I2C_MONITOR_DEFAULT_SAMPLE_HZ 8000000u

/** @brief SDA GPIO for logical I2C monitor channel `0`. */
#define I2C_MONITOR_CHANNEL0_SDA_GPIO 16u
/** @brief SCL GPIO for logical I2C monitor channel `0`. */
#define I2C_MONITOR_CHANNEL0_SCL_GPIO 17u
/** @brief SDA GPIO for logical I2C monitor channel `1`. */
#define I2C_MONITOR_CHANNEL1_SDA_GPIO 18u
/** @brief SCL GPIO for logical I2C monitor channel `1`. */
#define I2C_MONITOR_CHANNEL1_SCL_GPIO 19u
/** @brief SDA GPIO for logical I2C monitor channel `2`. */
#define I2C_MONITOR_CHANNEL2_SDA_GPIO 20u
/** @brief SCL GPIO for logical I2C monitor channel `2`. */
#define I2C_MONITOR_CHANNEL2_SCL_GPIO 21u
/** @brief SDA GPIO for logical I2C monitor channel `3`. */
#define I2C_MONITOR_CHANNEL3_SDA_GPIO 26u
/** @brief SCL GPIO for logical I2C monitor channel `3`. */
#define I2C_MONITOR_CHANNEL3_SCL_GPIO 27u

#endif