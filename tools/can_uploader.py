#!/usr/bin/env python3
import argparse
import pathlib
import struct
import sys
import time
import zlib

try:
    import can
except ImportError:
    can = None

HOST_CMD_ID = 0x101
HOST_DATA_ID = 0x102
HOST_SEQ_DATA_BASE_ID = 0x200
BOOT_RESP_ID = 0x181

APP_START_ADDR = 0x08010000

ACK = 0x79
NACK = 0x1F

CMD_PING = 0x01
CMD_INFO = 0x02
CMD_ERASE = 0x03
CMD_SET_ADDR = 0x04
CMD_DATA = 0x05
CMD_CRC = 0x06
CMD_BOOT = 0x07
CMD_RESET = 0x08

BOOTLOADER_VERSION_MAJOR = 0x00
APP_TEST_VERSION_MAJOR = 0xA0

APP_STATES = {
    0x00: "INVALID",
    0x01: "VALID",
    0x02: "UPDATE_IN_PROGRESS",
    0x03: "METADATA_ERASED",
    0x04: "CRC_FAILED",
}

ERROR_NAMES = {
    0x01: "UNKNOWN_CMD",
    0x02: "ERASE_FAILED",
    0x03: "INVALID_LENGTH",
    0x04: "INVALID_ADDRESS",
    0x05: "CHECKSUM",
    0x06: "SEQUENCE",
    0x07: "FLASH_WRITE",
    0x08: "APP_INVALID",
    0x09: "CRC_MISMATCH",
    0x0A: "METADATA_WRITE",
}


class BootloaderError(RuntimeError):
    pass


class BootloaderNackError(BootloaderError):
    pass


class BootloaderTimeoutError(BootloaderError):
    pass


class BootloaderSendError(BootloaderError):
    pass


class BootloaderTransferError(BootloaderError):
    pass


def checksum8(data):
    return sum(data) & 0xFF


def make_frame(cmd, payload=b""):
    data = bytes([cmd]) + payload
    if len(data) > 8:
        raise ValueError("CAN payload too long")
    return data.ljust(8, b"\x00")


class CanBootClient:
    def __init__(self, interface, channel, bitrate, timeout, erase_timeout,
                 retries):
        if can is None:
            raise BootloaderError("python-can is not installed")

        kwargs = {
            "interface": interface,
            "channel": channel,
        }
        if bitrate:
            kwargs["bitrate"] = bitrate

        self.bus = can.Bus(**kwargs)
        self.timeout = timeout
        self.erase_timeout = erase_timeout
        self.retries = retries

    def close(self):
        self.bus.shutdown()

    def drain_rx(self):
        while self.bus.recv(timeout=0.0) is not None:
            pass

    def send(self, data, arbitration_id=HOST_CMD_ID):
        try:
            self.bus.send(can.Message(arbitration_id=arbitration_id,
                                      data=data,
                                      is_extended_id=False))
        except can.CanError as exc:
            raise BootloaderSendError(str(exc)) from exc

    def recv_response(self, expected_cmd, timeout=None):
        if timeout is None:
            timeout = self.timeout

        deadline = time.monotonic() + timeout

        while time.monotonic() < deadline:
            msg = self.bus.recv(timeout=max(0.0, deadline - time.monotonic()))
            if msg is None:
                break
            if msg.is_extended_id or msg.arbitration_id != BOOT_RESP_ID:
                continue
            data = bytes(msg.data).ljust(8, b"\x00")
            if data[0] == ACK and data[1] == expected_cmd:
                return data
            if data[0] == NACK and data[1] == expected_cmd:
                error_name = ERROR_NAMES.get(data[2], "UNKNOWN_ERROR")
                raise BootloaderNackError(
                    f"NACK for cmd 0x{expected_cmd:02X}: "
                    f"{error_name} (0x{data[2]:02X})"
                )

        raise BootloaderTimeoutError(f"timeout waiting for cmd 0x{expected_cmd:02X}")

    def transact(self, arbitration_id, data, expected_cmd, timeout=None,
                 retryable=True):
        attempts = self.retries + 1 if retryable else 1
        last_error = None

        for attempt in range(attempts):
            self.drain_rx()
            try:
                self.send(data, arbitration_id=arbitration_id)
                return self.recv_response(expected_cmd, timeout=timeout)
            except BootloaderNackError:
                raise
            except (BootloaderTimeoutError, BootloaderSendError) as exc:
                last_error = exc
                if attempt + 1 >= attempts:
                    break
                time.sleep(0.02 * (attempt + 1))

        raise last_error

    def command(self, cmd, payload=b"", timeout=None, retryable=True):
        return self.transact(HOST_CMD_ID, make_frame(cmd, payload), cmd,
                             timeout=timeout, retryable=retryable)

    def ping(self):
        return self.command(CMD_PING)

    def info_page(self, page):
        return self.command(CMD_INFO, bytes([page]))

    def erase(self):
        return self.command(CMD_ERASE, timeout=self.erase_timeout)

    def set_addr(self, address):
        return self.command(CMD_SET_ADDR, struct.pack("<I", address))

    def write_word(self, seq, word_bytes):
        if len(word_bytes) != 4:
            raise ValueError("word_bytes must be exactly 4 bytes")
        header = bytes([CMD_DATA, seq & 0xFF, (seq >> 8) & 0xFF]) + word_bytes
        data = header + bytes([checksum8(header)])
        return self.transact(HOST_CMD_ID, data, CMD_DATA,
                             retryable=True)

    def write_data8(self, data, seq=None):
        if len(data) != 8:
            raise ValueError("data must be exactly 8 bytes")
        arbitration_id = HOST_DATA_ID
        if seq is not None:
            arbitration_id = HOST_SEQ_DATA_BASE_ID | (seq & 0xFF)
        return self.transact(arbitration_id, data, CMD_DATA,
                             retryable=(seq is not None))

    def crc(self, expected_crc):
        return self.command(CMD_CRC, struct.pack("<I", expected_crc))

    def boot(self):
        return self.command(CMD_BOOT, retryable=False)

    def reset(self):
        return self.command(CMD_RESET, retryable=False)


def pad_firmware(data, alignment):
    remainder = len(data) % alignment
    if remainder == 0:
        return data
    return data + (b"\xFF" * (alignment - remainder))


def upload(client, firmware, address, do_erase, do_crc, do_boot, legacy_data,
           raw_fast_data):
    response = client.ping()
    print(f"PING ok: version {response[2]}.{response[3]}")

    if response[2] == APP_TEST_VERSION_MAJOR:
        print("App is running; requesting bootloader reset...")
        client.reset()
        time.sleep(0.5)
        response = client.ping()
        print(f"PING ok: version {response[2]}.{response[3]}")

    if response[2] != BOOTLOADER_VERSION_MAJOR:
        raise BootloaderError(
            f"target did not enter bootloader, got version "
            f"{response[2]}.{response[3]}"
        )

    info0 = client.info_page(0)
    info1 = client.info_page(1)
    info2 = client.info_page(2)
    info3 = client.info_page(3)
    info4 = client.info_page(4)
    info5 = client.info_page(5)
    app_start = struct.unpack("<I", info1[3:7])[0]
    app_end = struct.unpack("<I", info2[3:7])[0]
    app_meta = struct.unpack("<I", info3[3:7])[0]
    app_state = APP_STATES.get(info4[3], f"UNKNOWN_0x{info4[3]:02X}")
    meta_size = struct.unpack("<I", info4[4:8])[0]
    meta_crc = struct.unpack("<I", info5[3:7])[0]
    print(
        "INFO ok: "
        f"protocol {info0[5]}.{info0[6]}, "
        f"flags 0x{info0[7]:02X}, "
        f"app 0x{app_start:08X}-0x{app_end:08X}, "
        f"meta 0x{app_meta:08X}, "
        f"state {app_state}, "
        f"size {meta_size}, "
        f"crc 0x{meta_crc:08X}"
    )

    if address != app_start:
        print(
            f"warning: upload address 0x{address:08X} differs from "
            f"bootloader app start 0x{app_start:08X}",
            file=sys.stderr
        )

    padded_for_size = pad_firmware(firmware, 4 if legacy_data else 8)
    if address < app_start or address + len(padded_for_size) > app_end:
        raise BootloaderError(
            f"firmware does not fit app region: "
            f"0x{address:08X}-0x{address + len(padded_for_size):08X}, "
            f"allowed 0x{app_start:08X}-0x{app_end:08X}"
        )

    if not do_erase and app_state != "VALID":
        raise BootloaderError(
            f"--no-erase is unsafe while app state is {app_state}"
        )

    if app_state != "VALID":
        print(f"Previous app state is {app_state}; a full upload is required")

    if do_erase:
        print("Erasing app region...")
        client.erase()
        print("ERASE ok")

    client.set_addr(address)
    print(f"SET_ADDR ok: 0x{address:08X}")

    if legacy_data:
        padded = padded_for_size
        total_words = len(padded) // 4

        for seq in range(total_words):
            start = seq * 4
            try:
                client.write_word(seq, padded[start:start + 4])
            except BootloaderError as exc:
                failed_addr = address + start
                raise BootloaderTransferError(
                    f"write failed at word {seq}/{total_words}, "
                    f"address 0x{failed_addr:08X}: {exc}"
                ) from exc
            if (seq + 1) % 256 == 0 or seq + 1 == total_words:
                print(f"Wrote {seq + 1}/{total_words} words")
    else:
        padded = padded_for_size
        total_frames = len(padded) // 8

        for frame in range(total_frames):
            start = frame * 8
            try:
                seq = None if raw_fast_data else frame
                client.write_data8(padded[start:start + 8], seq=seq)
            except BootloaderError as exc:
                failed_addr = address + start
                raise BootloaderTransferError(
                    f"write failed at data frame {frame}/{total_frames}, "
                    f"address 0x{failed_addr:08X}: {exc}"
                ) from exc
            if (frame + 1) % 128 == 0 or frame + 1 == total_frames:
                print(f"Wrote {frame + 1}/{total_frames} data frames")

    if do_crc:
        expected_crc = zlib.crc32(padded) & 0xFFFFFFFF
        client.crc(expected_crc)
        print(f"CRC ok: 0x{expected_crc:08X}")

    if do_boot:
        client.boot()
        print("BOOT ok")


def parse_args(argv):
    parser = argparse.ArgumentParser(description="STM32F446 CAN bootloader uploader")
    parser.add_argument("firmware", type=pathlib.Path, help="raw .bin firmware")
    parser.add_argument("--interface", default="socketcan", help="python-can interface")
    parser.add_argument("--channel", default="can0", help="CAN channel")
    parser.add_argument("--bitrate", type=int, default=None,
                        help="bitrate for interfaces that configure it themselves")
    parser.add_argument("--address", type=lambda x: int(x, 0), default=APP_START_ADDR)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--erase-timeout", type=float, default=10.0)
    parser.add_argument("--retries", type=int, default=3,
                        help="retry count for retry-safe transfers")
    parser.add_argument("--no-erase", action="store_true")
    parser.add_argument("--no-crc", action="store_true")
    parser.add_argument("--boot", action="store_true", help="send CMD_BOOT after upload")
    parser.add_argument("--legacy-data", action="store_true",
                        help="use CMD_DATA on 0x101 with 4 data bytes per frame")
    parser.add_argument("--raw-fast-data", action="store_true",
                        help="use raw 0x102 fast data frames without sequence")
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)

    if not args.firmware.exists():
        raise BootloaderError(f"firmware not found: {args.firmware}")

    firmware = args.firmware.read_bytes()
    if not firmware:
        raise BootloaderError("firmware is empty")

    if args.retries < 0:
        raise BootloaderError("--retries must be >= 0")

    client = CanBootClient(args.interface, args.channel, args.bitrate,
                           args.timeout, args.erase_timeout, args.retries)
    try:
        upload(client, firmware, args.address, not args.no_erase,
               not args.no_crc, args.boot, args.legacy_data,
               args.raw_fast_data)
    finally:
        client.close()


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except BootloaderError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
