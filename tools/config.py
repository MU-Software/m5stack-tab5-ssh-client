# SPDX-License-Identifier: GPL-3.0-or-later
"""Device configuration: the JSON document of tools/config.schema.json, the SD card's .config/tab5-ssh/config.json."""

import json
from pathlib import Path

import jsonschema
import typer

SCHEMA = json.loads((Path(__file__).resolve().parent / "config.schema.json").read_text())

app = typer.Typer(no_args_is_help=True)


@app.command()
def check(config: Path) -> None:
    """Validate a configuration file."""
    jsonschema.validate(json.loads(config.read_text()), SCHEMA)


@app.callback()
def main() -> None:
    """The Tab5's configuration file."""


if __name__ == "__main__":
    app()
