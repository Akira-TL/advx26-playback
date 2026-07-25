# Board Link UART v1

## Purpose

Board Link UART is the wired control transport between the Trigger Board and the Playback Board. It carries the same command and report JSON bodies already used by the BLE Board Link, so playback has one control model and one execution engine.

The previous URL-only P06/P07 proposal is retired. P06/P07 do not expose a hardware UART function on BK7258 and must not be used for this link.

## Hardware contract

Signals are named from the Playback Board perspective.

| P11 pin | GPIO | Hardware function | Direction | Meaning |
|---:|---:|---|---|---|
| 1 | P10 / GPIO10 | UART0 RX | input | Playback receives Trigger TX |
| 2 | P11 / GPIO11 | UART0 TX | output | Playback sends reports to Trigger RX |
| 13 or 17 | GND | ground | — | common signal ground |

Cross-connect the two boards:

```text
Trigger TX  ---> Playback P11-1 / P10 / UART0 RX
Trigger RX  <--- Playback P11-2 / P11 / UART0 TX
Trigger GND ----- Playback P11-13 or P11-17 / GND
```

Electrical requirements:

- 3.3 V TTL only;
- TX and RX are crossed;
- both boards share ground;
- do not connect either signal to 5 V;
- no RTS/CTS flow control.

P10/P11 are also labelled `SD D2` and `SD D3` on the expansion header. The Playback firmware does not use the SD interface and remaps them to UART0 when the application starts.

## UART configuration

```text
Port:         UART0
Baud rate:    115200
Data bits:    8
Parity:       none
Stop bits:    1
Flow control: none
Encoding:     UTF-8 JSON
Framing:      one JSON object followed by LF
```

The receiver accepts both `\n` and `\r\n`. Empty lines are ignored. A command body may contain at most 4096 bytes. An overlength line is discarded through the next LF and produces a `MALFORMED_MESSAGE` NACK when the link is able to respond.

## Protocol behavior

The sender must begin each connection with `HELLO`. Until a valid `HELLO` is accepted, all other commands receive a `PROTOCOL_INCOMPATIBLE` NACK with diagnostic `HELLO required`.

After `HELLO_ACK`, UART accepts the complete Board Link command set:

- `GET_STATUS`;
- `LOAD_SESSION`;
- `PLAY`;
- `PAUSE`;
- `SEEK_MS`;
- `STOP`.

`LOAD_SESSION` carries the complete resolved Playback Session, including MP4, MP3 and MP3-index assets plus their metadata. The Trigger Board obtains that session from the backend and forwards it without inventing media metadata on the device.

Playback reports are serialized as one LF-terminated JSON object and include:

- `HELLO_ACK`;
- `ACK`;
- `NACK`;
- `STATE`;
- `PROGRESS`;
- `COMPLETED`;
- `ERROR`.

BLE GATT and UART feed the same `playback_engine`. They are transport adapters, not separate playback implementations.

## Minimal exchange

Trigger sends:

```json
{"schema_version":1,"type":"HELLO","session_id":null,"sequence_id":1,"payload":{"protocol_major":1,"protocol_minor":0,"role":"TRIGGER","boot_id":"trigger-boot-1","max_message_bytes":4096,"capabilities":["BOARD_LINK_V1","T5AI_H264_MP3_V1"]}}
```

Playback replies with `HELLO_ACK`. Trigger can then request current state:

```json
{"schema_version":1,"type":"GET_STATUS","session_id":null,"sequence_id":2,"payload":{}}
```

For playback, Trigger sends a valid `LOAD_SESSION` document using the same schema accepted by `tools/playback_ble_test.py send`.

## Startup diagnostics

Successful Playback startup includes:

```text
Board Link UART0 ready: RX=P10/P11-1 TX=P11/P11-2 baud=115200
Playback application ready; media starts only from Board Link LOAD_SESSION
```

After a valid UART handshake, debug logging includes:

```text
Board Link UART started=1 handshake=1 received=1 rejected=0
```

## Flashing note

UART0 is also used by the T5AI bootloader path. The application owns P10/P11 only after boot. Flashing remains possible by resetting the board into the normal download flow; disconnect the peer TX temporarily if it drives unsolicited data during reset or flashing.
