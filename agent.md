# Agent instructions

## Scope

This repository contains only the SoundPola Playback Board firmware.

- Do not add Trigger Board NFC, BLE Central, or control-panel implementation here.
- Playback Board is an output-only executor.
- The parent integration repository is `/home/akira/Projects/advx26`.
- TuyaOpen SDK is expected at `/home/akira/SDKs/TuyaOpen-v1.9.0`.

## Hardware safety

Only operate on the Playback Board USB serial identity `5AAE167197`. Use `/dev/serial/by-id/` paths; never infer the target from mutable `/dev/ttyACM*` numbering.

## Workflow

1. Decide whether the previous slice should be committed.
2. Plan the next slice.
3. Implement.
4. Run syntax/build checks.
5. Show all branches.

Commit messages use lowercase `fix` or `feat`, a module, and a Chinese description, for example:

```text
feat(playback): 实现媒体会话状态机
```
