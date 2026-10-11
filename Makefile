# SPDX-License-Identifier: GPL-3.0-or-later
# esptool and the monitor run on the host: the IDF container cannot see USB serial devices.

BAUD ?= 921600

APP     = build/app
IDF     = docker compose run --rm $(2) idf idf.py -C $(1) -B /workspace/build/$(3) -D SDKCONFIG=/workspace/build/$(3)/sdkconfig
DEVICE  = uv run python $(CURDIR)/tools/device.py
SHIROKO = build/shiroko-host

.PHONY: help build reconfigure menuconfig size clean ide check test test-linux flash monitor erase-flash erase-config c6-update sd-on sd-off shiroko-host

help:                ## List the targets
	@grep -hE '^[a-z0-9-]+:.*## ' $(MAKEFILE_LIST) | awk 'BEGIN {FS = ":.*## "}; {printf "  %-14s %s\n", $$1, $$2}'

build: $(SHIROKO)/generated  ## Build the firmware (re-applies sdkconfig.defaults when it changed)
	@if [ sdkconfig.defaults -nt $(APP)/sdkconfig ]; then rm -f $(APP)/sdkconfig; fi
	$(call IDF,.,,app) build

shiroko-host:        ## Shiroko's generated sources and the 12x24 font packages (fonts/ goes to the SD card's .local/share/tab5-ssh/)
	$(MAKE) -C third_party/shiroko tab5-host TAB5_HOST=$(CURDIR)/$(SHIROKO) TAB5_CELL_WIDTH=12 TAB5_CELL_HEIGHT=24

$(SHIROKO)/generated:
	$(MAKE) shiroko-host

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

flash: build         ## Build and install: an OTA update over USB, or esptool while USB-Serial/JTAG has the port
	$(DEVICE) flash $(APP)/tab5_ssh.bin

erase-flash:         ## Erase the whole flash
	$(DEVICE) esptool erase-flash

erase-config:        ## Erase the configuration in NVS (Wi-Fi networks)
	uv run python tools/config.py erase

sd-on:               ## Hand the SD card to this computer as USB mass storage (the terminal cannot use it meanwhile)
	$(DEVICE) send "sd on"

sd-off:              ## Give the SD card back to the firmware
	$(DEVICE) send "sd off"

c6-update:           ## Flash a one-shot image that updates the ESP32-C6's esp_hosted (then make monitor, make flash)
	$(call IDF,tools/c6-update/cp,-e IDF_TARGET=esp32c6,c6) build
	$(call IDF,tools/c6-update/host,,c6-update) -D C6_IMAGE=/workspace/build/c6/tab5_c6.bin build
	cd build/c6-update && $(DEVICE) esptool --baud $(BAUD) write-flash "@flash_args"

monitor:             ## Logs from the control port, with backtrace decoding
	uv run python -m esp_idf_monitor --no-reset --port $$($(DEVICE) port control) --target esp32p4 $(APP)/tab5_ssh.elf
