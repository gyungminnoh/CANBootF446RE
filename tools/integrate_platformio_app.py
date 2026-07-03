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

1. Set the vector table at the start of `main()`:

```c
SCB->VTOR = APP_START_ADDR;
```

2. Include the bootloader command handler from:

```text
examples/app_bootloader_integration.c
```

3. Connect the app's CAN RX path to:

```c
app_handle_bootloader_can_command(&rx_header, data);
```

4. Build the app and upload the generated `.bin`:

```bash
pio run -e {app_env}

python /path/to/CANBootF446RE/tools/can_uploader.py \\
  --interface socketcan \\
  --channel can0 \\
  --node-id {node_id} \\
  .pio/build/{app_env}/firmware.bin \\
  --boot
```
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
