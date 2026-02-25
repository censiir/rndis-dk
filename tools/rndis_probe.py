#!/usr/bin/env python3
"""User-space RNDIS control-plane probe using libusb via ctypes.

This script is dependency-free (no pyusb needed) and exercises the same
initialize/query/set/keepalive sequence as the DriverKit extension.
"""

from __future__ import annotations

import argparse
import ctypes as ct
import struct
import sys
from dataclasses import dataclass


RNDIS_CONTROL_CLASS = 0xE0
RNDIS_CONTROL_SUBCLASS = 0x01
RNDIS_CONTROL_PROTOCOL = 0x03
RNDIS_DATA_CLASS = 0x0A

CDC_SEND_ENCAPSULATED_COMMAND = 0x00
CDC_GET_ENCAPSULATED_RESPONSE = 0x01

RNDIS_MSG_INITIALIZE = 0x00000002
RNDIS_MSG_HALT = 0x00000003
RNDIS_MSG_QUERY = 0x00000004
RNDIS_MSG_SET = 0x00000005
RNDIS_MSG_KEEPALIVE = 0x00000008
RNDIS_MSG_PACKET = 0x00000001

RNDIS_MSG_INITIALIZE_CMPLT = 0x80000002
RNDIS_MSG_QUERY_CMPLT = 0x80000004
RNDIS_MSG_SET_CMPLT = 0x80000005
RNDIS_MSG_KEEPALIVE_CMPLT = 0x80000008
RNDIS_MSG_INDICATE_STATUS = 0x00000007

RNDIS_STATUS_SUCCESS = 0x00000000

OID_8023_PERMANENT_ADDRESS = 0x01010101
OID_8023_CURRENT_ADDRESS = 0x01010102
OID_GEN_CURRENT_PACKET_FILTER = 0x0001010E

PACKET_FILTER_DIRECTED = 0x00000001
PACKET_FILTER_ALL_MULTICAST = 0x00000004
PACKET_FILTER_BROADCAST = 0x00000008

CONTROL_BUFFER_CAPACITY = 4096
LIBUSB_ERROR_TIMEOUT = -7
RNDIS_PACKET_HEADER_LENGTH = 44
ETHERNET_MIN_FRAME_LENGTH = 60


class libusb_context(ct.Structure):
    pass


class libusb_device(ct.Structure):
    pass


class libusb_device_handle(ct.Structure):
    pass


class libusb_endpoint_descriptor(ct.Structure):
    _fields_ = [
        ("bLength", ct.c_uint8),
        ("bDescriptorType", ct.c_uint8),
        ("bEndpointAddress", ct.c_uint8),
        ("bmAttributes", ct.c_uint8),
        ("wMaxPacketSize", ct.c_uint16),
        ("bInterval", ct.c_uint8),
        ("bRefresh", ct.c_uint8),
        ("bSynchAddress", ct.c_uint8),
        ("extra", ct.POINTER(ct.c_uint8)),
        ("extra_length", ct.c_int),
    ]


class libusb_interface_descriptor(ct.Structure):
    _fields_ = [
        ("bLength", ct.c_uint8),
        ("bDescriptorType", ct.c_uint8),
        ("bInterfaceNumber", ct.c_uint8),
        ("bAlternateSetting", ct.c_uint8),
        ("bNumEndpoints", ct.c_uint8),
        ("bInterfaceClass", ct.c_uint8),
        ("bInterfaceSubClass", ct.c_uint8),
        ("bInterfaceProtocol", ct.c_uint8),
        ("iInterface", ct.c_uint8),
        ("endpoint", ct.POINTER(libusb_endpoint_descriptor)),
        ("extra", ct.POINTER(ct.c_uint8)),
        ("extra_length", ct.c_int),
    ]


class libusb_interface(ct.Structure):
    _fields_ = [
        ("altsetting", ct.POINTER(libusb_interface_descriptor)),
        ("num_altsetting", ct.c_int),
    ]


class libusb_config_descriptor(ct.Structure):
    _fields_ = [
        ("bLength", ct.c_uint8),
        ("bDescriptorType", ct.c_uint8),
        ("wTotalLength", ct.c_uint16),
        ("bNumInterfaces", ct.c_uint8),
        ("bConfigurationValue", ct.c_uint8),
        ("iConfiguration", ct.c_uint8),
        ("bmAttributes", ct.c_uint8),
        ("MaxPower", ct.c_uint8),
        ("interface", ct.POINTER(libusb_interface)),
        ("extra", ct.POINTER(ct.c_uint8)),
        ("extra_length", ct.c_int),
    ]


class libusb_device_descriptor(ct.Structure):
    _fields_ = [
        ("bLength", ct.c_uint8),
        ("bDescriptorType", ct.c_uint8),
        ("bcdUSB", ct.c_uint16),
        ("bDeviceClass", ct.c_uint8),
        ("bDeviceSubClass", ct.c_uint8),
        ("bDeviceProtocol", ct.c_uint8),
        ("bMaxPacketSize0", ct.c_uint8),
        ("idVendor", ct.c_uint16),
        ("idProduct", ct.c_uint16),
        ("bcdDevice", ct.c_uint16),
        ("iManufacturer", ct.c_uint8),
        ("iProduct", ct.c_uint8),
        ("iSerialNumber", ct.c_uint8),
        ("bNumConfigurations", ct.c_uint8),
    ]


@dataclass
class RNDISCandidate:
    device: ct.POINTER(libusb_device)
    vid: int
    pid: int
    control_ifnum: int
    data_ifnum: int | None
    data_altsetting: int | None
    bulk_in_ep: int | None
    bulk_out_ep: int | None
    path_index: int


class ProbeError(RuntimeError):
    pass


class LibUSB:
    def __init__(self) -> None:
        self.lib = ct.CDLL("/opt/homebrew/lib/libusb-1.0.dylib")
        self._bind()

        self.ctx = ct.POINTER(libusb_context)()
        rc = self.lib.libusb_init(ct.byref(self.ctx))
        if rc != 0:
            raise ProbeError(f"libusb_init failed: {self.error_name(rc)} ({rc})")

    def _bind(self) -> None:
        lib = self.lib

        lib.libusb_init.argtypes = [ct.POINTER(ct.POINTER(libusb_context))]
        lib.libusb_init.restype = ct.c_int

        lib.libusb_exit.argtypes = [ct.POINTER(libusb_context)]
        lib.libusb_exit.restype = None

        lib.libusb_get_device_list.argtypes = [
            ct.POINTER(libusb_context),
            ct.POINTER(ct.POINTER(ct.POINTER(libusb_device))),
        ]
        lib.libusb_get_device_list.restype = ct.c_ssize_t

        lib.libusb_free_device_list.argtypes = [ct.POINTER(ct.POINTER(libusb_device)), ct.c_int]
        lib.libusb_free_device_list.restype = None

        lib.libusb_get_device_descriptor.argtypes = [
            ct.POINTER(libusb_device),
            ct.POINTER(libusb_device_descriptor),
        ]
        lib.libusb_get_device_descriptor.restype = ct.c_int

        lib.libusb_get_active_config_descriptor.argtypes = [
            ct.POINTER(libusb_device),
            ct.POINTER(ct.POINTER(libusb_config_descriptor)),
        ]
        lib.libusb_get_active_config_descriptor.restype = ct.c_int

        lib.libusb_free_config_descriptor.argtypes = [ct.POINTER(libusb_config_descriptor)]
        lib.libusb_free_config_descriptor.restype = None

        lib.libusb_open.argtypes = [
            ct.POINTER(libusb_device),
            ct.POINTER(ct.POINTER(libusb_device_handle)),
        ]
        lib.libusb_open.restype = ct.c_int

        lib.libusb_reset_device.argtypes = [ct.POINTER(libusb_device_handle)]
        lib.libusb_reset_device.restype = ct.c_int

        lib.libusb_ref_device.argtypes = [ct.POINTER(libusb_device)]
        lib.libusb_ref_device.restype = ct.POINTER(libusb_device)

        lib.libusb_unref_device.argtypes = [ct.POINTER(libusb_device)]
        lib.libusb_unref_device.restype = None

        lib.libusb_close.argtypes = [ct.POINTER(libusb_device_handle)]
        lib.libusb_close.restype = None

        lib.libusb_claim_interface.argtypes = [ct.POINTER(libusb_device_handle), ct.c_int]
        lib.libusb_claim_interface.restype = ct.c_int

        lib.libusb_release_interface.argtypes = [ct.POINTER(libusb_device_handle), ct.c_int]
        lib.libusb_release_interface.restype = ct.c_int

        lib.libusb_set_interface_alt_setting.argtypes = [ct.POINTER(libusb_device_handle), ct.c_int, ct.c_int]
        lib.libusb_set_interface_alt_setting.restype = ct.c_int

        if hasattr(lib, "libusb_set_auto_detach_kernel_driver"):
            lib.libusb_set_auto_detach_kernel_driver.argtypes = [
                ct.POINTER(libusb_device_handle),
                ct.c_int,
            ]
            lib.libusb_set_auto_detach_kernel_driver.restype = ct.c_int

        lib.libusb_control_transfer.argtypes = [
            ct.POINTER(libusb_device_handle),
            ct.c_uint8,
            ct.c_uint8,
            ct.c_uint16,
            ct.c_uint16,
            ct.POINTER(ct.c_uint8),
            ct.c_uint16,
            ct.c_uint,
        ]
        lib.libusb_control_transfer.restype = ct.c_int

        lib.libusb_bulk_transfer.argtypes = [
            ct.POINTER(libusb_device_handle),
            ct.c_uint8,
            ct.POINTER(ct.c_uint8),
            ct.c_int,
            ct.POINTER(ct.c_int),
            ct.c_uint,
        ]
        lib.libusb_bulk_transfer.restype = ct.c_int

        lib.libusb_error_name.argtypes = [ct.c_int]
        lib.libusb_error_name.restype = ct.c_char_p

    def close(self) -> None:
        if self.ctx:
            self.lib.libusb_exit(self.ctx)
            self.ctx = ct.POINTER(libusb_context)()

    def error_name(self, rc: int) -> str:
        raw = self.lib.libusb_error_name(rc)
        if not raw:
            return "unknown"
        return raw.decode("utf-8", errors="replace")


def parse_query_information_buffer(response: bytes) -> bytes:
    if len(response) < 24:
        raise ProbeError(f"query complete too short: {len(response)} bytes")

    _, message_length, _, status, info_len, info_offset = struct.unpack_from("<IIIIII", response, 0)

    if message_length > len(response) or message_length < 24:
        raise ProbeError(f"invalid query message length: {message_length} for rx={len(response)}")

    if status != RNDIS_STATUS_SUCCESS:
        raise ProbeError(f"query failed with RNDIS status 0x{status:08x}")

    def in_bounds(start: int, size: int) -> bool:
        end = start + size
        return start >= 0 and size >= 0 and end >= start and end <= message_length

    # Spec: offset is relative to start of RequestId field (byte 8).
    primary_start = 8 + info_offset
    if in_bounds(primary_start, info_len):
        return response[primary_start : primary_start + info_len]

    # Compatibility fallback for devices/reporting that effectively shift by 4.
    compat_start = 12 + info_offset
    if in_bounds(compat_start, info_len):
        return response[compat_start : compat_start + info_len]

    raise ProbeError(
        f"query payload out of bounds: info_len={info_len} info_offset={info_offset} msg_len={message_length}"
    )


def hexdump(data: bytes, width: int = 16) -> str:
    lines: list[str] = []
    for i in range(0, len(data), width):
        chunk = data[i : i + width]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        ascii_part = "".join(chr(b) if 32 <= b <= 126 else "." for b in chunk)
        lines.append(f"{i:04x}  {hex_part:<{width * 3}}  {ascii_part}")
    return "\n".join(lines)


def wrap_rndis_packet(ethernet_frame: bytes) -> bytes:
    if len(ethernet_frame) < ETHERNET_MIN_FRAME_LENGTH:
        ethernet_frame = ethernet_frame + bytes(ETHERNET_MIN_FRAME_LENGTH - len(ethernet_frame))

    data_offset = 36  # Data starts at byte 44, offset is from byte 8.
    message_length = RNDIS_PACKET_HEADER_LENGTH + len(ethernet_frame)
    header = struct.pack(
        "<IIIIIIIIIII",
        RNDIS_MSG_PACKET,
        message_length,
        data_offset,
        len(ethernet_frame),
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    )
    return header + ethernet_frame


def unwrap_rndis_packets(payload: bytes) -> list[bytes]:
    packets: list[bytes] = []
    cursor = 0
    while (cursor + 8) <= len(payload):
        msg_type, msg_len = struct.unpack_from("<II", payload, cursor)
        if msg_type != RNDIS_MSG_PACKET or msg_len < RNDIS_PACKET_HEADER_LENGTH:
            break
        if (cursor + msg_len) > len(payload):
            break

        msg = payload[cursor : cursor + msg_len]
        data_offset, data_len = struct.unpack_from("<II", msg, 8)
        data_start = 8 + data_offset
        data_end = data_start + data_len
        if data_start < RNDIS_PACKET_HEADER_LENGTH or data_end > len(msg):
            break

        packets.append(msg[data_start:data_end])
        cursor += msg_len

    return packets


class RNDISSession:
    def __init__(
        self,
        usb: LibUSB,
        candidate: RNDISCandidate,
        timeout_ms: int,
        polls: int,
        verbose: bool,
    ) -> None:
        self.usb = usb
        self.candidate = candidate
        self.timeout_ms = timeout_ms
        self.polls = polls
        self.verbose = verbose

        self.handle = ct.POINTER(libusb_device_handle)()
        self.next_request_id = 1

        rc = self.usb.lib.libusb_open(candidate.device, ct.byref(self.handle))
        if rc != 0:
            raise ProbeError(f"libusb_open failed: {usb.error_name(rc)} ({rc})")

        if hasattr(self.usb.lib, "libusb_set_auto_detach_kernel_driver"):
            self.usb.lib.libusb_set_auto_detach_kernel_driver(self.handle, 1)

        self.claimed_ifnums: list[int] = []
        self._claim(candidate.control_ifnum)
        if candidate.data_ifnum is not None and candidate.data_ifnum != candidate.control_ifnum:
            self._claim(candidate.data_ifnum)
            if candidate.data_altsetting is not None:
                rc = self.usb.lib.libusb_set_interface_alt_setting(
                    self.handle, candidate.data_ifnum, candidate.data_altsetting
                )
                if rc != 0:
                    raise ProbeError(
                        f"set alt setting {candidate.data_ifnum}:{candidate.data_altsetting} failed: "
                        f"{self.usb.error_name(rc)} ({rc})"
                    )

    def _claim(self, ifnum: int) -> None:
        rc = self.usb.lib.libusb_claim_interface(self.handle, ifnum)
        if rc != 0:
            raise ProbeError(f"claim interface {ifnum} failed: {self.usb.error_name(rc)} ({rc})")
        self.claimed_ifnums.append(ifnum)

    def close(self) -> None:
        for ifnum in reversed(self.claimed_ifnums):
            self.usb.lib.libusb_release_interface(self.handle, ifnum)
        self.claimed_ifnums.clear()
        if self.handle:
            self.usb.lib.libusb_close(self.handle)
            self.handle = ct.POINTER(libusb_device_handle)()

    def _send_control(self, bm_request_type: int, b_request: int, payload: bytes, w_index: int) -> bytes:
        if len(payload) > CONTROL_BUFFER_CAPACITY:
            raise ProbeError(f"control payload too large: {len(payload)}")

        buf = (ct.c_uint8 * max(len(payload), 1))()
        for i, byte in enumerate(payload):
            buf[i] = byte

        rc = self.usb.lib.libusb_control_transfer(
            self.handle,
            bm_request_type,
            b_request,
            0,
            w_index,
            buf,
            len(payload),
            self.timeout_ms,
        )

        if rc < 0:
            if rc == LIBUSB_ERROR_TIMEOUT and bm_request_type == 0xA1:
                return b""
            raise ProbeError(
                f"control transfer failed bm=0x{bm_request_type:02x} req=0x{b_request:02x}: "
                f"{self.usb.error_name(rc)} ({rc})"
            )

        if bm_request_type == 0x21 and rc != len(payload):
            raise ProbeError(f"short SEND_ENCAPSULATED_COMMAND write: wrote={rc} expected={len(payload)}")

        return bytes(buf[:rc])

    def send_encapsulated_command(self, payload: bytes) -> None:
        self._send_control(0x21, CDC_SEND_ENCAPSULATED_COMMAND, payload, self.candidate.control_ifnum)
        if self.verbose:
            print(f">>> SEND {len(payload)} bytes\n{hexdump(payload)}")

    def get_encapsulated_response(self) -> bytes:
        rx = self._send_control(
            0xA1,
            CDC_GET_ENCAPSULATED_RESPONSE,
            bytes(CONTROL_BUFFER_CAPACITY),
            self.candidate.control_ifnum,
        )
        if self.verbose and rx:
            print(f"<<< RECV {len(rx)} bytes\n{hexdump(rx)}")
        return rx

    def _bulk_transfer(self, endpoint: int, payload: bytes, transfer_len: int | None = None) -> bytes:
        if transfer_len is None:
            transfer_len = len(payload)
        if transfer_len <= 0:
            raise ProbeError("bulk transfer length must be positive")

        buf = (ct.c_uint8 * transfer_len)()
        if payload:
            copy_len = min(len(payload), transfer_len)
            for i in range(copy_len):
                buf[i] = payload[i]

        transferred = ct.c_int(0)
        rc = self.usb.lib.libusb_bulk_transfer(
            self.handle,
            endpoint,
            buf,
            transfer_len,
            ct.byref(transferred),
            self.timeout_ms,
        )
        if rc < 0:
            if rc == LIBUSB_ERROR_TIMEOUT and (endpoint & 0x80):
                return b""
            raise ProbeError(f"bulk transfer failed ep=0x{endpoint:02x}: {self.usb.error_name(rc)} ({rc})")

        return bytes(buf[: transferred.value])

    def _next_request_id(self) -> int:
        req_id = self.next_request_id
        self.next_request_id = (self.next_request_id + 1) & 0xFFFFFFFF
        if self.next_request_id == 0:
            self.next_request_id = 1
        return req_id

    def transaction(self, request: bytes, expected_msg_type: int, expected_request_id: int) -> bytes:
        self.send_encapsulated_command(request)

        for _ in range(self.polls):
            response = self.get_encapsulated_response()
            if len(response) < 8:
                continue

            msg_type, msg_len = struct.unpack_from("<II", response, 0)
            if msg_len < 8 or msg_len > len(response):
                raise ProbeError(f"invalid response header: type=0x{msg_type:08x} msg_len={msg_len} rx={len(response)}")

            response = response[:msg_len]
            if msg_type == RNDIS_MSG_INDICATE_STATUS:
                continue

            if expected_msg_type and msg_type != expected_msg_type:
                raise ProbeError(
                    f"unexpected response type: got=0x{msg_type:08x} expected=0x{expected_msg_type:08x}"
                )

            if expected_request_id:
                if len(response) < 12:
                    raise ProbeError("response too short to contain request id")
                response_request_id = struct.unpack_from("<I", response, 8)[0]
                if response_request_id != expected_request_id:
                    continue

            return response

        raise ProbeError("timed out waiting for matching encapsulated response")

    def initialize(self) -> int:
        req_id = self._next_request_id()
        request = struct.pack(
            "<IIIIII",
            RNDIS_MSG_INITIALIZE,
            24,
            req_id,
            1,
            0,
            0x4000,
        )
        response = self.transaction(request, RNDIS_MSG_INITIALIZE_CMPLT, req_id)
        if len(response) < 52:
            raise ProbeError(f"initialize complete too short: {len(response)} bytes")

        _, _, _, status, _, _, _, _, _, max_transfer_size, _, _, _ = struct.unpack_from("<IIIIIIIIIIIII", response, 0)
        if status != RNDIS_STATUS_SUCCESS:
            raise ProbeError(f"initialize failed with RNDIS status 0x{status:08x}")

        return max_transfer_size

    def query_oid(self, oid: int) -> bytes:
        req_id = self._next_request_id()
        request = struct.pack(
            "<IIIIIII",
            RNDIS_MSG_QUERY,
            28,
            req_id,
            oid,
            0,
            0,
            0,
        )
        response = self.transaction(request, RNDIS_MSG_QUERY_CMPLT, req_id)
        return parse_query_information_buffer(response)

    def set_oid(self, oid: int, payload: bytes) -> None:
        if len(payload) > 256:
            raise ProbeError(f"set payload too large: {len(payload)}")

        req_id = self._next_request_id()
        info_offset = 20  # From RequestId field (offset 8) to payload start.
        request_header = struct.pack(
            "<IIIIIII",
            RNDIS_MSG_SET,
            28 + len(payload),
            req_id,
            oid,
            len(payload),
            info_offset,
            0,
        )
        response = self.transaction(request_header + payload, RNDIS_MSG_SET_CMPLT, req_id)
        if len(response) < 16:
            raise ProbeError(f"set complete too short: {len(response)} bytes")

        _, _, _, status = struct.unpack_from("<IIII", response, 0)
        if status != RNDIS_STATUS_SUCCESS:
            raise ProbeError(f"set OID 0x{oid:08x} failed with RNDIS status 0x{status:08x}")

    def keepalive(self) -> None:
        req_id = self._next_request_id()
        request = struct.pack("<III", RNDIS_MSG_KEEPALIVE, 12, req_id)
        response = self.transaction(request, RNDIS_MSG_KEEPALIVE_CMPLT, req_id)
        if len(response) < 16:
            raise ProbeError(f"keepalive complete too short: {len(response)} bytes")
        _, _, _, status = struct.unpack_from("<IIII", response, 0)
        if status != RNDIS_STATUS_SUCCESS:
            raise ProbeError(f"keepalive failed with RNDIS status 0x{status:08x}")

    def halt(self) -> None:
        request = struct.pack("<II", RNDIS_MSG_HALT, 8)
        self.send_encapsulated_command(request)

    def send_rndis_packet(self, ethernet_frame: bytes) -> int:
        if self.candidate.bulk_out_ep is None:
            raise ProbeError("candidate has no bulk OUT endpoint")

        rndis_packet = wrap_rndis_packet(ethernet_frame)
        tx = self._bulk_transfer(self.candidate.bulk_out_ep, rndis_packet)
        if len(tx) != len(rndis_packet):
            raise ProbeError(f"short bulk write: wrote={len(tx)} expected={len(rndis_packet)}")
        return len(tx)

    def receive_rndis_packets(self, read_size: int, attempts: int) -> list[bytes]:
        if self.candidate.bulk_in_ep is None:
            raise ProbeError("candidate has no bulk IN endpoint")
        if read_size <= 0:
            raise ProbeError("read_size must be positive")
        if attempts <= 0:
            raise ProbeError("attempts must be positive")

        packets: list[bytes] = []
        for _ in range(attempts):
            raw = self._bulk_transfer(self.candidate.bulk_in_ep, bytes(read_size), transfer_len=read_size)
            if not raw:
                continue
            decoded = unwrap_rndis_packets(raw)
            if decoded:
                packets.extend(decoded)
        return packets

    def bulk_smoke_test(self, src_mac: bytes, read_size: int, read_attempts: int) -> tuple[int, list[bytes]]:
        if len(src_mac) < 6:
            raise ProbeError("source MAC is too short for Ethernet test frame")

        dst_mac = bytes.fromhex("ff ff ff ff ff ff")
        ethertype = b"\x88\xb5"  # Experimental local-use Ethertype.
        payload = bytes((i & 0xFF) for i in range(46))
        frame = dst_mac + src_mac[:6] + ethertype + payload

        tx_len = self.send_rndis_packet(frame)
        rx_packets = self.receive_rndis_packets(read_size=read_size, attempts=read_attempts)
        return tx_len, rx_packets


def _find_bulk_endpoints(alt: libusb_interface_descriptor) -> tuple[int | None, int | None]:
    bulk_in: int | None = None
    bulk_out: int | None = None

    for ep_idx in range(alt.bNumEndpoints):
        ep = alt.endpoint[ep_idx]
        transfer_type = ep.bmAttributes & 0x03
        if transfer_type != 0x02:
            continue
        ep_addr = int(ep.bEndpointAddress)
        if (ep_addr & 0x80) and bulk_in is None:
            bulk_in = ep_addr
        elif (ep_addr & 0x80) == 0 and bulk_out is None:
            bulk_out = ep_addr

    return bulk_in, bulk_out


def find_candidate_interfaces(
    config: libusb_config_descriptor,
) -> tuple[int | None, int | None, int | None, int | None, int | None]:
    control_ifnum: int | None = None
    data_ifnum: int | None = None
    data_altsetting: int | None = None
    bulk_in_ep: int | None = None
    bulk_out_ep: int | None = None

    for i in range(config.bNumInterfaces):
        iface = config.interface[i]
        for alt_index in range(iface.num_altsetting):
            alt = iface.altsetting[alt_index]
            if (
                alt.bInterfaceClass == RNDIS_CONTROL_CLASS
                and alt.bInterfaceSubClass == RNDIS_CONTROL_SUBCLASS
                and alt.bInterfaceProtocol == RNDIS_CONTROL_PROTOCOL
            ):
                control_ifnum = int(alt.bInterfaceNumber)
            elif alt.bInterfaceClass == RNDIS_DATA_CLASS:
                in_ep, out_ep = _find_bulk_endpoints(alt)
                if data_ifnum is None:
                    data_ifnum = int(alt.bInterfaceNumber)
                    data_altsetting = int(alt.bAlternateSetting)
                    bulk_in_ep = in_ep
                    bulk_out_ep = out_ep

                if in_ep is not None and out_ep is not None:
                    data_ifnum = int(alt.bInterfaceNumber)
                    data_altsetting = int(alt.bAlternateSetting)
                    bulk_in_ep = in_ep
                    bulk_out_ep = out_ep
                    break

    return control_ifnum, data_ifnum, data_altsetting, bulk_in_ep, bulk_out_ep


def enumerate_candidates(usb: LibUSB, vid: int | None, pid: int | None) -> list[RNDISCandidate]:
    device_list = ct.POINTER(ct.POINTER(libusb_device))()
    count = usb.lib.libusb_get_device_list(usb.ctx, ct.byref(device_list))
    if count < 0:
        raise ProbeError(f"libusb_get_device_list failed: {usb.error_name(int(count))} ({int(count)})")

    candidates: list[RNDISCandidate] = []
    try:
        for i in range(int(count)):
            dev = device_list[i]
            desc = libusb_device_descriptor()
            rc = usb.lib.libusb_get_device_descriptor(dev, ct.byref(desc))
            if rc != 0:
                continue

            if vid is not None and desc.idVendor != vid:
                continue
            if pid is not None and desc.idProduct != pid:
                continue

            cfg = ct.POINTER(libusb_config_descriptor)()
            rc = usb.lib.libusb_get_active_config_descriptor(dev, ct.byref(cfg))
            if rc != 0 or not cfg:
                continue

            try:
                control_ifnum, data_ifnum, data_altsetting, bulk_in_ep, bulk_out_ep = find_candidate_interfaces(
                    cfg.contents
                )
                if control_ifnum is None:
                    continue

                candidates.append(
                    RNDISCandidate(
                        device=usb.lib.libusb_ref_device(dev),
                        vid=int(desc.idVendor),
                        pid=int(desc.idProduct),
                        control_ifnum=control_ifnum,
                        data_ifnum=data_ifnum,
                        data_altsetting=data_altsetting,
                        bulk_in_ep=bulk_in_ep,
                        bulk_out_ep=bulk_out_ep,
                        path_index=i,
                    )
                )
            finally:
                usb.lib.libusb_free_config_descriptor(cfg)
    finally:
        usb.lib.libusb_free_device_list(device_list, 1)

    return candidates


def release_candidates(usb: LibUSB, candidates: list[RNDISCandidate]) -> None:
    for candidate in candidates:
        if candidate.device:
            usb.lib.libusb_unref_device(candidate.device)
            candidate.device = ct.POINTER(libusb_device)()


def reset_candidate_device(usb: LibUSB, candidate: RNDISCandidate) -> None:
    handle = ct.POINTER(libusb_device_handle)()
    rc = usb.lib.libusb_open(candidate.device, ct.byref(handle))
    if rc != 0:
        raise ProbeError(f"libusb_open for reset failed: {usb.error_name(rc)} ({rc})")

    try:
        rc = usb.lib.libusb_reset_device(handle)
        if rc != 0:
            raise ProbeError(f"libusb_reset_device failed: {usb.error_name(rc)} ({rc})")
    finally:
        usb.lib.libusb_close(handle)


def parse_int_opt(value: str) -> int:
    return int(value, 0)


def run_self_test() -> None:
    payload = bytes.fromhex("de ad be ef 01 02")
    request_id = 9

    # Primary/spec layout.
    msg = struct.pack(
        "<IIIIII",
        RNDIS_MSG_QUERY_CMPLT,
        24 + len(payload),
        request_id,
        RNDIS_STATUS_SUCCESS,
        len(payload),
        16,
    ) + payload
    if parse_query_information_buffer(msg) != payload:
        raise ProbeError("self-test failed: spec payload decode mismatch")

    # Out-of-bounds payload should fail.
    bad_msg = struct.pack(
        "<IIIIII",
        RNDIS_MSG_QUERY_CMPLT,
        24 + len(payload),
        request_id,
        RNDIS_STATUS_SUCCESS,
        len(payload),
        0x1000,
    ) + payload
    try:
        parse_query_information_buffer(bad_msg)
        raise ProbeError("self-test failed: malformed message did not raise")
    except ProbeError:
        pass

    frame = bytes.fromhex("ff ff ff ff ff ff 00 11 22 33 44 55 08 00") + bytes(46)
    wrapped = wrap_rndis_packet(frame)
    unwrapped = unwrap_rndis_packets(wrapped)
    if len(unwrapped) != 1 or unwrapped[0] != frame:
        raise ProbeError("self-test failed: packet wrap/unwrap mismatch")

    print("self-test passed")


def main() -> int:
    parser = argparse.ArgumentParser(description="RNDIS USB control-plane probe (libusb/ctypes)")
    parser.add_argument("--vid", type=parse_int_opt, help="USB vendor id (e.g. 0x18d1)")
    parser.add_argument("--pid", type=parse_int_opt, help="USB product id (e.g. 0x4ee7)")
    parser.add_argument("--device-index", type=int, default=0, help="index in filtered candidate list")
    parser.add_argument("--list", action="store_true", help="list RNDIS candidates and exit")
    parser.add_argument("--timeout-ms", type=int, default=3000, help="USB control transfer timeout")
    parser.add_argument("--polls", type=int, default=16, help="encapsulated response poll attempts")
    parser.add_argument("--skip-set-filter", action="store_true", help="do not set packet filter")
    parser.add_argument(
        "--packet-filter",
        type=parse_int_opt,
        default=PACKET_FILTER_DIRECTED | PACKET_FILTER_BROADCAST | PACKET_FILTER_ALL_MULTICAST,
        help="packet filter value for OID_GEN_CURRENT_PACKET_FILTER",
    )
    parser.add_argument("--halt-on-exit", action="store_true", help="send RNDIS HALT before exit")
    parser.add_argument("--verbose", action="store_true", help="hex dump control transfers")
    parser.add_argument("--bulk-smoke-test", action="store_true", help="exercise RNDIS packet bulk IN/OUT path")
    parser.add_argument("--bulk-read-size", type=int, default=16384, help="bulk IN read size when smoke testing")
    parser.add_argument("--bulk-read-attempts", type=int, default=8, help="bulk IN read attempts for smoke test")
    parser.add_argument("--reset-device", action="store_true", help="issue USB reset to force re-enumeration")
    parser.add_argument("--self-test", action="store_true", help="run parser self-test and exit")
    args = parser.parse_args()

    if args.self_test:
        run_self_test()
        return 0

    usb = LibUSB()
    candidates: list[RNDISCandidate] = []
    try:
        candidates = enumerate_candidates(usb, args.vid, args.pid)
        if args.list:
            if not candidates:
                print("no RNDIS candidates found")
                return 1

            for idx, candidate in enumerate(candidates):
                data_if = "none" if candidate.data_ifnum is None else str(candidate.data_ifnum)
                alt = "none" if candidate.data_altsetting is None else str(candidate.data_altsetting)
                bulk_in = "none" if candidate.bulk_in_ep is None else f"0x{candidate.bulk_in_ep:02x}"
                bulk_out = "none" if candidate.bulk_out_ep is None else f"0x{candidate.bulk_out_ep:02x}"
                print(
                    f"[{idx}] vid=0x{candidate.vid:04x} pid=0x{candidate.pid:04x} "
                    f"control_if={candidate.control_ifnum} data_if={data_if} alt={alt} "
                    f"bulk_in={bulk_in} bulk_out={bulk_out}"
                )
            return 0

        if not candidates:
            raise ProbeError("no RNDIS candidates found")

        if args.device_index < 0 or args.device_index >= len(candidates):
            raise ProbeError(f"device-index {args.device_index} out of range (0..{len(candidates) - 1})")

        candidate = candidates[args.device_index]
        print(
            f"selected device vid=0x{candidate.vid:04x} pid=0x{candidate.pid:04x} "
            f"control_if={candidate.control_ifnum} data_if={candidate.data_ifnum} "
            f"alt={candidate.data_altsetting} bulk_in={candidate.bulk_in_ep} bulk_out={candidate.bulk_out_ep}"
        )

        if args.reset_device:
            reset_candidate_device(usb, candidate)
            print("usb reset issued")
            return 0

        session = RNDISSession(usb, candidate, args.timeout_ms, args.polls, args.verbose)
        try:
            max_transfer_size = session.initialize()
            print(f"initialize ok: max_transfer_size={max_transfer_size}")

            try:
                mac = session.query_oid(OID_8023_CURRENT_ADDRESS)
                print(f"OID_8023_CURRENT_ADDRESS: {':'.join(f'{b:02x}' for b in mac[:6])}")
            except ProbeError:
                mac = session.query_oid(OID_8023_PERMANENT_ADDRESS)
                print(f"OID_8023_PERMANENT_ADDRESS: {':'.join(f'{b:02x}' for b in mac[:6])}")

            if not args.skip_set_filter:
                payload = struct.pack("<I", args.packet_filter)
                session.set_oid(OID_GEN_CURRENT_PACKET_FILTER, payload)
                print(f"set packet filter ok: 0x{args.packet_filter:08x}")

            session.keepalive()
            print("keepalive ok")

            if args.bulk_smoke_test:
                tx_len, rx_packets = session.bulk_smoke_test(
                    src_mac=mac,
                    read_size=args.bulk_read_size,
                    read_attempts=args.bulk_read_attempts,
                )
                print(f"bulk smoke tx ok: {tx_len} bytes")
                if rx_packets:
                    print(f"bulk smoke rx packets: {len(rx_packets)}")
                    first = rx_packets[0]
                    print(f"first rx packet len={len(first)}")
                    if args.verbose:
                        print(hexdump(first))
                else:
                    print("bulk smoke rx: no packets observed (timeouts/idle bus)")

            if args.halt_on_exit:
                session.halt()
                print("halt sent")
        finally:
            session.close()

    except ProbeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    finally:
        release_candidates(usb, candidates)
        usb.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
