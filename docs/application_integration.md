# Application Integration

This project already includes a test app under `src/app_test/`. Use this document when integrating a separate real application with the bootloader.

## Required App Changes

1. Link the app at `0x08010000`.
2. Set the vector table to `0x08010000` before enabling interrupts.
3. Include the boot request implementation.
4. Add a CAN command path that accepts `CMD_RESET`.
5. Build a raw `.bin` and upload it with `tools/can_uploader.py`.

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

The included uploader detects the app by sending `CMD_PING` on CAN ID `0x101`. The app should respond on `0x181`:

```text
Host -> App: 101#0100000000000000
App  -> Host: 181#7901A00100000000
```

When the app receives `CMD_RESET`, it should:

1. ACK the command.
2. Store the boot request magic.
3. Reset the MCU.

```text
Host -> App: 101#0800000000000000
App  -> Host: 181#7908010000000000
```

After reset, the bootloader sees the request and stays in CAN update mode.

See `examples/app_bootloader_integration.c` for a compact implementation.

## Build Output

The uploader expects a raw `.bin` with no embedded address information. It writes the file to `APP_START_ADDR`, currently `0x08010000`.

```bash
.venv/bin/python tools/can_uploader.py \
  --interface socketcan \
  --channel can0 \
  --timeout 2 \
  --erase-timeout 10 \
  --retries 3 \
  path/to/application.bin \
  --boot
```

## Integration Checklist

- App linker `FLASH ORIGIN` is `0x08010000`.
- App linker reserves the last 16 bytes of Flash.
- `SCB->VTOR = APP_START_ADDR` runs before enabling interrupts.
- App uses the same CAN bitrate as the bootloader.
- App handles `CMD_PING` and `CMD_RESET` on CAN ID `0x101`.
- App writes boot request with `boot_request_set()`.
- App resets with `NVIC_SystemReset()`.
- App `.bin` is uploaded with `APP_START_ADDR = 0x08010000`.

## Current Test App Reference

The in-repo test app already follows this pattern:

```text
src/app_test/main.c
linker/STM32F446RETX_APP_FLASH.ld
```
