from __future__ import annotations

import unittest
from unittest import mock

from picotrace.control.hid import (
    DEFAULT_HID_REPORT_SIZE,
    HidCommand,
    HidControlClient,
    HidOpcode,
    HidProtocolError,
    HidStatus,
    decode_hid_response,
    open_hid_control_device,
)


class HidControlTests(unittest.TestCase):
    def test_hid_command_packs_fixed_report(self) -> None:
        report = HidCommand(opcode=HidOpcode.GET_STATUS, sequence=7, payload=b"\xAA\xBB").to_report_bytes()

        self.assertEqual(len(report), DEFAULT_HID_REPORT_SIZE)
        self.assertEqual(report[:6], bytes([HidOpcode.GET_STATUS, 7, 0, 2, 0xAA, 0xBB]))
        self.assertEqual(report[6:], b"\x00" * (DEFAULT_HID_REPORT_SIZE - 6))

    def test_decode_hid_response_rejects_bad_payload_length(self) -> None:
        report = bytearray(DEFAULT_HID_REPORT_SIZE)
        report[2] = HidStatus.OK
        report[3] = DEFAULT_HID_REPORT_SIZE

        with self.assertRaises(HidProtocolError):
            decode_hid_response(report)

    def test_client_get_status_uses_hid_control_transfers(self) -> None:
        device = mock.Mock()

        def ctrl_transfer(bm_request_type, b_request, w_value, w_index, data_or_w_length, timeout=None):
            if b_request == 0x09:
                self.assertEqual(len(data_or_w_length), DEFAULT_HID_REPORT_SIZE)
                self.assertEqual(data_or_w_length[:4], bytes([HidOpcode.GET_STATUS, 0, 0, 0]))
                return DEFAULT_HID_REPORT_SIZE

            self.assertEqual(b_request, 0x01)
            self.assertEqual(data_or_w_length, DEFAULT_HID_REPORT_SIZE)
            response = bytearray(DEFAULT_HID_REPORT_SIZE)
            response[0] = HidOpcode.GET_STATUS
            response[1] = 0
            response[2] = HidStatus.OK
            response[3] = 1
            response[4] = 1
            return response

        device.ctrl_transfer.side_effect = ctrl_transfer
        client = HidControlClient(device=device, interface_number=3)

        status = client.get_status()

        self.assertTrue(status.stream_enabled)
        self.assertEqual(device.ctrl_transfer.call_count, 2)

    def test_open_hid_control_device_ignores_unsupported_kernel_driver_query(self) -> None:
        endpoint = mock.Mock(bEndpointAddress=0x84)
        interface = mock.Mock(bInterfaceClass=0x03, bInterfaceNumber=3)
        interface.__iter__ = mock.Mock(return_value=iter([endpoint]))
        device = mock.Mock()
        device.get_active_configuration.return_value = [interface]
        device.is_kernel_driver_active.side_effect = NotImplementedError()

        with mock.patch("picotrace.control.hid.find_usb_device", return_value=device), mock.patch(
            "picotrace.control.hid.usb.util.claim_interface"
        ) as claim_interface_mock:
            opened_device, interface_number = open_hid_control_device()

        self.assertIs(opened_device, device)
        self.assertEqual(interface_number, 3)
        claim_interface_mock.assert_called_once_with(device, 3)
