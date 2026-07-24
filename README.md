# SoundPola Playback Firmware

Tuya T5AI Playback Board firmware for the SoundPola competition prototype.

## Responsibilities

- receive Playback Sessions and commands over the BLE Board Link;
- download device-ready media from the Cloud Media Service;
- render video on the attached 3.5-inch ILI9488 display;
- decode/output audio through Bluetooth A2DP Source;
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

`include/demo_network_config.h` is intentionally ignored because it contains Wi-Fi credentials, the fixed Playback Bearer authorization value, and the target speaker address. Beken classic Bluetooth addresses are stored in reverse byte order: visible `AA:BB:CC:DD:EE:FF` becomes `{0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA}`.

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
