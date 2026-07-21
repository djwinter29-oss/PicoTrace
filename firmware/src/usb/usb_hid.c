#include "usb/usb_hid.h"

#include "tusb.h"

#include <stddef.h>

#include "device_control.h"
#include <string.h>

/** @brief Payload bytes required for the I2C set-rate request. */
#define USB_HID_I2C_MONITOR_SET_PAYLOAD_BYTES 5u
/** @brief Payload bytes required for the SPI set-config request. */
#define USB_HID_SPI_MONITOR_SET_PAYLOAD_BYTES 8u

/** @brief Function type used by the local HID opcode dispatcher. */
typedef bool (*usb_hid_handler_fn_t)(usb_hid_command_t *response, const usb_hid_command_t *request);

/** @brief One local HID opcode descriptor used for uniform dispatch and length checks. */
typedef struct {
    uint8_t opcode; /**< HID opcode handled by this descriptor. */
    uint8_t min_payload_length; /**< Minimum request payload bytes required for the opcode. */
    usb_hid_handler_fn_t handler; /**< Callback that prepares the response for the opcode. */
} usb_hid_opcode_handler_t;

static void usb_hid_set_response(const usb_hid_command_t *response);
static void usb_hid_prepare_response(usb_hid_command_t *response, const usb_hid_command_t *request, usb_hid_status_t status);

/** @brief Decode a 32-bit little-endian value from a HID payload buffer. */
static uint32_t usb_hid_read_u32_le(const uint8_t *data) {
    return ((uint32_t)data[0]) |
        (((uint32_t)data[1]) << 8u) |
        (((uint32_t)data[2]) << 16u) |
        (((uint32_t)data[3]) << 24u);
}

static bool usb_hid_handle_nop(usb_hid_command_t *response, const usb_hid_command_t *request) {
    (void)response;
    (void)request;
    return true;
}

static bool usb_hid_handle_get_status(usb_hid_command_t *response, const usb_hid_command_t *request) {
    (void)request;

    response->payload_length = device_control_encode_device_status_payload(response->payload, sizeof(response->payload));
    if (response->payload_length == 0u) {
        response->status = USB_HID_STATUS_REJECTED;
    }

    return true;
}

static bool usb_hid_handle_stream_enable(usb_hid_command_t *response, const usb_hid_command_t *request) {
    (void)response;
    (void)request;
    device_control_set_stream_enabled(true);
    return true;
}

static bool usb_hid_handle_stream_disable(usb_hid_command_t *response, const usb_hid_command_t *request) {
    (void)response;
    (void)request;
    device_control_set_stream_enabled(false);
    return true;
}

static bool usb_hid_handle_led_on(usb_hid_command_t *response, const usb_hid_command_t *request) {
    (void)response;
    (void)request;
    device_control_set_led(true);
    return true;
}

static bool usb_hid_handle_led_off(usb_hid_command_t *response, const usb_hid_command_t *request) {
    (void)response;
    (void)request;
    device_control_set_led(false);
    return true;
}

static bool usb_hid_handle_reboot(usb_hid_command_t *response, const usb_hid_command_t *request) {
    (void)response;
    (void)request;
    device_control_reboot();
    return true;
}

/** @brief Apply one I2C monitor set-rate HID request. */
static bool usb_hid_handle_i2c_monitor_set_rate(usb_hid_command_t *response, const usb_hid_command_t *request) {
    device_control_result_t result;
    uint32_t sample_hz;

    sample_hz = usb_hid_read_u32_le(&request->payload[1]);
    result = device_control_i2c_configure_channel(request->payload[0], sample_hz, NULL);
    if (result != DEVICE_CONTROL_RESULT_OK) {
        response->status = device_control_result_to_hid_status(result);
    }

    return true;
}

/** @brief Encode one I2C monitor status snapshot into a HID response payload. */
static bool usb_hid_handle_i2c_monitor_get_status(usb_hid_command_t *response, const usb_hid_command_t *request) {
    i2c_monitor_channel_status_t status;
    device_control_result_t result;

    result = device_control_i2c_read_channel_status(request->payload[0], &status);
    if (result != DEVICE_CONTROL_RESULT_OK) {
        response->status = device_control_result_to_hid_status(result);
        response->payload_length = 0u;
        return true;
    }

    response->payload_length = device_control_encode_i2c_channel_status_payload(
        request->payload[0],
        &status,
        response->payload,
        sizeof(response->payload)
    );
    if (response->payload_length == 0u) {
        response->status = USB_HID_STATUS_REJECTED;
    }

    return true;
}

static bool usb_hid_handle_i2c_monitor_get_all_status(usb_hid_command_t *response) {
    i2c_monitor_channel_status_t status[I2C_MONITOR_CHANNEL_COUNT];
    device_control_result_t result;

    result = device_control_i2c_read_all_status(status);
    if (result != DEVICE_CONTROL_RESULT_OK) {
        response->status = device_control_result_to_hid_status(result);
        response->payload_length = 0u;
        return true;
    }

    response->payload_length = device_control_encode_i2c_all_status_payload(
        status,
        I2C_MONITOR_CHANNEL_COUNT,
        response->payload,
        sizeof(response->payload)
    );
    if (response->payload_length == 0u) {
        response->status = USB_HID_STATUS_REJECTED;
    }

    return true;
}

/** @brief Apply one SPI monitor set-config HID request. */
static bool usb_hid_handle_spi_monitor_set_config(usb_hid_command_t *response, const usb_hid_command_t *request) {
    spi_monitor_bus_config_t config;
    device_control_result_t result;

    config.capture = (spi_monitor_capture_t)request->payload[1];
    config.spi_mode = request->payload[2];
    config.channel_select_mask = request->payload[3];
    config.timeout_us = usb_hid_read_u32_le(&request->payload[4]);
    result = device_control_spi_configure_bus(request->payload[0], &config, NULL);
    if (result != DEVICE_CONTROL_RESULT_OK) {
        response->status = device_control_result_to_hid_status(result);
    }

    return true;
}

/** @brief Encode one SPI monitor bus status snapshot into a HID response payload. */
static bool usb_hid_handle_spi_monitor_get_status(usb_hid_command_t *response, const usb_hid_command_t *request) {
    spi_monitor_bus_status_t status;
    device_control_result_t result;

    result = device_control_spi_read_bus_status(request->payload[0], &status);
    if (result != DEVICE_CONTROL_RESULT_OK) {
        response->status = device_control_result_to_hid_status(result);
        response->payload_length = 0u;
        return true;
    }

    response->payload_length = device_control_encode_spi_bus_status_payload(
        request->payload[0],
        &status,
        response->payload,
        sizeof(response->payload)
    );
    if (response->payload_length == 0u) {
        response->status = USB_HID_STATUS_REJECTED;
    }

    return true;
}

static bool usb_hid_handle_spi_monitor_get_all_status(usb_hid_command_t *response) {
    spi_monitor_channel_status_t status[SPI_MONITOR_CHANNEL_COUNT];
    device_control_result_t result;

    result = device_control_spi_read_all_status(status);
    if (result != DEVICE_CONTROL_RESULT_OK) {
        response->status = device_control_result_to_hid_status(result);
        response->payload_length = 0u;
        return true;
    }

    response->payload_length = device_control_encode_spi_all_status_payload(
        status,
        SPI_MONITOR_CHANNEL_COUNT,
        response->payload,
        sizeof(response->payload)
    );
    if (response->payload_length == 0u) {
        response->status = USB_HID_STATUS_REJECTED;
    }

    return true;
}

static bool usb_hid_handle_i2c_monitor_get_all_status_request(usb_hid_command_t *response, const usb_hid_command_t *request) {
    (void)request;
    return usb_hid_handle_i2c_monitor_get_all_status(response);
}

static bool usb_hid_handle_spi_monitor_get_all_status_request(usb_hid_command_t *response, const usb_hid_command_t *request) {
    (void)request;
    return usb_hid_handle_spi_monitor_get_all_status(response);
}

static const usb_hid_opcode_handler_t usb_hid_opcode_handlers[] = {
    {USB_HID_OPCODE_NOP, 0u, usb_hid_handle_nop},
    {USB_HID_OPCODE_GET_STATUS, 0u, usb_hid_handle_get_status},
    {USB_HID_OPCODE_STREAM_ENABLE, 0u, usb_hid_handle_stream_enable},
    {USB_HID_OPCODE_STREAM_DISABLE, 0u, usb_hid_handle_stream_disable},
    {USB_HID_OPCODE_I2C_MONITOR_SET_RATE, USB_HID_I2C_MONITOR_SET_PAYLOAD_BYTES, usb_hid_handle_i2c_monitor_set_rate},
    {USB_HID_OPCODE_I2C_MONITOR_GET_STATUS, 1u, usb_hid_handle_i2c_monitor_get_status},
    {USB_HID_OPCODE_I2C_MONITOR_GET_ALL_STATUS, 0u, usb_hid_handle_i2c_monitor_get_all_status_request},
    {USB_HID_OPCODE_SPI_MONITOR_SET_CONFIG, USB_HID_SPI_MONITOR_SET_PAYLOAD_BYTES, usb_hid_handle_spi_monitor_set_config},
    {USB_HID_OPCODE_SPI_MONITOR_GET_STATUS, 1u, usb_hid_handle_spi_monitor_get_status},
    {USB_HID_OPCODE_SPI_MONITOR_GET_ALL_STATUS, 0u, usb_hid_handle_spi_monitor_get_all_status_request},
    {USB_HID_OPCODE_LED_ON, 0u, usb_hid_handle_led_on},
    {USB_HID_OPCODE_LED_OFF, 0u, usb_hid_handle_led_off},
    {USB_HID_OPCODE_REBOOT, 0u, usb_hid_handle_reboot},
};

static bool usb_hid_dispatch_command(usb_hid_command_t *response, const usb_hid_command_t *request) {
    for (uint32_t index = 0u; index < (sizeof(usb_hid_opcode_handlers) / sizeof(usb_hid_opcode_handlers[0])); ++index) {
        const usb_hid_opcode_handler_t *handler = &usb_hid_opcode_handlers[index];

        if (handler->opcode != request->opcode) {
            continue;
        }

        if (request->payload_length < handler->min_payload_length) {
            response->status = USB_HID_STATUS_BAD_LENGTH;
            response->payload_length = 0u;
            return true;
        }

        return handler->handler(response, request);
    }

    return false;
}

/** @brief Most recently received HID output report pending dispatch. */
static usb_hid_command_t hid_command_state;
/** @brief Most recently prepared HID input report returned to the host. */
static usb_hid_command_t hid_report_state;
/** @brief Indicates whether @ref hid_command_state currently holds an unread command. */
static bool hid_command_pending;
/** @brief Indicates whether @ref hid_report_state holds a prepared response. */
static bool hid_response_ready;
/** @brief Lightweight observability counters for HID control-path behavior. */
static usb_hid_stats_t usb_hid_stats;

/**
 * @brief Stage a BUSY response for one incoming HID report without overwriting an older pending command.
 * @param buffer Caller-owned raw HID report bytes.
 * @param bufsize Number of bytes available in @p buffer.
 */
static void usb_hid_stage_busy_response(uint8_t const *buffer, uint16_t bufsize) {
    usb_hid_command_t request = {0};
    uint16_t copy_len = bufsize;

    if (copy_len > sizeof(request)) {
        copy_len = (uint16_t)sizeof(request);
    }

    memcpy(&request, buffer, copy_len);
    usb_hid_prepare_response(&hid_report_state, &request, USB_HID_STATUS_BUSY);
    hid_response_ready = true;
}

/** @brief Publish one prepared HID response for the next input report read. */
static void usb_hid_set_response(const usb_hid_command_t *response) {
    if (response == NULL) {
        return;
    }

    hid_report_state = *response;
    hid_response_ready = true;
}

/** @brief Initialize one HID response from the request opcode, sequence, and status. */
static void usb_hid_prepare_response(usb_hid_command_t *response, const usb_hid_command_t *request, usb_hid_status_t status) {
    memset(response, 0, sizeof(*response));
    response->opcode = request->opcode;
    response->sequence = request->sequence;
    response->status = (uint8_t)status;
}

/** @copydoc usb_hid_poll */
void usb_hid_poll(void) {
    usb_hid_command_t response;

    if (!hid_command_pending || hid_response_ready) {
        return;
    }

    hid_command_pending = false;

    usb_hid_prepare_response(&response, &hid_command_state, USB_HID_STATUS_OK);

    if (usb_hid_dispatch_command(&response, &hid_command_state)) {
        usb_hid_set_response(&response);
        return;
    }

    usb_hid_stats.unknown_opcodes += 1u;
    response.status = USB_HID_STATUS_UNKNOWN_COMMAND;
    usb_hid_set_response(&response);
}

uint16_t tud_hid_get_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t *buffer,
    uint16_t reqlen
) {
    uint16_t copy_len = reqlen;

    (void)instance;
    (void)report_id;
    (void)report_type;

    if (copy_len > sizeof(hid_report_state)) {
        copy_len = (uint16_t)sizeof(hid_report_state);
    }

    memcpy(buffer, &hid_report_state, copy_len);
    hid_response_ready = false;
    return copy_len;
}

void tud_hid_set_report_cb(
    uint8_t instance,
    uint8_t report_id,
    hid_report_type_t report_type,
    uint8_t const *buffer,
    uint16_t bufsize
) {
    uint16_t copy_len = bufsize;

    (void)instance;
    (void)report_id;
    (void)report_type;

    if (copy_len > sizeof(hid_report_state)) {
        copy_len = (uint16_t)sizeof(hid_report_state);
    }

    /* ponytail: HID control currently keeps exactly one staged command and one staged response.
     * That is acceptable now because host control traffic is bounded and the main loop services HID
     * every pass. If host burst depth becomes a real problem, replace this backpressure rule with a
     * small request/response ring so additional reports are queued instead of rejected.
     */
    if (hid_command_pending) {
        usb_hid_stats.busy_rejects += 1u;
        if (!hid_response_ready) {
            usb_hid_stage_busy_response(buffer, copy_len);
        }
        return;
    }

    if (hid_response_ready) {
        usb_hid_stats.busy_rejects += 1u;
        return;
    }

    memset(&hid_command_state, 0, sizeof(hid_command_state));
    memcpy(&hid_command_state, buffer, copy_len);
    hid_command_pending = true;
}

usb_hid_stats_t usb_hid_get_stats(void) {
    return usb_hid_stats;
}

void usb_hid_reset_stats(void) {
    memset(&usb_hid_stats, 0, sizeof(usb_hid_stats));
}