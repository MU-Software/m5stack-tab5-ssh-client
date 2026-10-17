# SPDX-License-Identifier: GPL-3.0-or-later
"""The Tab5 over USB: the firmware's terminal and control ports (TinyUSB), or USB-Serial/JTAG for esptool. Or SSH."""

import os
import subprocess
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
host: str | None = None


@app.callback()
def main(
    destination: Annotated[
        str | None, typer.Option("--host", envvar="TAB5_HOST", help="over SSH instead: a Host in ~/.ssh/config, ...")
    ] = None,
) -> None:
    global host
    host = destination


def ports(jtag: bool = False) -> list[str]:
    """TAB5 picks a board by its MAC, the USB serial number in both modes."""
    board = os.environ.get("TAB5")
    return sorted(
        p.device
        for p in list_ports.comports()
        if p.vid == ESPRESSIF and (p.pid == USB_SERIAL_JTAG) == jtag and board in (None, p.serial_number)
    )


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


def ssh(
    destination: str, command: str, data: bytes | None = None, batch: str = "no"
) -> subprocess.CompletedProcess[bytes]:
    options = ["-o", "ConnectTimeout=3", "-o", f"BatchMode={batch}"]
    return subprocess.run(["ssh", *options, destination, command], input=data, capture_output=True)


def update_over_network(destination: str, command: str, line: str, data: bytes) -> None:
    if (sent := ssh(destination, line, data=data)).stdout.strip() != f"{command} ESP_OK".encode():
        raise SystemExit((sent.stdout or sent.stderr).decode().strip())
    time.sleep(2)
    # This login is what keeps a new app image
    for _ in range(30):
        if (back := ssh(destination, "version", batch="yes")).returncode == 0:
            return print(f"updated: {back.stdout.decode().strip()}")
        time.sleep(1)
    raise SystemExit("the Tab5 did not come back over the network: a new app image rolls back after 60 s")


@app.command()
def update(image: Path, c6: Annotated[bool, typer.Option(help="an ESP32-C6 esp_hosted image")] = False) -> None:
    """Send an app image (or the C6's) over the control port or SSH, then wait for the Tab5 to come back."""
    data, command = image.read_bytes(), "c6 update" if c6 else "update"
    line = f"{command} {len(data)} {zlib.crc32(data):08x}"
    if host:
        return update_over_network(host, command, line, data)
    with serial.Serial(firmware_port(-1), timeout=0.1) as s:
        s.write(f"{line}\n".encode())
        wait_line(s, b"ready", 10)
        s.write(data)
        if (result := wait_line(s, f"{command} ".encode(), 180)) != f"{command} ESP_OK".encode():
            raise SystemExit(result.decode())
    for present in (False, True):
        for _ in range(150):
            if bool(ports()) == present:
                break
            time.sleep(0.2)
        else:
            raise SystemExit("the Tab5 did not come back on USB" if present else "the Tab5 did not restart")
    print(
        "updated; the C6 keeps the image once the Tab5 reaches it, else boots the one before after a minute"
        if c6
        else "updated; the new image keeps itself once this computer configures its USB"
    )


@app.command()
def flash(image: Path) -> None:
    """Install a build: an OTA update when TinyUSB firmware runs, else esptool with the flash_args next to it."""
    if host or ports():
        update(image)
    else:
        os.chdir(image.parent)
        esptool_main("write-flash", "@flash_args")


@app.command("esptool", context_settings={"allow_extra_args": True, "ignore_unknown_options": True})
def run_esptool(context: typer.Context) -> None:
    """Run an esptool command (erase-flash, ...) over USB-Serial/JTAG."""
    esptool_main(*context.args)


@app.command()
def send(command: Annotated[str, typer.Argument(help="any unknown command makes the firmware log the list")]) -> None:
    """Send a control command to the firmware; over SSH, print what it logs until it is quiet for a second."""
    if host:
        raise SystemExit(subprocess.run(["ssh", host, command]).returncode)
    with serial.Serial(firmware_port(-1)) as s:
        s.write(command.encode() + b"\n")


@app.command()
def port(name: Annotated[str, typer.Argument(help="terminal or control")]) -> None:
    """Print the port of the firmware's terminal or control interface."""
    print(firmware_port(0 if name == "terminal" else -1))


if __name__ == "__main__":
    app()
