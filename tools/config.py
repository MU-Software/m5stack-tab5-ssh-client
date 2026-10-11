# SPDX-License-Identifier: GPL-3.0-or-later
"""Device configuration: the JSON document of tools/config.schema.json, kept in NVS or on the SD card."""

import csv
import glob
import io
import json
from pathlib import Path
from typing import Annotated

import jsonschema
import typer
from esp_idf_nvs_partition_gen import nvs_partition_gen as nvs
from esptool.cmds import attach_flash, detect_chip, erase_region, reset_chip, run_stub, write_flash
from esptool.loader import ESPLoader

TOP = Path(__file__).resolve().parent.parent
with (TOP / "tools/config.schema.json").open() as f:
    SCHEMA = json.load(f)

app = typer.Typer(no_args_is_help=True)


def default_port() -> str:
    return sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))[0]


Serial = Annotated[str, typer.Option(default_factory=default_port)]


def nvs_partition() -> tuple[int, int]:
    with (TOP / "partitions.csv").open() as f:
        for name, _, _, offset, size, *_ in csv.reader(line for line in f if line.strip() and not line.startswith("#")):
            if name.strip() == "nvs":
                return int(offset, 0), int(size, 0)
    raise LookupError("no nvs partition")


def connect(port: str) -> ESPLoader:
    esp = run_stub(detect_chip(port))
    attach_flash(esp)
    return esp


@app.command()
def check(config: Path) -> None:
    """Validate a configuration file."""
    with config.open() as f:
        jsonschema.validate(json.load(f), SCHEMA)


@app.command()
def write(config: Path, serial: Serial) -> None:
    """Write a configuration file to NVS as it is; the image is built in memory."""
    text = config.read_text()
    jsonschema.validate(json.loads(text), SCHEMA)

    offset, size = nvs_partition()
    image = io.BytesIO()
    with nvs.nvs_open(image, size, nvs.Page.VERSION2) as part:
        part.write_namespace("cfg")
        part.write_entry("config", text, "string")
    with connect(serial) as esp:
        write_flash(esp, [(offset, image.getvalue())])
        reset_chip(esp)


@app.command()
def erase(serial: Serial) -> None:
    """Erase the configuration in NVS."""
    offset, size = nvs_partition()
    with connect(serial) as esp:
        erase_region(esp, offset, size)
        reset_chip(esp)


if __name__ == "__main__":
    app()
