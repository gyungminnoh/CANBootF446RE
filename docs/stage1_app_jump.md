# Stage 1: Minimal app jump bootloader

## Project structure

```text
CANBootF446RE/
  platformio.ini
  include/
    boot_config.h       Common addresses, CAN IDs, magic values
    bootloader.h        Bootloader public API
    boot_request.h      RTC backup-register boot request abstraction
    can_if.h            CAN interface
    flash_if.h          Flash address/sector helper
    stm32f4xx_hal_conf.h
  src/
    main.c
    bootloader.c
    boot_request.c
    can_if.c
    flash_if.c
    stm32f4xx_hal_msp.c
  linker/
    STM32F446RETX_APP_FLASH.ld
  examples/
    app_bootloader_integration.c
```

## Current behavior

The current bootloader is intentionally minimal.

1. Reset enters bootloader at `0x08000000`.
2. HAL, GPIO, and CAN1 are initialized.
3. RTC backup-register boot magic is checked and cleared if present.
4. If boot magic is not set and the app vector table at `0x08010000` is valid, the bootloader waits `500 ms` for a CAN update request.
5. If the app is invalid or boot magic was set, the bootloader remains in a simple CAN polling loop.
6. If no update request arrives during the wait window, the bootloader jumps to the app.

Flash erase/write, CRC verification, and persistent app metadata are implemented.

## Important macros

```c
#define BOOTLOADER_START_ADDR   0x08000000UL
#define APP_START_ADDR          0x08010000UL
#define FLASH_END_ADDR          0x08080000UL

#define BOOT_MAGIC_VALUE        0xB007C0DEUL

#define BOOT_UPDATE_WAIT_MS     500UL

#define BOOT_NODE_ID            0U
#define CAN_HOST_CMD_ID         (0x100U + BOOT_NODE_ID)
#define CAN_HOST_SEQ_DATA_BASE_ID (0x200U + (BOOT_NODE_ID * 0x10U))
#define CAN_BOOT_RESP_ID        (0x180U + BOOT_NODE_ID)
```

## App validation rule

The bootloader accepts an app only when:

- `*(uint32_t *)0x08010000` is inside SRAM.
- `*(uint32_t *)0x08010004` is a Thumb address pointing into the app Flash region.

CRC validation writes persistent metadata after `CMD_CRC` passes. On reset, the bootloader validates the vector table and metadata before jumping to the app.

## App linker script change

For an application linked behind this bootloader, change Flash origin and length:

```ld
FLASH (rx) : ORIGIN = 0x08010000, LENGTH = 448K - 16
```

The provided sample is:

```text
linker/STM32F446RETX_APP_FLASH.ld
```

This linker script reserves the last 16 bytes of Flash for bootloader metadata:

```text
APP_META_ADDR = 0x0807FFF0
```

In PlatformIO app projects, select it with:

```ini
board_build.ldscript = linker/STM32F446RETX_APP_FLASH.ld
```

In STM32CubeIDE, replace the generated linker script's `FLASH` memory line with the same origin/length.

## App vector table relocation

The app must relocate its vector table early, before enabling interrupts:

```c
#define APP_START_ADDR 0x08010000UL

int main(void)
{
    SCB->VTOR = APP_START_ADDR;
    HAL_Init();
    SystemClock_Config();
    /* app init */
}
```

If using CubeMX-generated `system_stm32f4xx.c`, another valid approach is setting `VECT_TAB_OFFSET` to `0x10000`.
Directly setting `SCB->VTOR` in `main()` is more explicit for this project.

## Boot request from app

The app should not erase/write Flash itself. To request update mode:

1. Receive a dedicated CAN command in the app.
2. Store `BOOT_MAGIC_VALUE` through `boot_request_set()`.
3. Call `NVIC_SystemReset()`.
4. The bootloader sees the magic after reset and remains in bootloader mode.

The first SRAM implementation was replaced with RTC backup register `BKP0R`, because SRAM near the stack was overwritten during reset/startup. The public API remains isolated in `boot_request.c/h`.

See:

```text
src/app_test/main.c
examples/app_bootloader_integration.c
docs/application_integration.md
```

The test app is linked at `0x08010000` with:

```bash
pio run -e nucleo_f446re_test_app
```

The app responds to PING as:

```text
180#7901A00100000000
```

Sending `CMD_RESET` requests bootloader mode:

```text
100#0800000000000000
```

The app ACK includes byte 2 set to `1` when the boot request magic was written successfully:

```text
180#7908010000000000
```

After reset, the bootloader responds to PING as:

```text
180#7901000100000000
```

## CAN PING test

CAN1 uses PB8/PB9 and is configured for 500 kbit/s with the current 180 MHz system clock and 45 MHz APB1 clock.

Linux SocketCAN setup:

```bash
sudo ip link set can0 down
sudo ip link set can0 up type can bitrate 500000
```

Terminal 1:

```bash
candump can0
```

Terminal 2:

```bash
cansend can0 100#0100000000000000
```

Expected response:

```text
181   [8]  79 01 00 01 00 00 00 00
```

`0x79` is ACK, `0x01` is `CMD_PING`, and `00 01` is bootloader version `0.1`.

If a valid app is already present at `0x08010000`, send PING within the first `500 ms` after reset to keep the bootloader active. If no app is present, the bootloader remains in the CAN loop indefinitely.

## Later uploader design

The Python uploader uses `python-can` and raw `.bin` files. Since `.bin` has no address metadata, it always writes to `APP_START_ADDR` unless `--address` is overridden.

Install dependency:

```bash
python3 -m venv .venv
.venv/bin/python -m pip install python-can
```

Uploader command flow:

```text
PING -> ERASE -> SET_ADDR(0x08010000) -> DATA... -> CRC -> BOOT
```

Example:

```bash
.venv/bin/python tools/can_uploader.py \
  --interface socketcan \
  --channel can0 \
  .pio/build/nucleo_f446re_test_app/firmware.bin \
  --boot
```

Validated result after uploader boot:

```text
100#0100000000000000
180#7901A00100000000
```

Validated uploader output:

```text
PING ok: version 0.1
ERASE ok
SET_ADDR ok: 0x08010000
Wrote 1474/1474 words
CRC ok: 0x310E0090
BOOT ok
```

## Current erase command

`CMD_ERASE` is implemented in the bootloader. It erases only the app region:

```text
Sector 4: 0x08010000 - 0x0801FFFF
Sector 5: 0x08020000 - 0x0803FFFF
Sector 6: 0x08040000 - 0x0805FFFF
Sector 7: 0x08060000 - 0x0807FFFF
```

The bootloader sectors 0-3 are not erased. Sector 7 is erased with the app, so metadata is cleared during every update and rewritten after CRC verification.

CAN command:

```text
100#0300000000000000
```

Successful response:

```text
180#7903000000000000
```

## Current write commands

`CMD_SET_ADDR` sets the next write address. The address must be 4-byte aligned and inside the app region.

Example for `0x08010000`:

```text
100#0400000108000000
180#7904000000000000
```

Bootloader-region addresses are rejected. Example for `0x08000000`:

```text
100#0400000008000000
180#1F04040000000000
```

The uploader reports this as:

```text
NACK for cmd 0x04: INVALID_ADDRESS (0x04)
```

`CMD_DATA` writes one 32-bit word:

```text
[CMD_DATA, seq_low, seq_high, data0, data1, data2, data3, checksum]
```

The checksum is the 8-bit sum of bytes 0-6.

The faster uploader path uses sequence-encoded data CAN IDs:

```text
Host data IDs: 0x200 + node_id * 0x10 through 0x20F + node_id * 0x10
Sequence:      arbitration_id & 0x0F
Payload:       [data0, data1, data2, data3, data4, data5, data6, data7]
Response:      [ACK, CMD_DATA, 0, 0, 0, 0, 0, 0]
```

This mode writes two 32-bit words per CAN frame. `CMD_SET_ADDR` selects the starting Flash address and resets the expected sequence to 0. The sequence wraps modulo 16.

The older raw fast path on CAN ID `0x110 + node_id` is still accepted for compatibility and can be selected with `--raw-fast-data`. The legacy 4-byte `CMD_DATA` protocol remains available with `--legacy-data`.

Example writes:

```text
100#0500000000022027
180#7905000000000000

100#050100C5010108D5
180#7905000000000000
```

Readback after these frames:

```text
0x08010000: 20020000 080101c5 ffffffff ffffffff
```

## Current CRC command

`CMD_CRC` sends the expected CRC32 of the bytes written in the current upload session. Because Classic CAN has only 8 payload bytes, the app size is not sent in this frame; the bootloader uses `current_write_addr - APP_START_ADDR`.

Payload:

```text
[CMD_CRC, crc0, crc1, crc2, crc3, 0, 0, 0]
```

CRC is standard reflected CRC32, matching Python `zlib.crc32()`. The uploader calculates CRC over the padded data actually written to Flash. The default fast data mode pads to 8 bytes; `--legacy-data` pads to 4 bytes.

After CRC passes, the bootloader writes metadata at `0x0807FFF0`:

```c
typedef struct {
    uint32_t magic;    // 0xA5A55A5A
    uint32_t app_size;
    uint32_t app_crc;
    uint32_t state;    // 0x00000000 means VALID
} app_meta_t;
```

Metadata state values reported by `CMD_INFO`:

```text
0x00 INVALID
0x01 VALID
0x02 UPDATE_IN_PROGRESS
0x03 METADATA_ERASED
0x04 CRC_FAILED
```

The stored metadata word uses `0x00000000` for valid. After app erase, the metadata area is erased with sector 7, so a reset during or after an incomplete update leaves the target in bootloader mode instead of jumping to the app.

Validated metadata example:

```text
0x0807fff0: a5a55a5a 00001708 310e0090 00000000
```

## Current INFO command

`CMD_INFO` uses byte 1 as a page selector:

```text
[CMD_INFO, page, 0, 0, 0, 0, 0, 0]
```

Current pages:

```text
page 0: bootloader version, protocol version, feature flags
page 1: APP_START_ADDR
page 2: APP_FLASH_END_ADDR
page 3: APP_META_ADDR
page 4: app state and metadata app_size
page 5: metadata app_crc
```

The Python uploader reads these pages before erase/write and prints the memory map:

```text
INFO ok: protocol 1.0, node 0, app 0x08010000-0x0807FFF0, meta 0x0807FFF0, state VALID, size 5896, crc 0x310E0090
```

Validated interrupted-update behavior:

```text
erase -> info page 4: UPDATE_IN_PROGRESS
reset -> bootloader remains active
info page 4: METADATA_ERASED
```

## NACK error codes

```text
0x01 UNKNOWN_CMD
0x02 ERASE_FAILED
0x03 INVALID_LENGTH
0x04 INVALID_ADDRESS
0x05 CHECKSUM
0x06 SEQUENCE
0x07 FLASH_WRITE
0x08 APP_INVALID
0x09 CRC_MISMATCH
0x0A METADATA_WRITE
```

Linux SocketCAN setup example:

```bash
sudo ip link set can0 up type can bitrate 500000
python3 tools/can_uploader.py --interface socketcan --channel can0 --retries 3 firmware.bin
```

Default upload mode uses CAN IDs `0x200 + node_id * 0x10` through `0x20F + node_id * 0x10` for sequence-checked 8-byte data frames. Add `--raw-fast-data` to use the older raw node data ID, or `--legacy-data` to use the node command ID `CMD_DATA` frame format.

If the test app is already running, the uploader detects app PING version `0xA0.0x01`, sends `CMD_RESET`, waits for the bootloader, and then starts the upload.

The uploader retries retry-safe transfers on CAN send errors or ACK timeout. NACK responses are treated as explicit bootloader rejection and are not retried. Data frame retry is safe for the sequenced fast path because the bootloader ACKs an exact duplicate of the most recently committed frame instead of trying to write it again.

Validated sequence mismatch example:

```text
201#FFFFFFFFFFFFFFFF
180#1F05060000000000
```

`0x06` is `SEQUENCE`.

Hardware regression test:

```bash
python3 tools/test_can_bootloader.py \
  --interface socketcan \
  --channel can0 \
  --timeout 2 \
  --erase-timeout 10 \
  .pio/build/nucleo_f446re_test_app/firmware.bin
```

Validated result:

```text
[TEST] invalid address NACK
[TEST] sequence mismatch NACK
[TEST] interrupted update state
[TEST] CRC mismatch NACK
[TEST] full upload and boot
[TEST] PASS
```

Windows support should be added later through python-can backends such as `slcan`, `pcan`, or vendor-specific drivers.

Real application integration notes are in `docs/application_integration.md`.
