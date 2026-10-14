# m5stack-tab5-ssh-client

SSH client for the M5Stack Tab5 (ESP32-P4) on wolfSSH, drawn on the panel through Ghostty and Shiroko. Work in progress:
keys from M5Stack's Keyboard for Tab5, a USB keyboard on the USB-A port (boot protocol, no hubs) or the USB terminal
port. Korean (Dubeolsik): LANG1, or a short press of CapsLock (Aa on the Tab5 keyboard); a long press locks
capitals.

## Build and flash

Needs Docker (`espressif/idf:v6.1`) and [uv](https://docs.astral.sh/uv/).

```sh
git submodule update --init
make build
make flash      # OTA over USB; esptool while the Tab5 shows USB-Serial/JTAG
make monitor    # logs
make sd-on      # SD card as USB mass storage, until make sd-off
make c6-update  # the ESP32-C6's esp_hosted, over USB (logged at boot when it differs)
```

USB-C is TinyUSB: a terminal port and a control port (`tools/device.py`). An OTA image that USB never comes up with
rolls back after 30 s. The ROM loader needs a BOOT reset.

## Configuration

- Wi-Fi: `.config/tab5-ssh/config.json` on the SD card, else NVS (`tools/config.py write`, ROM loader).
  Schema: `tools/config.schema.json`.
- SSH: the SD card's `.ssh/` as `~/.ssh/`: the first non-pattern `Host` in `config`, `known_hosts`, keys
  (`id_ed25519`, `id_ecdsa`, `id_rsa`). No key: password prompt. Unknown host: yes/no prompt.
- tmux: the session runs in `tmux new-session -s tab5-XXXXXXXX`, its name kept in NVS per `Host`; the next connection
  attaches to it, or starts another when it is gone. From tmux 3.2 the client declares truecolor, strikethrough,
  overline, underline styles and colours, and kitty images as Unicode placeholders (`-T`); from tmux 3.3 the session's
  windows allow passthrough, for kitty images from programs in it. No tmux on the host: a plain shell.
- Network: Wi-Fi while it reaches the SSH host, else LTE through a Stamp CAT-M (Tab5 TX G6, RX G46), kept connected
  in the background. A dropped or moved session reconnects to its tmux session; a closed one on Enter. Control
  commands: `wifi on|off`, `wifi connect SSID [PASSWORD]` (saved to the configuration in use), `wifi forget SSID`,
  `lte on|off|status`, `net status`, `net check HOST PORT`.
- Fonts: `build/shiroko-host/fonts/` (`*.shrf`, `NOTICE`, `LICENSES/`) to the SD card's `.local/share/tab5-ssh/fonts/`.

## Development

```sh
uv sync && uv run pre-commit install   # once
make ide                               # compile database for VS Code
make check                             # pre-commit on all files
make test test-linux                   # SSH tests against a local sshd under sanitizers
```

## Licence

GPL-3.0-or-later. wolfSSH and wolfSSL: GPL-3.0-or-later; Shiroko: MIT (`third_party/shiroko/NOTICE`); zstd:
BSD-3-Clause; Ghostty (libghostty-vt, with uucode and Zig's compiler_rt): MIT; Wuffs: Apache-2.0 OR MIT; ESP-IDF and
the Tab5 BSP: Apache-2.0.
