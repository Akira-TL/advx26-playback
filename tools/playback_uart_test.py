#!/usr/bin/env python3
"""Smoke-test Playback Board Link over UART0/P10/P11."""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any

DEFAULT_PORT = "/dev/serial/by-id/usb-1a86_USB_Dual_Serial_5AAE167197-if00"
BAUDRATE = 115200
HELLO_SEQUENCE_ID = 1
STATUS_SEQUENCE_ID = 2


def compact_json(document: dict[str, Any]) -> bytes:
    return json.dumps(
        document,
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode("utf-8")


def hello_command() -> dict[str, Any]:
    return {
        "schema_version": 1,
        "type": "HELLO",
        "session_id": None,
        "sequence_id": HELLO_SEQUENCE_ID,
        "payload": {
            "protocol_major": 1,
            "protocol_minor": 0,
            "role": "TRIGGER",
            "boot_id": f"uart-host-{int(time.time())}",
            "max_message_bytes": 4096,
            "capabilities": ["BOARD_LINK_V1", "T5AI_H264_MP3_V1"],
        },
    }


def status_command() -> dict[str, Any]:
    return {
        "schema_version": 1,
        "type": "GET_STATUS",
        "session_id": None,
        "sequence_id": STATUS_SEQUENCE_ID,
        "payload": {},
    }


def load_document(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError(f"读取命令 JSON 失败：{error}") from error
    if not isinstance(document, dict):
        raise ValueError("命令 JSON 顶层必须是对象")
    sequence_id = document.get("sequence_id")
    if not isinstance(sequence_id, int) or sequence_id <= HELLO_SEQUENCE_ID:
        raise ValueError("命令 JSON 的 sequence_id 必须是大于 1 的整数")
    return document


def send_document(serial_port: Any, document: dict[str, Any]) -> None:
    body = compact_json(document)
    if len(body) > 4096:
        raise ValueError(f"命令超过 4096 bytes：{len(body)}")
    serial_port.write(body + b"\n")
    serial_port.flush()
    print(f"TX {document.get('type', 'UNKNOWN')} sequence={document.get('sequence_id')} bytes={len(body)}")


def read_report(serial_port: Any, deadline: float) -> dict[str, Any] | None:
    while time.monotonic() < deadline:
        raw_line = serial_port.readline()
        if not raw_line:
            continue
        line = raw_line.strip()
        if not line:
            continue
        try:
            report = json.loads(line.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            print(f"RX non-JSON: {line[:160]!r}")
            continue
        if not isinstance(report, dict):
            print(f"RX ignored non-object: {report!r}")
            continue
        print(json.dumps(report, ensure_ascii=False, indent=2))
        return report
    return None


def wait_for_sequence(serial_port: Any, sequence_id: int, timeout: float) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while True:
        report = read_report(serial_port, deadline)
        if report is None:
            raise TimeoutError(f"等待 sequence_id={sequence_id} 报告超时")
        if report.get("sequence_id") == sequence_id:
            return report


def run(args: argparse.Namespace) -> int:
    try:
        import serial
    except ImportError:
        print(
            "缺少 pyserial；请使用：uv run --with pyserial tools/playback_uart_test.py status",
            file=sys.stderr,
        )
        return 2

    document = status_command() if args.action == "status" else load_document(args.json_file)
    with serial.Serial(
        args.port,
        BAUDRATE,
        timeout=0.2,
        write_timeout=2.0,
    ) as serial_port:
        serial_port.reset_input_buffer()
        serial_port.reset_output_buffer()

        send_document(serial_port, hello_command())
        hello_report = wait_for_sequence(serial_port, HELLO_SEQUENCE_ID, args.report_timeout)
        if hello_report.get("type") != "HELLO_ACK":
            raise RuntimeError(f"HELLO 握手失败：{hello_report.get('type')}")

        send_document(serial_port, document)
        wait_for_sequence(serial_port, int(document["sequence_id"]), args.report_timeout)

        listen_deadline = time.monotonic() + args.listen_seconds
        while time.monotonic() < listen_deadline:
            if read_report(serial_port, listen_deadline) is None:
                break
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default=DEFAULT_PORT, help="UART0 串口路径")
    parser.add_argument("--report-timeout", type=float, default=8.0)
    parser.add_argument("--listen-seconds", type=float, default=1.0)

    subparsers = parser.add_subparsers(dest="action", required=True)
    subparsers.add_parser("status", help="完成 HELLO 后读取 Playback 状态")
    send_parser = subparsers.add_parser("send", help="发送完整 Board Link 命令 JSON")
    send_parser.add_argument("json_file", type=Path)
    return parser.parse_args()


def main() -> int:
    try:
        return run(parse_args())
    except KeyboardInterrupt:
        return 130
    except Exception as error:
        print(f"测试失败：{type(error).__name__}: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
