# Application Integration

This project already includes a test app under `src/app_test/`. Use this document when integrating a separate real application with the bootloader.

## Required App Changes

1. Link the app at `0x08010000`.
2. Set the vector table to `0x08010000` before enabling interrupts.
3. Include the boot request implementation.
4. Add a CAN command path that accepts `CMD_RESET`.
5. Build bootloader and app with the same `BOOT_NODE_ID`.
6. Build a raw `.bin` and upload it with `tools/can_uploader.py --node-id`.

## One-Step PlatformIO Preparation

For a PlatformIO STM32 app project, this repository provides a helper script:

```bash
python tools/integrate_platformio_app.py /path/to/app_project \
  --base-env nucleo_f446re \
  --app-env nucleo_f446re_boot \
  --node-id 0
```

Use `--dry-run` first to preview the changes:

```bash
python tools/integrate_platformio_app.py /path/to/app_project --dry-run
```

The script copies:

```text
include/boot_config.h
include/boot_request.h
include/can_if.h
src/boot_request.c
linker/STM32F446RETX_APP_FLASH.ld
examples/app_bootloader_integration.c
```

It also appends a bootloader-linked PlatformIO environment:

```ini
[env:nucleo_f446re_boot]
extends = env:nucleo_f446re
board_build.ldscript = linker/STM32F446RETX_APP_FLASH.ld
build_flags =
    ${env:nucleo_f446re.build_flags}
    -D BOOT_NODE_ID=0
```

The script cannot safely edit arbitrary app logic, so these manual changes remain:

- Add `SCB->VTOR = APP_START_ADDR` at the start of app `main()`.
- Connect the app's CAN RX path to the copied bootloader command handler.

## Linker Script

Set the app Flash origin and length:

```ld
FLASH (rx) : ORIGIN = 0x08010000, LENGTH = 448K - 16
```

The last 16 bytes are reserved for bootloader metadata at `0x0807FFF0`.

## Vector Table

Place this at the start of the app `main()` before peripheral interrupts are enabled:

```c
#include "boot_config.h"

int main(void)
{
    SCB->VTOR = APP_START_ADDR;

    HAL_Init();
    SystemClock_Config();
    /* ... */
}
```

## Boot Request

Copy these files into the real app or share them from this project:

```text
include/boot_config.h
include/boot_request.h
src/boot_request.c
```

The current implementation stores the request in RTC backup register `BKP0R`. The app does not erase or write Flash directly. It only writes the boot request magic and resets.

```c
boot_request_set();
HAL_Delay(20);
NVIC_SystemReset();
```

## CAN Update Command

The included uploader detects the app by sending `CMD_PING` on the node-specific command ID:

```text
command ID  = 0x100 + node_id
response ID = 0x180 + node_id
```

For node 0, the app should respond as:

```text
Host -> App: 100#0100000000000000
App  -> Host: 180#7901A00100000000
```

When the app receives `CMD_RESET`, it should:

1. ACK the command.
2. Store the boot request magic.
3. Reset the MCU.

```text
Host -> App: 100#0800000000000000
App  -> Host: 180#7908010000000000
```

After reset, the bootloader sees the request and stays in CAN update mode.

See `examples/app_bootloader_integration.c` for a compact implementation.

## Node ID

Each board uses a build-time node ID:

```c
#define BOOT_NODE_ID 0U
```

Supported range is `0..15`.

For PlatformIO, create a node-specific environment:

```ini
[env:nucleo_f446re_node1]
extends = env:nucleo_f446re
build_flags =
  ${env:nucleo_f446re.build_flags}
  -D BOOT_NODE_ID=1
```

Build the bootloader and matching app with the same node ID. Upload with the same value:

```bash
.venv/bin/python tools/can_uploader.py --node-id 1 firmware.bin
```

## Build Output

The uploader expects a raw `.bin` with no embedded address information. It writes the file to `APP_START_ADDR`, currently `0x08010000`.

```bash
.venv/bin/python tools/can_uploader.py \
  --interface socketcan \
  --channel can0 \
  --timeout 2 \
  --erase-timeout 10 \
  --retries 3 \
  --node-id 0 \
  path/to/application.bin \
  --boot
```

## Integration Checklist

- App linker `FLASH ORIGIN` is `0x08010000`.
- App linker reserves the last 16 bytes of Flash.
- `SCB->VTOR = APP_START_ADDR` runs before enabling interrupts.
- App uses the same CAN bitrate as the bootloader.
- App and bootloader are built with the same `BOOT_NODE_ID`.
- App handles `CMD_PING` and `CMD_RESET` on CAN ID `0x100 + node_id`.
- App writes boot request with `boot_request_set()`.
- App resets with `NVIC_SystemReset()`.
- App `.bin` is uploaded with `APP_START_ADDR = 0x08010000`.

## Current Test App Reference

The in-repo test app already follows this pattern:

```text
src/app_test/main.c
linker/STM32F446RETX_APP_FLASH.ld
```
