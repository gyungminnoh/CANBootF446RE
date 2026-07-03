# CANBootF446RE

Custom Classic CAN bootloader for STM32F446RE / NUCLEO-F446RE.

The bootloader is flashed once through ST-Link at `0x08000000`. After that, the application firmware can be updated over CAN without ST-Link.

## Memory map

- Bootloader: `0x08000000`
- Application: `0x08010000`
- App metadata: `0x0807FFF0`
- App writable Flash range: `0x08010000` to `0x0807FFF0`

The test application is linked with `linker/STM32F446RETX_APP_FLASH.ld`, which moves `FLASH ORIGIN` to `0x08010000`.

## CAN protocol

- Node ID range: `0` to `15`, selected at build time with `BOOT_NODE_ID`
- Host command ID: `0x100 + node_id`
- Bootloader response ID: `0x180 + node_id`
- Sequenced fast data IDs: `0x200 + node_id * 0x10 + seq_low4`
- Legacy raw fast data ID: `0x110 + node_id`

Implemented commands:

- `CMD_PING`
- `CMD_INFO`
- `CMD_ERASE`
- `CMD_SET_ADDR`
- `CMD_DATA`
- `CMD_CRC`
- `CMD_BOOT`
- `CMD_RESET`

The default uploader path writes 8 bytes per CAN frame using `0x200 | seq`, then validates the written app with CRC32 before marking metadata valid.

## Build

```bash
pio run -e nucleo_f446re
pio run -e nucleo_f446re_test_app
```

The default build uses `BOOT_NODE_ID=0`. For another board, add a node-specific build flag:

```ini
[env:nucleo_f446re_node1]
extends = env:nucleo_f446re
build_flags =
  ${env:nucleo_f446re.build_flags}
  -D BOOT_NODE_ID=1
```

## Flash bootloader with ST-Link

```bash
pio run -e nucleo_f446re -t upload
```

## Upload app over CAN

Linux SocketCAN example:

```bash
sudo ip link set can0 down
sudo ip link set can0 up type can bitrate 500000

.venv/bin/python tools/can_uploader.py \
  --interface socketcan \
  --channel can0 \
  --timeout 2 \
  --erase-timeout 10 \
  --retries 3 \
  --node-id 0 \
  .pio/build/nucleo_f446re_test_app/firmware.bin \
  --boot
```

The uploader can detect the included test app, request bootloader reset over CAN, erase the app area, upload the binary, verify CRC, and boot the app.

## Prepare Another PlatformIO App

```bash
python tools/integrate_platformio_app.py /path/to/app_project \
  --base-env nucleo_f446re \
  --app-env nucleo_f446re_boot \
  --node-id 0
```

Run with `--dry-run` first to preview file copies and `platformio.ini` changes.

## Hardware test

With the board and CAN interface connected:

```bash
.venv/bin/python tools/test_can_bootloader.py \
  --interface socketcan \
  --channel can0 \
  --timeout 2 \
  --erase-timeout 10 \
  --node-id 0 \
  .pio/build/nucleo_f446re_test_app/firmware.bin
```

The test covers PING/INFO, invalid address NACK, sequence mismatch NACK, interrupted update state, CRC mismatch NACK, and full upload/boot.

## Notes

- CAN1 uses PB8/PB9 with AF9 on the NUCLEO-F446RE.
- The project uses HSE bypass 8 MHz from the ST-Link MCO path.
- The bootloader never writes below `APP_START_ADDR`.
- If an update is interrupted after erase, metadata remains invalid/erased and the device stays in bootloader mode.
- The uploader retries retry-safe transfers on timeout/send errors. NACK responses are not retried.

More implementation details are in `docs/stage1_app_jump.md`.

For integrating a real application, see `docs/application_integration.md`.
