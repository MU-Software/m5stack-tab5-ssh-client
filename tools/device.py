# SPDX-License-Identifier: GPL-3.0-or-later
"""The Tab5 over USB: the firmware's terminal and control ports (TinyUSB), or USB-Serial/JTAG for esptool."""

import os
import time
import zlib
from pathlib import Path
from typing import Annotated

import esptool
import serial
import typer
from serial.tools import list_ports

ESPRESSIF = 0x303A
USB_SERIAL_JTAG = 0x1001

app = typer.Typer(no_args_is_help=True)


def ports(jtag: bool = False) -> list[str]:
    return sorted(p.device for p in list_ports.comports() if p.vid == ESPRESSIF and (p.pid == USB_SERIAL_JTAG) == jtag)


def firmware_port(index: int) -> str:
    if not (found := ports()):
        raise SystemExit("no Tab5 firmware with TinyUSB on USB")
    return found[index]


def jtag_port() -> str:
    if not (found := ports(jtag=True)):
        raise SystemExit("no USB-Serial/JTAG: TinyUSB firmware updates over make flash; BOOT reaches the ROM")
    return found[0]


def wait_line(s: serial.Serial, prefix: bytes, seconds: float) -> bytes:
    end, buffer = time.monotonic() + seconds, b""
    while time.monotonic() < end:
        buffer += bytes(s.read(256))
        for line in buffer.split(b"\n"):
            if line.startswith(prefix):
                return line.strip()
            if line.startswith(b"update "):
                raise SystemExit(line.strip().decode())
    raise SystemExit(f"no {prefix.decode()!r} from the Tab5")


def esptool_main(*args: str) -> None:
    esptool.main(["--chip", "esp32p4", "--port", jtag_port(), "--after", "watchdog-reset", *args])


@app.command()
def update(image: Path) -> None:
    """Send an app image over the control port into the other OTA slot, then wait for the Tab5 to come back."""
    data = image.read_bytes()
    with serial.Serial(firmware_port(-1), timeout=0.1) as s:
        s.write(f"update {len(data)} {zlib.crc32(data):08x}\n".encode())
        wait_line(s, b"ready", 10)
        s.write(data)
        if (result := wait_line(s, b"update ", 60)) != b"update ESP_OK":
            raise SystemExit(result.decode())
    for present in (False, True):
        for _ in range(150):
            if bool(ports()) == present:
                break
            time.sleep(0.2)
        else:
            raise SystemExit("the Tab5 did not come back on USB" if present else "the Tab5 did not restart")
    print("updated; the new image keeps itself once this computer configures its USB")


@app.command()
def flash(image: Path) -> None:
    """Install a build: an OTA update when TinyUSB firmware runs, else esptool with the flash_args next to it."""
    if ports():
        update(image)
    else:
        os.chdir(image.parent)
        esptool_main("write-flash", "@flash_args")


@app.command("esptool", context_settings={"allow_extra_args": True, "ignore_unknown_options": True})
def run_esptool(context: typer.Context) -> None:
    """Run an esptool command (erase-flash, ...) over USB-Serial/JTAG."""
    esptool_main(*context.args)


@app.command()
def send(
    command: Annotated[str, typer.Argument(help="sd on|off, restart, lte on|off|status, net check HOST PORT")],
) -> None:
    """Send a control command to the firmware."""
    with serial.Serial(firmware_port(-1)) as s:
        s.write(command.encode() + b"\n")


@app.command()
def port(name: Annotated[str, typer.Argument(help="terminal or control")]) -> None:
    """Print the port of the firmware's terminal or control interface."""
    print(firmware_port(0 if name == "terminal" else -1))


if __name__ == "__main__":
    app()
