# m5stack-tab5-ssh-client

SSH client for the M5Stack Tab5 (ESP32-P4) on wolfSSH. Work in progress: a USB serial port is the terminal.

## Build and flash

Needs Docker (`espressif/idf:v6.1`) and [uv](https://docs.astral.sh/uv/).

```sh
git submodule update --init
make build
make flash      # OTA over USB; esptool while the Tab5 shows USB-Serial/JTAG
make monitor    # logs
make sd-on      # SD card as USB mass storage, until make sd-off
make c6-update  # once per Tab5, from the ROM loader
```

USB-C is TinyUSB: a terminal port and a control port (`tools/device.py`). An OTA image that USB never comes up with
rolls back after 30 s. The ROM loader needs a BOOT reset.

## Configuration

- Wi-Fi: `.config/tab5-ssh/config.json` on the SD card, else NVS (`tools/config.py write`, ROM loader).
  Schema: `tools/config.schema.json`.
- SSH: the SD card's `.ssh/` as `~/.ssh/`: the first non-pattern `Host` in `config`, `known_hosts`, keys
  (`id_ed25519`, `id_ecdsa`, `id_rsa`). No key: password prompt. Unknown host: yes/no prompt.

## Development

```sh
uv sync && uv run pre-commit install   # once
make ide                               # compile database for VS Code
make check                             # pre-commit on all files
make test test-linux                   # SSH tests against a local sshd under sanitizers
```

## Licence

GPL-3.0-or-later. wolfSSH and wolfSSL: GPL-3.0-or-later; ESP-IDF: Apache-2.0.
