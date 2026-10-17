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
It keeps a copy of the known networks and joins them itself (the first one a scan sees, in their order; again after a
drop), so it stays joined through restarts of the Tab5, which then only gets an address again.

`nvs list|get NAMESPACE KEY|dump [NAMESPACE]|set NAMESPACE KEY TYPE VALUE|erase NAMESPACE [KEY]` reads and changes
the Tab5's NVS, `c6 nvs …` the C6's, over USB (`tools/device.py send`) or SSH; values only in the reply, never in a log.
TYPE `u8`…`i64`, `str`, `blob` (hex); a command line holds at most 511 bytes. Over SSH, `[c6 ]nvs load [NAMESPACE]`
replaces the namespace, or all of NVS, with what a dump wrote, from stdin: checked whole first, empty input erases.

USB-C is TinyUSB: a terminal port and a control port (`tools/device.py`). An OTA image is kept once it has run 10 s
and USB came up: one that USB never comes up with rolls back after 30 s, one that crashes before at the next restart.
The ROM loader needs a BOOT reset.

With `remote.enabled` on (off by default) and `.ssh/authorized_keys` on the SD card (lines with options never match),
the Tab5 answers SSH on port 22 for those keys, over Wi-Fi (and LTE with `remote.lte`), as `remote.user` only when set;
one command a connection: the control commands (the log follows each until it is quiet for a second), `log` (the RAM
ring's last 64 KiB, then live), `version`, `restart`, `config` (the configuration in use, Wi-Fi passwords included),
`config set` (a whole one on stdin, checked as at boot, for the next start), `update`/`c6 update SIZE CRC32` with the
image on stdin (`TAB5_HOST=tab5 make flash`, `tools/device.py --host`). Its Ed25519 host key is
`.config/tab5-ssh/ssh_host_ed25519_key` on the SD card (PKCS#8 PEM), made when the port first opens, with a `.pub`
beside it for `known_hosts`; the log shows its fingerprint. An image sent this way is kept once a key logs in again within
60 s and it has run 10 s; USB does not count for it.

## Configuration

- Wi-Fi and settings: `.config/tab5-ssh/config.json` on the SD card, nowhere else; without the card or the file the Tab5
  runs on the defaults and says so in the log and on the settings screen. Schema: `tools/config.schema.json`
  (`tools/config.py check FILE`); Wi-Fi under `network.wifi.access_points`, with `network.wifi.enabled` and
  `network.lte.enabled`. The settings screen saves each change there, under the same keys, making the file when there is
  none (never over one that is not JSON).
- SSH: the SD card's `.ssh/` as `~/.ssh/`: the non-pattern `Host`s in `config`, `known_hosts`, keys
  (`id_ed25519`, `id_ecdsa`, `id_rsa`). No key: password prompt. Unknown host: yes/no prompt. With two hosts or more and
  no session to restore, a list opens at boot and connects to the last one after 10 s, unless touched or typed into first.
- tmux: each session runs in its own `tmux new-session -s tab5-MMMMMM-RRRRRR` (the MAC's last three bytes, then random),
  its name kept in NVS; the next connection attaches to it, or starts another when it is gone. From tmux 3.2 the client
  declares truecolor, strikethrough, overline, underline styles and colours, and kitty images as Unicode placeholders
  (`-T`); from tmux 3.3 the session's windows allow passthrough, for kitty images from programs in it; its `mouse` is
  on, for touch. Each connection to a host also kills this Tab5's tmux sessions there that no open session uses,
  unattached and older than a minute. No tmux on the host: a plain shell.
- Sessions: up to 4 at once, to one host or several, each with its own terminal, kept connected and read while hidden,
  and reopened after a restart. The sessions screen (세션 on the bottom bar) lists them, shows the chosen one (전환) or
  ends it with its tmux session (끊기, not the last one), and opens a new one to a host from its list (hidden while 4 are
  open). From two on, each also has a tab on the top bar in place of the host, which shows it, or opens the sessions
  screen when it is shown (`?`: it waits for a password or a host key, asked only once it is shown).
- Network: Wi-Fi while it reaches the SSH host (the shown session's), else LTE through a Stamp CAT-M (Tab5 TX G6, RX G46), kept connected
  in the background; LTE keeps redialing (waits of 90-960 s) unless no modem answered or spoke since boot. Without a path the
  top right says what LTE does, and Enter dials at once. A dropped or moved session reconnects to its tmux session; a
  closed one on Enter. Control commands: `wifi on|off` (for this boot; with `save` also `network.wifi.enabled`, as the
  network screen's buttons and `wifi connect` do), `wifi connect SSID [PASSWORD]` (saved to the configuration),
  `wifi forget SSID`, `lte on|off [save]|status`, `net status`, `net check HOST PORT`, `settings`,
  `set NAME VALUE` (as in the configuration; text also without quotes).
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
- Screen: a bar above the terminal (host or session tabs, network, battery, time) and one below (buttons for the
  sessions, network and settings screens, which cover the terminal); touch, or arrows, Tab, Enter and Esc while one is
  open. On the terminal a tap clicks and a drag turns the wheel every `touch.wheel_px` (48) for a program that asked for
  the mouse (tmux, vim with `mouse=a`, htop); otherwise a drag is Up/Down on the alternate screen (`touch.arrows`, as
  Ghostty's alternate scroll) or scrolls the scrollback until the next key. The settings screen restarts the Tab5 too.
  The network screen lists the saved Wi-Fi networks in the order they are tried; one opens to change its password, move
  it up, join or delete it, 추가 adds one from a scan or typed (연결 saves it once joined). `.config/tab5-ssh/ui.json` on
  the SD card replaces the built-in `main/ui.json`.
- Sleep: after `sleep.idle_minutes` without a key or a touch (화면 끄기; 0, the default: never), or on `sleep`, the screen
  goes off; a key or `wake` turns it on, the key going no further (touch does not). While no computer uses USB-C the
  Tab5 then light-sleeps in 5 s steps, awake 0.1 s between them for the network and the sessions; only the Tab5 keyboard
  and a computer on USB-C wake it, with `sleep.wifi_wake` (experimental) also the C6's traffic. USB-A has no power
  meanwhile unless `sleep.usb_a` (잘 때 USB-A): a USB keyboard cannot wake it. `power profile`: once USB-C is unplugged,
  the battery's draw with the screen on, the backlight off, the panel asleep, USB-A off and in light sleep, to the log.
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
`field` (`main/settings.c` lists the settings; a `field` of another name edits a value of the screens', shown as `*`
with `"secret": true`).

## Licence

GPL-3.0-or-later. wolfSSH and wolfSSL: GPL-3.0-or-later; Shiroko: MIT (`third_party/shiroko/NOTICE`); zstd:
BSD-3-Clause; Ghostty (libghostty-vt, with uucode and Zig's compiler_rt): MIT; Wuffs: Apache-2.0 OR MIT; ESP-IDF and
the Tab5 BSP: Apache-2.0.
