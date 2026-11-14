# m5stack-tab5-ssh-client

SSH client for the M5Stack Tab5 (ESP32-P4) on wolfSSH, drawn through Ghostty and Shiroko.

## Build and flash

Needs Docker (`espressif/idf:v6.1.1`) and [uv](https://docs.astral.sh/uv/).

```sh
git submodule update --init
make build
make flash      # OTA over USB; esptool while the Tab5 shows USB-Serial/JTAG
make monitor    # logs
make sd-on      # SD card as USB mass storage, until make sd-off
make c6-update  # the ESP32-C6's esp_hosted (c6/), over USB
```

- Update the C6 first: Wi-Fi needs C6 protocol 9.
- The C6 keeps a new image once the Tab5 reaches it, else boots the one before after a minute; its bootloader has no
  rollback.
- USB-C: a terminal port and a control port (`tools/device.py`). The ROM loader needs a BOOT reset.
- A new Tab5 image is kept after 10 s with USB up, or over SSH after a login within 60 s; else it rolls back.
- Fonts: `build/shiroko-host/fonts/` to the SD card's `.local/share/tab5-ssh/fonts/`.

## SD card

- `.config/tab5-ssh/config.json`: Wi-Fi and settings (`tools/config.schema.json`, `tools/config.py check FILE`),
  written by the settings screen; defaults without it.
- `.config/tab5-ssh/ui.json`: replaces `main/ui.json`.
- `.ssh/`: `config` (non-pattern `Host`s), `known_hosts`, `id_ed25519`, `id_ecdsa`, `id_rsa` (else a password prompt),
  `authorized_keys` for remote control.
- `.local/state/tab5-ssh/`: `sessions.json`, `log/` (a file a day).

## Use

- Keys: the Tab5 keyboard, a USB keyboard on USB-A (boot protocol, no hubs), the USB terminal port. Korean (Dubeolsik):
  LANG1 or a short CapsLock (Aa), a long one locks capitals. Sym with 1…0, - and +: F1–F12.
- Touch on the terminal: a tap clicks, a drag turns the mouse wheel every `touch.wheel_px`, else Up/Down on the alternate
  screen (`touch.arrows`) or scrolls back. On screens: arrows, Tab, Enter; Esc cancels a dialog.
- Sessions: up to 4, each a tmux session `tab5-MMMMMM-RRRRRR` on its host (tmux required). None connects after a
  restart; a drop reconnects, a tmux detach disconnects, a tmux session gone deletes its record.
- Network: Wi-Fi while it reaches the internet, else LTE through a Stamp CAT-M (TX G6, RX G46).
- Sleep: the screen goes off after `sleep.idle_minutes` (0: never) or `sleep`. Without a computer on USB-C the Tab5 then
  light-sleeps, woken by the Tab5 keyboard, USB-C, or the C6 with `sleep.wifi_wake`; USB-A stays powered only with
  `sleep.usb_a`.

## Control

The control port (`tools/device.py send`) or SSH; an unknown command lists them.

- `[c6 ]nvs …`: values only in the reply, never in a log; over SSH also `nvs load` from stdin.
- `modem at COMMAND`: while PPP is down; some settings cost a network attach.
- A panic is logged at the next boot, also to the SD card, with a `make decode ADDRESSES="…"` line; `crash` repeats it.
- Remote control: `remote.enabled` and `.ssh/authorized_keys` (lines with options never match) open port 22 over Wi-Fi,
  LTE too with `remote.lte`, as `remote.user` when set. One command a connection: the control commands, `log`,
  `version`, `config` (Wi-Fi passwords included), `config set` and `[c6 ]update SIZE CRC32` from stdin
  (`TAB5_HOST=tab5 make flash`). Host key: `.config/tab5-ssh/ssh_host_ed25519_key`, its fingerprint in the log.

## Development

```sh
uv sync && uv run pre-commit install   # once
make ide                               # compile database for VS Code
make check                             # pre-commit on all files
make test test-linux                   # SSH tests against a local sshd under sanitizers
make sim                               # the UI in an SDL3 window (vcpkg), the mouse as touch
```

- `build/sim/tab5_sim [--input STEPS] [--shot FILE.bmp] main/ui.json build/shiroko-host/fonts`: steps in `sim/sim.c`.
- `main/ui.json`: `rows` and `columns` of children sized in cells, `"N%"` or `"Nfr"`, `"when": SETTING`; leaves `text`,
  `button`, `terminal`, `list`, `toggle`, `number`, `choice`, `field` (settings in `main/settings.c`).

## Licence

GPL-3.0-or-later. wolfSSH and wolfSSL: GPL-3.0-or-later; Shiroko: MIT (`third_party/shiroko/NOTICE`); zstd:
BSD-3-Clause; Ghostty (libghostty-vt, with uucode and Zig's compiler_rt): MIT; Wuffs: Apache-2.0 OR MIT; ESP-IDF and
the Tab5 BSP: Apache-2.0.
