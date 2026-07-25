# SoundPola Playback Firmware

Tuya T5AI Playback Board firmware for the SoundPola competition prototype.

## Responsibilities

- receive full Playback Sessions and commands over BLE GATT or P11 UART Board Link;
- download device-ready media from the Cloud Media Service;
- render video on the attached 3.5-inch ILI9488 display;
- decode/output audio through the board's wired mono speaker;
- report acknowledgements, state, progress, completion, and errors;
- remain output-only: user interaction belongs to the Trigger Board.

## Hardware baseline

- board: Tuya T5AI development board;
- TuyaOpen board choice: `TUYA_T5AI_BOARD`;
- display: 3.5-inch ILI9488 with GT1151 touch controller;
- logical playback canvas: 480×320 landscape.

## Layout

```text
CMakeLists.txt
app_default.config
config/
include/
src/
```

## Local device configuration

Copy the tracked template before building:

```bash
cp include/demo_network_config.example.h include/demo_network_config.h
```

`include/demo_network_config.h` is intentionally ignored because it contains Wi-Fi credentials and the fixed Playback Bearer authorization value.

## P11 UART Board Link

The wired transport is defined in [`docs/serial-url-link-v1.md`](docs/serial-url-link-v1.md). It uses hardware UART0 at 115200 8N1:

```text
P11-1 / P10 = Playback RX
P11-2 / P11 = Playback TX
P11-13/17   = common GND
```

UART carries LF-terminated Board Link JSON and supports the same `HELLO`, `LOAD_SESSION`, `PLAY`, `PAUSE`, `SEEK_MS`, `STOP`, status and report semantics as BLE GATT. P06/P07 are not hardware-UART pins and are not used.

## Build

The parent `advx26` repository owns shared TuyaOpen SDK patching and integration checks. From the parent checkout:

```bash
cd /home/akira/Projects/advx26
bash scripts/apply-tuyaopen-patches.sh ~/SDKs/TuyaOpen-v1.9.0
source ~/SDKs/TuyaOpen-v1.9.0/export.sh
cd firmware/playback
tos.py check
tos.py config choice -c TUYA_T5AI_BOARD_LCD_3.5.config
tos.py build
```

Generated `.build/` and `dist/` content is local only.

## Host-side UART smoke test

After flashing, the board's UART0 USB interface can validate the same P10/P11 transport before connecting the Trigger Board:

```bash
cd /home/akira/Projects/advx26/firmware/playback
uv run --with pyserial tools/playback_uart_test.py status
```

To send a complete Board Link command document:

```bash
uv run --with pyserial tools/playback_uart_test.py send /path/to/command.json
```

The default port is `/dev/serial/by-id/usb-1a86_USB_Dual_Serial_5AAE167197-if00`; override it with `--port` when needed.

## Host-side BLE smoke test

The Playback UI remains idle until a Board Link `LOAD_SESSION` command arrives. A host-side client can verify advertising, HELLO negotiation, notifications, and command handling without the Trigger Board:

```bash
cd /home/akira/Projects/advx26/firmware/playback
uv run --with bleak tools/playback_ble_test.py status
```

To send a complete Board Link command document, including `LOAD_SESSION`:

```bash
uv run --with bleak tools/playback_ble_test.py send /path/to/command.json
```

Use `--address XX:XX:XX:XX:XX:XX` when BlueZ does not expose the scan-response name. Keep the serial log open while testing; successful startup includes `Playback application ready`, and BLE connection changes are logged as `Board Link connected=...`.
