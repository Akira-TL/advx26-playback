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
