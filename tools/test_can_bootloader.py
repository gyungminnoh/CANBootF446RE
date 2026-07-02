#!/usr/bin/env python3
import argparse
import pathlib
import struct
import sys
import time

from can_uploader import (
    APP_STATES,
    APP_TEST_VERSION_MAJOR,
    BOOTLOADER_VERSION_MAJOR,
    CMD_DATA,
    CMD_SET_ADDR,
    HOST_CMD_ID,
    HOST_SEQ_DATA_BASE_ID,
    BootloaderError,
    BootloaderNackError,
    CanBootClient,
    make_frame,
    upload,
)


class TestFailure(RuntimeError):
    pass


def expect(condition, message):
    if not condition:
        raise TestFailure(message)


def print_step(message):
    print(f"[TEST] {message}")


def ensure_bootloader(client):
    response = client.ping()
    print_step(f"PING version {response[2]}.{response[3]}")

    if response[2] == APP_TEST_VERSION_MAJOR:
        print_step("app is running, requesting bootloader reset")
        client.reset()
        time.sleep(0.5)
        response = client.ping()
        print_step(f"PING version {response[2]}.{response[3]}")

    expect(response[2] == BOOTLOADER_VERSION_MAJOR,
           "target did not enter bootloader")


def read_info(client):
    info1 = client.info_page(1)
    info2 = client.info_page(2)
    info4 = client.info_page(4)
    info5 = client.info_page(5)

    return {
        "app_start": struct.unpack("<I", info1[3:7])[0],
        "app_end": struct.unpack("<I", info2[3:7])[0],
        "state": APP_STATES.get(info4[3], f"UNKNOWN_0x{info4[3]:02X}"),
        "size": struct.unpack("<I", info4[4:8])[0],
        "crc": struct.unpack("<I", info5[3:7])[0],
    }


def test_info(client):
    print_step("INFO")
    info = read_info(client)
    print_step(
        f"app 0x{info['app_start']:08X}-0x{info['app_end']:08X}, "
        f"state {info['state']}, size {info['size']}, "
        f"crc 0x{info['crc']:08X}"
    )
    expect(info["app_start"] < info["app_end"], "invalid app range")


def test_invalid_address(client):
    print_step("invalid address NACK")
    try:
        client.set_addr(0x08000000)
    except BootloaderNackError as exc:
        expect("INVALID_ADDRESS" in str(exc), "unexpected NACK for invalid address")
        return
    raise TestFailure("invalid bootloader address was accepted")


def test_sequence_mismatch(client):
    print_step("sequence mismatch NACK")
    info = read_info(client)
    client.set_addr(info["app_start"])

    try:
        client.transact(HOST_SEQ_DATA_BASE_ID | 1, b"\xFF" * 8, CMD_DATA,
                        retryable=False)
    except BootloaderNackError as exc:
        expect("SEQUENCE" in str(exc), "unexpected NACK for sequence mismatch")
        return
    raise TestFailure("out-of-order data frame was accepted")


def test_interrupted_update(client):
    print_step("interrupted update state")
    client.erase()
    info = read_info(client)
    expect(info["state"] == "UPDATE_IN_PROGRESS",
           f"expected UPDATE_IN_PROGRESS, got {info['state']}")

    client.reset()
    time.sleep(0.5)
    ensure_bootloader(client)
    info = read_info(client)
    expect(info["state"] == "METADATA_ERASED",
           f"expected METADATA_ERASED, got {info['state']}")


def test_bad_crc(client, firmware):
    print_step("CRC mismatch NACK")
    info = read_info(client)
    client.erase()
    client.set_addr(info["app_start"])

    padded = firmware
    if len(padded) % 8 != 0:
        padded += b"\xFF" * (8 - (len(padded) % 8))

    for frame in range(min(4, len(padded) // 8)):
        start = frame * 8
        client.write_data8(padded[start:start + 8], seq=frame)

    try:
        client.crc(0x00000000)
    except BootloaderNackError as exc:
        expect("CRC_MISMATCH" in str(exc), "unexpected NACK for bad CRC")
        return
    raise TestFailure("bad CRC was accepted")


def test_full_upload(client, firmware):
    print_step("full upload and boot")
    info = read_info(client)
    upload(client, firmware, info["app_start"], True, True, True, False, False)
    time.sleep(0.5)
    response = client.ping()
    expect(response[2] == APP_TEST_VERSION_MAJOR,
           "test app did not respond after boot")


def parse_args(argv):
    parser = argparse.ArgumentParser(description="CAN bootloader hardware tests")
    parser.add_argument("firmware", type=pathlib.Path,
                        help="test app raw .bin firmware")
    parser.add_argument("--interface", default="socketcan")
    parser.add_argument("--channel", default="can0")
    parser.add_argument("--bitrate", type=int, default=None)
    parser.add_argument("--timeout", type=float, default=2.0)
    parser.add_argument("--erase-timeout", type=float, default=10.0)
    parser.add_argument("--retries", type=int, default=3)
    parser.add_argument("--skip-destructive", action="store_true",
                        help="skip erase/write/reset tests")
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)

    if not args.firmware.exists():
        raise TestFailure(f"firmware not found: {args.firmware}")

    firmware = args.firmware.read_bytes()
    if not firmware:
        raise TestFailure("firmware is empty")

    client = CanBootClient(args.interface, args.channel, args.bitrate,
                           args.timeout, args.erase_timeout, args.retries)
    try:
        ensure_bootloader(client)
        test_info(client)
        test_invalid_address(client)

        if not args.skip_destructive:
            test_sequence_mismatch(client)
            test_interrupted_update(client)
            test_bad_crc(client, firmware)
            test_full_upload(client, firmware)

        print("[TEST] PASS")
    finally:
        client.close()


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except (BootloaderError, TestFailure) as exc:
        print(f"[TEST] FAIL: {exc}", file=sys.stderr)
        sys.exit(1)
