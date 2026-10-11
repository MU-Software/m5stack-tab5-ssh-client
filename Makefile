# SPDX-License-Identifier: GPL-3.0-or-later
# esptool and the monitor run on the host: the IDF container cannot see USB serial devices.

BAUD ?= 921600
PORT ?= $(firstword $(wildcard /dev/cu.usbmodem*) $(wildcard /dev/ttyACM*))

APP     = build/app
IDF     = docker compose run --rm $(2) idf idf.py -C $(1) -B /workspace/build/$(3) -D SDKCONFIG=/workspace/build/$(3)/sdkconfig
ESPTOOL = uv run python -m esptool --chip esp32p4 --port $(PORT)

.PHONY: help build reconfigure menuconfig size clean ide check test test-linux flash monitor erase-flash erase-config c6-update require-port

help:                ## List the targets
	@grep -hE '^[a-z0-9-]+:.*## ' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*## "}; {printf "  %-14s %s\n", $$1, $$2}'

build:               ## Build the firmware (re-applies sdkconfig.defaults when it changed)
	@if [ sdkconfig.defaults -nt $(APP)/sdkconfig ]; then rm -f $(APP)/sdkconfig; fi
	$(call IDF,.,,app) build

reconfigure:         ## Discard sdkconfig and re-apply sdkconfig.defaults
	rm -f $(APP)/sdkconfig
	$(call IDF,.,,app) reconfigure

menuconfig:          ## Open menuconfig
	$(call IDF,.,,app) menuconfig

size:                ## Size per component
	$(call IDF,.,,app) size-components

clean:               ## Remove build output and managed components
	rm -rf build managed_components tools/c6-update/*/managed_components

ide: build          ## Compile database for VS Code with the IDF headers copied out of the container
	uv run python tools/ide.py

test:                ## Host tests under ASan+UBSan against a local sshd container
	tests/run.sh

test-linux:          ## The same in Linux under ASan+UBSan+LSan, then MSan
	tests/run.sh address,undefined linux
	tests/run.sh memory linux

check:               ## Run every pre-commit hook (format, cppcheck, clang-tidy, ...) on all files
	uv run pre-commit run --all-files

require-port:
	@test -n "$(PORT)" || { echo "no serial port found; pass PORT=/dev/cu.usbmodemXXXX"; exit 1; }

flash: build require-port  ## Build and flash over USB
	cd $(APP) && $(ESPTOOL) --baud $(BAUD) write-flash "@flash_args"

erase-flash: require-port  ## Erase the whole flash
	$(ESPTOOL) erase-flash

erase-config: require-port ## Erase the configuration in NVS (Wi-Fi networks)
	uv run python tools/config.py erase --serial $(PORT)

c6-update: require-port    ## Flash a one-shot image that updates the ESP32-C6's esp_hosted (then make monitor, make flash)
	$(call IDF,tools/c6-update/cp,-e IDF_TARGET=esp32c6,c6) build
	$(call IDF,tools/c6-update/host,,c6-update) -D C6_IMAGE=/workspace/build/c6/tab5_c6.bin build
	cd build/c6-update && $(ESPTOOL) --baud $(BAUD) write-flash "@flash_args"

monitor: require-port      ## Serial monitor with backtrace decoding
	uv run python -m esp_idf_monitor --port $(PORT) --target esp32p4 $(APP)/tab5_ssh.elf
