# m5stack-tab5-ssh-client

SSH client for the M5Stack Tab5 (ESP32-P4) on wolfSSH, drawn on the panel through Ghostty and Shiroko. Work in progress:
keys from M5Stack's Keyboard for Tab5, a USB keyboard on the USB-A port (boot protocol, no hubs) or the USB terminal
port. Korean (Dubeolsik): LANG1, or a short press of CapsLock (Aa on the Tab5 keyboard); a long press locks
capitals. Sym with 1…0, - and + on the Tab5 keyboard: F1–F12.

## Build and flash

Needs Docker (`espressif/idf:v6.1`) and [uv](https://docs.astral.sh/uv/).

```sh
git submodule update --init
make build
make flash      # OTA over USB; esptool while the Tab5 shows USB-Serial/JTAG
make monitor    # logs
make sd-on      # SD card as USB mass storage, until make sd-off
make c6-update  # the ESP32-C6's esp_hosted (tools/c6), over USB (logged at boot when it differs)
```

The C6 keeps a new image once the Tab5 reaches it through esp_hosted, else it boots the one before after a minute: its
bootloader has no rollback, and it can only be reflashed otherwise through the back-cover pads. Its log shows as `C6`.
It stays joined through restarts of the Tab5, which then only gets an address again.

USB-C is TinyUSB: a terminal port and a control port (`tools/device.py`). An OTA image is kept once it has run 10 s
and USB came up: one that USB never comes up with rolls back after 30 s, one that crashes before at the next restart.
The ROM loader needs a BOOT reset.

## Configuration

- Wi-Fi and settings: `.config/tab5-ssh/config.json` on the SD card, else NVS (`tools/config.py write`, ROM loader).
  Schema: `tools/config.schema.json`. The settings screen saves each change there, under the same keys.
- SSH: the SD card's `.ssh/` as `~/.ssh/`: the non-pattern `Host`s in `config`, `known_hosts`, keys
  (`id_ed25519`, `id_ecdsa`, `id_rsa`). No key: password prompt. Unknown host: yes/no prompt. With two hosts or more a
  list opens at boot and connects to the last one after 10 s, unless touched or typed into first.
- tmux: the session runs in `tmux new-session -s tab5-XXXXXXXX`, its name kept in NVS per `Host`; the next connection
  attaches to it, or starts another when it is gone. From tmux 3.2 the client declares truecolor, strikethrough,
  overline, underline styles and colours, and kitty images as Unicode placeholders (`-T`); from tmux 3.3 the session's
  windows allow passthrough, for kitty images from programs in it. No tmux on the host: a plain shell.
- Network: Wi-Fi while it reaches the SSH host, else LTE through a Stamp CAT-M (Tab5 TX G6, RX G46), kept connected
  in the background; LTE keeps redialing (waits of 90-960 s) unless no modem answered or spoke since boot. Without a path the
  top right says what LTE does, and Enter dials at once. A dropped or moved session reconnects to its tmux session; a
  closed one on Enter. Control commands: `wifi on|off` (`wifi off save`: also after restarts, until `wifi on` or
  `wifi connect`), `wifi connect SSID [PASSWORD]` (saved to the configuration in use), `wifi forget SSID`,
  `lte on|off|status` (`lte off save` as `wifi off save`), `net status`, `net check HOST PORT`.
- Modem: before each dial the firmware reads the registration, cell, signal, PDP context, SIM (ICCID masked) and back-off
  state; once a boot, when no back-off is in sight, also its settings, keeping QUALCOMM URCs and the battery check on and
  eDRX off while the network grants it (`AT+CURCCFG="QUALCOMM",1`, `AT+CBATCHK=1`, `AT+CEDRXS=0,4`, each only when
  needed). `modem status` logs what was read, `modem read` reads it again while PPP is down (`lte off`), and the
  settings screen's 모뎀 page shows it with its age.
- Slow links: keys typed at the end of a line show underlined until the host echoes them (only once it echoed one since
  the last Enter or other key, so not at password prompts); top right, the seconds typing has had no answer from the host,
  not even to a keepalive request, or a session reconnects. Over LTE each connection step may take 30 s.
- Logs: on the SD card in `.local/state/tab5-ssh/log/`, a file a day (`YYYY-MM-DD-NN.log`, a new `NN` past `file_kb`), the
  oldest deleted past `keep_mb` or `keep_days`; `log` and `timezone` (POSIX TZ, default `KST-9`) in the configuration.
  The time comes from the RTC, then SNTP (`pool.ntp.org`), which sets the RTC.
- Screen: a bar above the terminal (host, network, battery, time) and one below (buttons for the hosts, network and
  settings screens, which cover the terminal); touch, or arrows, Tab, Enter and Esc while one is open. The settings
  screen restarts the Tab5 too. `.config/tab5-ssh/ui.json` on the SD card replaces the built-in `main/ui.json`.
- Fonts: `build/shiroko-host/fonts/` (`*.shrf`, `NOTICE`, `LICENSES/`) to the SD card's `.local/share/tab5-ssh/fonts/`.

## Development

```sh
uv sync && uv run pre-commit install   # once
make ide                               # compile database for VS Code
make check                             # pre-commit on all files
make test test-linux                   # SSH tests against a local sshd under sanitizers
make sim                               # the UI in an SDL3 window (vcpkg), the mouse as touch
```

`make sim` draws `main/ui.json` as the Tab5 does, with made-up values and a stand-in terminal, and reads it again when
it is saved. `build/sim/tab5_sim [--input STEPS] [--shot FILE.bmp] main/ui.json build/shiroko-host/fonts` runs touches
and keys first (`tap:COLUMN,ROW key:tab text:abc`, see `sim/sim.c`) and saves the screen and quits.

`main/ui.json` lays the screens out in cells: `rows` stack their children by `height`, `columns` set them side by side
by `width`, each a number of cells, `"N%"` or `"Nfr"` (a share of the rest; the default), `"when": SETTING` hides one
while that setting is off. Leaves are `text` (`{name}` for a value, `{name:N}` padded to N cells), `button` (another
screen's name opens it over the terminal), `terminal`, `list` and the settings' editors `toggle`, `number`, `choice` and
`field` (`main/settings.c` lists the settings).

## Licence

GPL-3.0-or-later. wolfSSH and wolfSSL: GPL-3.0-or-later; Shiroko: MIT (`third_party/shiroko/NOTICE`); zstd:
BSD-3-Clause; Ghostty (libghostty-vt, with uucode and Zig's compiler_rt): MIT; Wuffs: Apache-2.0 OR MIT; ESP-IDF and
the Tab5 BSP: Apache-2.0.
