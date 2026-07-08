#include "bsp/board_api.h"
#include "tusb.h"
#include <string.h>

#include "usb/usb_hid.h"

#define USB_VID 0xCafe
#define USB_PID 0x4003
#define USB_BCD  0x0210

#define USB_MS_VENDOR_REQUEST 0x01
#define USB_MS_OS_20_DESC_LEN 0x00B2
#define USB_BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

#define USB_STR_MANUFACTURER "PicoTrace"
#define USB_STR_PRODUCT "PicoTrace Protocol Tracer"
#define USB_STR_CDC "CDC Host Control"
#define USB_STR_VENDOR "Trace Data Stream"
#define USB_STR_HID "HID Control"

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_CDC_DATA,
    ITF_NUM_VENDOR,
    ITF_NUM_HID,
    ITF_NUM_TOTAL
};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_CDC,
    STRID_VENDOR,
    STRID_HID,
};

static uint8_t const hid_report_descriptor[] = {
    0x05, 0x01,
    0x09, 0x01,
    0xa1, 0x01,
    0x15, 0x00,
    0x26, 0xff, 0x00,
    0x75, 0x08,
    0x95, USB_HID_REPORT_SIZE,
    0x09, 0x01,
    0x81, 0x02,
    0x95, USB_HID_REPORT_SIZE,
    0x09, 0x01,
    0x91, 0x02,
    0xc0,
};

static tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0101,
    .iManufacturer = STRID_MANUFACTURER,
    .iProduct = STRID_PRODUCT,
    .iSerialNumber = STRID_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_VENDOR_OUT 0x03
#define EPNUM_VENDOR_IN  0x83
#define EPNUM_HID_IN     0x84
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_HID_DESC_LEN)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, STRID_CDC, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, STRID_VENDOR, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 64),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID, HID_ITF_PROTOCOL_NONE, sizeof(hid_report_descriptor), EPNUM_HID_IN, 64, 5),
};

static uint8_t const desc_bos[] = {
    TUD_BOS_DESCRIPTOR(USB_BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(USB_MS_OS_20_DESC_LEN, USB_MS_VENDOR_REQUEST),
};

static uint8_t const desc_ms_os_20[] = {
    U16_TO_U8S_LE(0x000A),
    U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000),
    U16_TO_U8S_LE(USB_MS_OS_20_DESC_LEN),

    U16_TO_U8S_LE(0x0008),
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0x00,
    0x00,
    U16_TO_U8S_LE(USB_MS_OS_20_DESC_LEN - 0x000A),

    U16_TO_U8S_LE(0x0008),
    U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    ITF_NUM_VENDOR,
    0x00,
    U16_TO_U8S_LE(USB_MS_OS_20_DESC_LEN - 0x000A - 0x0008),

    U16_TO_U8S_LE(0x0014),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    U16_TO_U8S_LE(USB_MS_OS_20_DESC_LEN - 0x000A - 0x0008 - 0x0008 - 0x0014),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007),
    U16_TO_U8S_LE(0x002A),
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00,
    'I', 0x00, 'n', 0x00, 't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00,
    'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00, 'U', 0x00, 'I', 0x00,
    'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050),
    '{', 0x00, 'D', 0x00, '6', 0x00, '9', 0x00, '4', 0x00, 'B', 0x00,
    '3', 0x00, 'F', 0x00, '7', 0x00, '-', 0x00, 'E', 0x00, '6', 0x00,
    '3', 0x00, '8', 0x00, '-', 0x00, '4', 0x00, 'A', 0x00, '7', 0x00,
    'D', 0x00, '-', 0x00, 'A', 0x00, '4', 0x00, 'D', 0x00, '8', 0x00,
    '-', 0x00, '8', 0x00, 'A', 0x00, '9', 0x00, '6', 0x00, '4', 0x00,
    '3', 0x00, 'D', 0x00, '8', 0x00, '4', 0x00, 'A', 0x00, '6', 0x00,
    '5', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == USB_MS_OS_20_DESC_LEN, "Incorrect Microsoft OS 2.0 descriptor size");

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

uint8_t const *tud_descriptor_bos_cb(void) {
    return desc_bos;
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    uint16_t total_len;

    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }

    if ((request->bmRequestType_bit.type != TUSB_REQ_TYPE_VENDOR) ||
        (request->bRequest != USB_MS_VENDOR_REQUEST) ||
        (request->wIndex != 0x0007)) {
        return false;
    }

    memcpy(&total_len, desc_ms_os_20 + 8, sizeof(total_len));
    return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20, total_len);
}

static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    USB_STR_MANUFACTURER,
    USB_STR_PRODUCT,
    NULL,
    USB_STR_CDC,
    USB_STR_VENDOR,
    USB_STR_HID,
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return hid_report_descriptor;
}

static uint16_t desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t chr_count;

    switch (index) {
    case STRID_LANGID:
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
        break;
    case STRID_SERIAL:
        chr_count = board_usb_get_serial(desc_str + 1, 32);
        break;
    default:
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
            return NULL;
        }
        const char *str = string_desc_arr[index];
        chr_count = strlen(str);
        for (size_t i = 0; i < chr_count; i++) {
            desc_str[1 + i] = (uint16_t)str[i];
        }
        break;
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}