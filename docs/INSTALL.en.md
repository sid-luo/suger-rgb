# Install Suger RGB

## ⚠️ DO NOT USE ESP32-C3 SUPER MINI — USE ESP32-C3 PRO MINI

> [!WARNING]
> **Do not buy or use an ESP32-C3 Super Mini (SuperMini) for this project.** These boards may have unreliable Wi-Fi, an undiscoverable setup hotspot, or failed Wi-Fi setup. A successful firmware flash does not guarantee working Wi-Fi.
>
> These issues occurred during this project's testing and were resolved by switching to an **ESP32-C3 Pro Mini**. **Choose the ESP32-C3 Pro Mini for this project.**

[简体中文](INSTALL.md) · [Back to project home](../README.md)

The browser installer is recommended. It verifies the chip and writes the bootloader, partition table, and application as one complete ESP32-C3 image. ESP-IDF and command-line tools are not required.

## Browser installation

### Requirements

- ESP32-C3 Pro Mini with 4 MB flash;
- a USB data cable;
- a Windows, macOS, or Linux computer;
- desktop Chrome or Edge;
- a stable connection to load the page and download the roughly 1 MB image.

Safari and iPhone do not support browser serial flashing. Close serial monitors, Arduino IDE, PlatformIO, and any other application that may already own the port.

### Steps

1. Open the [Suger RGB web installer](https://rgb.sidluo.com/installer/).
2. Connect the board with the USB data cable.
3. Select “Install Suger RGB.”
4. Choose the ESP32-C3 serial port in the browser prompt.
5. Confirm installation. Browser flashing erases the previous firmware and saved setup details.
6. Confirm and wait for the board to restart.

The installer accepts ESP32-C3 devices only and refuses another detected chip family. Flashing happens directly between the browser and the local USB device; the page does not receive Wi-Fi passwords or Nightscout configuration. Setup is required again after every browser flash.

### After flashing

On first boot, the LEDs show a low-brightness white chase and the board creates the open network `Suger-RGB-XXXX`:

1. Join that network from a phone;
2. wait for the captive page, or open `http://192.168.4.1/`;
3. enter home Wi-Fi, the Nightscout address, and pixel count;
4. validate a real glucose reading and save.

The device restarts and closes the setup page after saving. Its LAN address does not expose an administration page during normal operation. To change settings later, physically hold BOOT for about five seconds.

## The browser cannot find the board

Try these in order:

1. use a known USB data cable;
2. try another computer USB port and avoid an unstable hub;
3. close every application or page using the serial port;
4. unplug the board, hold BOOT while reconnecting USB, then release BOOT;
5. if the board has RST, hold BOOT, tap RST, then release BOOT;
6. start installation again and select the newly appearing port.

Boards with an external USB-to-serial bridge may require a CH34x, CP210x, or vendor driver. ESP32-C3 native USB Serial/JTAG normally needs no additional driver.

## Build from source

The project uses ESP-IDF 5.4.x:

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

Windows ports normally look like `COM5`. Exit the monitor with `Ctrl+]`.

## Flash the merged image manually

The complete image used by the web installer is:

```text
docs/installer/firmware/suger-rgb-esp32c3.bin
```

With ESP-IDF or esptool installed, write it at address `0x0`:

```bash
esptool.py --chip esp32c3 --port PORT write_flash 0x0 \
  docs/installer/firmware/suger-rgb-esp32c3.bin
```

This merged image is the same one used by the browser installer. It overwrites saved setup data, so configuration is required again.

Each release also provides `SHA256SUMS.txt` for download verification.

## Update the web-installer image

Maintainers run this from an ESP-IDF environment:

```bash
./tools/package_web_firmware.sh
```

The script rebuilds the project, creates the merged image beginning at `0x0`, validates it, and updates the SHA-256 file. Complete [`RELEASE_CHECKLIST.md`](RELEASE_CHECKLIST.md) before publishing.
