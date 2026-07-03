#!/usr/bin/env python3
import argparse
import configparser
import pathlib
import shutil
import sys


class IntegrationError(RuntimeError):
    pass


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]

FILES_TO_COPY = (
    ("include/boot_config.h", "include/boot_config.h"),
    ("include/boot_request.h", "include/boot_request.h"),
    ("include/can_if.h", "include/can_if.h"),
    ("src/boot_request.c", "src/boot_request.c"),
    ("linker/STM32F446RETX_APP_FLASH.ld", "linker/STM32F446RETX_APP_FLASH.ld"),
    ("examples/app_bootloader_integration.c",
     "examples/app_bootloader_integration.c"),
)


def read_platformio_config(path):
    parser = configparser.ConfigParser(strict=False)
    parser.optionxform = str
    with path.open("r", encoding="utf-8") as fp:
        parser.read_file(fp)
    return parser


def find_first_env(parser):
    for section in parser.sections():
        if section.startswith("env:"):
            return section[4:]
    return None


def env_section(env_name):
    return f"env:{env_name}"


def make_boot_env_block(base_env, app_env, node_id, base_has_build_flags):
    lines = [
        "",
        f"[{env_section(app_env)}]",
        f"extends = env:{base_env}",
        "board_build.ldscript = linker/STM32F446RETX_APP_FLASH.ld",
        "build_flags =",
    ]

    if base_has_build_flags:
        lines.append(f"    ${{env:{base_env}.build_flags}}")

    lines.append("    -D HSE_VALUE=8000000U")
    lines.append(f"    -D BOOT_NODE_ID={node_id}")
    lines.append("")
    return "\n".join(lines)


def copy_file(src_rel, dst_rel, project_dir, force, dry_run):
    src = REPO_ROOT / src_rel
    dst = project_dir / dst_rel

    if not src.exists():
        raise IntegrationError(f"missing source file: {src}")

    if dst.exists() and not force:
        print(f"skip existing: {dst_rel}")
        return

    print(f"copy: {src_rel} -> {dst_rel}")

    if dry_run:
        return

    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def write_integration_notes(project_dir, app_env, node_id, dry_run):
    path = project_dir / "docs" / "bootloader_integration_todo.md"
    content = f"""# Bootloader Integration TODO

This project has been prepared for the CAN bootloader.

Generated app environment:

```bash
pio run -e {app_env}
```

Node ID:

```text
{node_id}
```

Manual code changes still required:

## 1. Inspect The App's Current CAN Structure

Before editing, inspect these files:

```bash
rg -n "CAN_HandleTypeDef|HAL_CAN|CAN1|RxFifo|Fifo0|SystemClock_Config|MX_CAN|while \\(1\\)|int main" src include
```

Classify the app as one of these cases:

- **No CAN yet**: no `HAL_CAN_*` calls and no `CAN_HandleTypeDef`.
- **Polling CAN**: code calls `HAL_CAN_GetRxFifoFillLevel()` or `HAL_CAN_GetRxMessage()` from the main loop.
- **Interrupt CAN**: code uses `HAL_CAN_RxFifo0MsgPendingCallback()` or activates `CAN_IT_RX_FIFO0_MSG_PENDING`.
- **RTOS/queue CAN**: callback pushes frames into a queue or task.

For a very simple app, use the "No CAN yet" path below. For an existing CAN app, do not replace its CAN logic; insert the bootloader handler as an extra command path.

## 2. Set Vector Table At App Startup

Edit the real app's `main()` file. Add this include near the top:

```c
#include "boot_config.h"
```

At the start of `main()`, before enabling peripheral interrupts, add:

```c
SCB->VTOR = APP_START_ADDR;
```

Expected shape:

```c
int main(void)
{{
    SCB->VTOR = APP_START_ADDR;

    HAL_Init();
    SystemClock_Config();
    /* app init */
}}
```

Do not place this only in the example file. It must run in the app's real
reset path before interrupts or CAN callbacks can use the relocated vector
table.

## 2a. Match The App Clock To The Bootloader Test Baseline

For NUCLEO-F446RE in this project, the verified clock source is the ST-LINK MCO
8 MHz clock on HSE bypass. If the app was created as a generic PlatformIO/Cube
example, its `SystemClock_Config()` may still use HSI or a different HSE/PLL
shape. That can make the app jump succeed but stop in `Error_Handler()` before
CAN is initialized.

Use this known-good oscillator setup unless the board hardware is intentionally
different:

```c
RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
RCC_OscInitStruct.PLL.PLLM = 4;
RCC_OscInitStruct.PLL.PLLN = 180;
RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
RCC_OscInitStruct.PLL.PLLQ = 4;
```

Do not add `PLLR` for this F446 baseline unless the generated HAL code for the
target explicitly requires it. A mismatched PLL field can make
`HAL_RCC_OscConfig()` fail.

The generated PlatformIO boot env should also define the HSE value:

```ini
build_flags =
    -D HSE_VALUE=8000000U
    -D BOOT_NODE_ID={node_id}
```

## 3. Add The Bootloader Command Handler

The helper copied this file:

```text
examples/app_bootloader_integration.c
```

Use it as a reference. The key function is:

```c
app_handle_bootloader_can_command(&rx_header, data);
```

Recommended implementation choices:

- For a small app, copy the whole function `app_handle_bootloader_can_command()` into the app's CAN source file or keep `examples/app_bootloader_integration.c` in the build.
- If the app already has a CAN protocol dispatcher, call this function before the app's normal command switch. It returns without action when the CAN ID or command does not match.
- Keep `app_request_bootloader_reset()` behavior unchanged: ACK first, then `boot_request_set()`, short delay, `NVIC_SystemReset()`.

The handler expects node-specific IDs from `boot_config.h`:

```text
command ID  = 0x100 + BOOT_NODE_ID
response ID = 0x180 + BOOT_NODE_ID
```

For this project:

```text
BOOT_NODE_ID = {node_id}
```

## 4. Connect The Handler To CAN RX

### Case A: App Has No CAN Yet

Add a global CAN handle if one does not exist:

```c
CAN_HandleTypeDef hcan1;
```

Add or generate CAN1 init for the same bitrate as the bootloader. Current bootloader timing is 500 kbit/s with APB1 at 45 MHz:

```c
static void MX_CAN1_Init(void)
{{
    hcan1.Instance = CAN1;
    hcan1.Init.Prescaler = 5;
    hcan1.Init.Mode = CAN_MODE_NORMAL;
    hcan1.Init.SyncJumpWidth = CAN_SJW_1TQ;
    hcan1.Init.TimeSeg1 = CAN_BS1_15TQ;
    hcan1.Init.TimeSeg2 = CAN_BS2_2TQ;
    hcan1.Init.TimeTriggeredMode = DISABLE;
    hcan1.Init.AutoBusOff = ENABLE;
    hcan1.Init.AutoWakeUp = DISABLE;
    hcan1.Init.AutoRetransmission = ENABLE;
    hcan1.Init.ReceiveFifoLocked = DISABLE;
    hcan1.Init.TransmitFifoPriority = DISABLE;

    if (HAL_CAN_Init(&hcan1) != HAL_OK) {{
        Error_Handler();
    }}
}}
```

Configure a filter for this node's command ID and start CAN. For first bring-up,
an accept-all filter is often easier to debug; after PING/RESET are proven,
switch to the exact node command filter if desired.

Accept-all bring-up filter:

```c
filter.FilterIdHigh = 0;
filter.FilterIdLow = 0;
filter.FilterMaskIdHigh = 0;
filter.FilterMaskIdLow = 0;
```

Exact node command filter:

```c
static void App_CAN_Start(void)
{{
    CAN_FilterTypeDef filter = {{0}};
    uint32_t start_tick;

    filter.FilterBank = 0;
    filter.FilterMode = CAN_FILTERMODE_IDMASK;
    filter.FilterScale = CAN_FILTERSCALE_32BIT;
    filter.FilterIdHigh = (uint16_t)(CAN_HOST_CMD_ID << 5);
    filter.FilterIdLow = 0;
    filter.FilterMaskIdHigh = (uint16_t)(0x7FFU << 5);
    filter.FilterMaskIdLow = 0;
    filter.FilterFIFOAssignment = CAN_RX_FIFO0;
    filter.FilterActivation = ENABLE;
    filter.SlaveStartFilterBank = 14;

    if (HAL_CAN_ConfigFilter(&hcan1, &filter) != HAL_OK) {{
        Error_Handler();
    }}

    CLEAR_BIT(CAN1->FMR, CAN_FMR_FINIT);

    CLEAR_BIT(hcan1.Instance->MCR, CAN_MCR_SLEEP);
    start_tick = HAL_GetTick();
    while ((hcan1.Instance->MSR & CAN_MSR_SLAK) != 0U) {{
        if ((HAL_GetTick() - start_tick) > 10U) {{
            Error_Handler();
        }}
    }}

    if (HAL_CAN_Start(&hcan1) != HAL_OK) {{
        Error_Handler();
    }}
}}
```

Poll CAN in the main loop:

```c
static void App_CAN_Poll(void)
{{
    CAN_RxHeaderTypeDef rx_header = {{0}};
    uint8_t data[8] = {{0}};

    if (HAL_CAN_GetRxFifoFillLevel(&hcan1, CAN_RX_FIFO0) == 0U) {{
        return;
    }}

    if (HAL_CAN_GetRxMessage(&hcan1, CAN_RX_FIFO0, &rx_header, data) != HAL_OK) {{
        return;
    }}

    app_handle_bootloader_can_command(&rx_header, data);
}}
```

Call the functions:

```c
MX_CAN1_Init();
App_CAN_Start();

while (1) {{
    App_CAN_Poll();
    /* existing app loop */
}}
```

Also make sure CAN pins are configured for CAN1:

```text
PB8  -> CAN1_RX, AF9
PB9  -> CAN1_TX, AF9
```

### Case B: App Already Polls CAN

Find the place after `HAL_CAN_GetRxMessage()` succeeds. Add:

```c
app_handle_bootloader_can_command(&rx_header, data);
```

If the app has its own protocol switch, call the bootloader handler before it:

```c
app_handle_bootloader_can_command(&rx_header, data);
app_handle_normal_can_command(&rx_header, data);
```

The bootloader handler ignores frames that are not for `CAN_HOST_CMD_ID`.

### Case C: App Uses CAN RX Interrupts

In `HAL_CAN_RxFifo0MsgPendingCallback()` after reading the frame, add:

```c
app_handle_bootloader_can_command(&rx_header, data);
```

If the callback must stay short, push the frame into the existing queue and call the handler from the CAN task/consumer instead.

### Case D: App Uses RTOS Or A CAN Queue

Do not reset directly from a low-level ISR if the app architecture forbids it. Call `app_handle_bootloader_can_command()` from the task that consumes CAN frames, or translate `CMD_RESET` into an app event that calls:

```c
boot_request_set();
HAL_Delay(20);
NVIC_SystemReset();
```

## 5. Verify Expected CAN Behavior

After building and uploading once, the app should answer node {node_id} PING:

```text
Host -> App: {0x100 + node_id:03X}#0100000000000000
App  -> Host: {0x180 + node_id:03X}#7901A00100000000
```

The app should enter bootloader mode on reset command:

```text
Host -> App: {0x100 + node_id:03X}#0800000000000000
App  -> Host: {0x180 + node_id:03X}#7908010000000000
```

After reset, the bootloader should answer:

```text
Host -> Bootloader: {0x100 + node_id:03X}#0100000000000000
Bootloader -> Host: {0x180 + node_id:03X}#7901000100000000
```

## 6. Build And Upload The Generated `.bin`

```bash
pio run -e {app_env}

python /path/to/CANBootF446RE/tools/can_uploader.py \\
  --interface socketcan \\
  --channel can0 \\
  --node-id {node_id} \\
  .pio/build/{app_env}/firmware.bin \\
  --boot
```

## 7. Common Failure Modes

- No response to PING: CAN not initialized, wrong bitrate, wrong node ID, wrong filter, or transceiver wiring issue.
- Bootloader PING works but app PING does not: halt with ST-Link and check `pc`.
  If `pc` is inside `Error_Handler`, inspect `SystemClock_Config()` first,
  especially HSE bypass, PLLQ, `HSE_VALUE`, and accidental `PLLR` settings.
- App runs once but cannot update again: `CMD_RESET` handler is not connected to the app CAN RX path.
- HardFault after boot: missing `SCB->VTOR = APP_START_ADDR` or app linked at the wrong address.
- Upload rejects firmware size: app is larger than `0x08010000..0x0807FFF0`.
- Multiple boards respond: two boards were built with the same `BOOT_NODE_ID`.
- CAN frames visible in `candump` but app ignores them: verify the app command
  ID is `0x100 + BOOT_NODE_ID`, response ID is `0x180 + BOOT_NODE_ID`, and the
  CAN filter actually admits that ID.
- App linked and uploaded correctly but never starts after `CMD_BOOT`: verify
  the `.bin` was built from the boot env with
  `board_build.ldscript = linker/STM32F446RETX_APP_FLASH.ld`.
"""

    print(f"write: {path.relative_to(project_dir)}")

    if dry_run:
        return

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def integrate(args):
    project_dir = args.project.resolve()
    platformio_ini = project_dir / "platformio.ini"

    if not platformio_ini.exists():
        raise IntegrationError(f"platformio.ini not found: {platformio_ini}")

    if args.node_id < 0 or args.node_id > 15:
        raise IntegrationError("--node-id must be in range 0..15")

    parser = read_platformio_config(platformio_ini)
    base_env = args.base_env or find_first_env(parser)

    if not base_env:
        raise IntegrationError("no [env:*] section found in platformio.ini")

    base_section = env_section(base_env)
    if not parser.has_section(base_section):
        raise IntegrationError(f"base env not found: [{base_section}]")

    app_env = args.app_env or f"{base_env}_boot"
    app_section = env_section(app_env)

    for src_rel, dst_rel in FILES_TO_COPY:
        copy_file(src_rel, dst_rel, project_dir, args.force, args.dry_run)

    write_integration_notes(project_dir, app_env, args.node_id, args.dry_run)

    if parser.has_section(app_section):
        print(f"skip existing platformio env: [{app_section}]")
        return

    base_has_build_flags = parser.has_option(base_section, "build_flags")
    block = make_boot_env_block(base_env, app_env, args.node_id,
                                base_has_build_flags)

    print(f"append platformio env: [{app_section}]")

    if args.dry_run:
        print(block)
        return

    with platformio_ini.open("a", encoding="utf-8") as fp:
        fp.write(block)


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Prepare a PlatformIO STM32 app project for CAN bootloader uploads"
    )
    parser.add_argument("project", type=pathlib.Path,
                        help="path to the target PlatformIO app project")
    parser.add_argument("--base-env",
                        help="existing app env to extend; defaults to first env")
    parser.add_argument("--app-env",
                        help="new bootloader-linked app env name")
    parser.add_argument("--node-id", type=int, default=0,
                        help="BOOT_NODE_ID value, range 0..15")
    parser.add_argument("--force", action="store_true",
                        help="overwrite copied integration files")
    parser.add_argument("--dry-run", action="store_true",
                        help="show actions without changing files")
    return parser.parse_args(argv)


def main(argv):
    args = parse_args(argv)
    integrate(args)


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except IntegrationError as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
