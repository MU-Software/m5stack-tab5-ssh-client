# m5stack-tab5-ssh-client

SSH client for the M5Stack Tab5 (ESP32-P4), built on wolfSSH. Work in progress: for now the USB serial port is the
terminal of one SSH session over Wi-Fi.

## Build and flash

Needs Docker (ESP-IDF 6.1 runs in `espressif/idf:v6.1`) and [uv](https://docs.astral.sh/uv/) for esptool on the host.

```sh
git submodule update --init
make build
make flash      # PORT=/dev/cu.usbmodemXXXX to pick the port
uv run python tools/config.py write CONFIG_JSON   # into NVS
make monitor
make c6-update  # once per Tab5: esp_hosted on the ESP32-C6 to the host's version, then make flash
```

## Configuration

Wi-Fi networks are a JSON document (`tools/config.schema.json`) in NVS, or on a FAT SD card as
`.config/tab5-ssh/config.json`, which wins. Where to connect comes from the SD card's `.ssh/`, used like `~/.ssh/`: the
first `Host` in `config` that is not a pattern (HostName, User, Port, IdentityFile), `known_hosts`, and the key (default
`id_ed25519`, `id_ecdsa`, `id_rsa`). Without a key the password is asked when connecting; an unknown host is asked
about and added to `known_hosts`. `tools/config.py check FILE` validates a configuration file; VS Code does it while
editing.

## Development

```sh
uv sync && uv run pre-commit install   # once
make ide                               # build/ide/compile_commands.json for VS Code
make check                             # clang-format, cppcheck, clang-tidy, file checks
make test test-linux                   # SSH tests against a local sshd under ASan/UBSan/LSan/MSan
```

## Licence

GPL-3.0-or-later (see `LICENSE`). wolfSSH and wolfSSL are GPL-3.0-or-later; ESP-IDF is Apache-2.0.
