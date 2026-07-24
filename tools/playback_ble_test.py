#!/usr/bin/env python3
"""Host-side smoke client for the SoundPola Playback Board Link GATT service."""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import secrets
import struct
import sys
import time
from pathlib import Path
from typing import Any

DEVICE_NAME = "SoundPola Playback"
SERVICE_UUID = "68d887b5-dd75-4af9-81a3-80af3301c051"
COMMAND_UUID = "c66ff7a5-ec6e-46eb-b679-d837de1c780e"
REPORT_UUID = "d1b3806a-61c9-4a7c-a943-d685afcdefbe"
WIRE_MAGIC = 0xA7
PROTOCOL_MAJOR = 1
COMMAND_KIND = 0x01
REPORT_KIND = 0x02
HEADER = struct.Struct("<BBBBIHHHH")
MAX_ATT_VALUE_SIZE = 244
DEFAULT_ATT_VALUE_SIZE = 20


class ReportAssembler:
    def __init__(self, queue: asyncio.Queue[dict[str, Any]]) -> None:
        self._queue = queue
        self._messages: dict[int, dict[str, Any]] = {}

    def feed(self, _sender: Any, data: bytearray) -> None:
        raw = bytes(data)
        if len(raw) < HEADER.size:
            print(f"忽略过短通知：{len(raw)} bytes", file=sys.stderr)
            return

        magic, protocol, kind, flags, message_id, index, count, length, reserved = HEADER.unpack_from(raw)
        payload = raw[HEADER.size:]
        if (
            magic != WIRE_MAGIC
            or protocol != PROTOCOL_MAJOR
            or kind != REPORT_KIND
            or flags != 0
            or reserved != 0
            or message_id == 0
            or count == 0
            or index >= count
            or length != len(payload)
        ):
            print("忽略非法 Board Link 通知", file=sys.stderr)
            return

        message = self._messages.setdefault(
            message_id,
            {"count": count, "parts": {}, "started": time.monotonic()},
        )
        if message["count"] != count:
            self._messages.pop(message_id, None)
            print("忽略分片数量冲突的通知", file=sys.stderr)
            return

        message["parts"][index] = payload
        if len(message["parts"]) != count:
            return

        try:
            body = b"".join(message["parts"][part] for part in range(count))
            report = json.loads(body.decode("utf-8"))
        except (KeyError, UnicodeDecodeError, json.JSONDecodeError) as error:
            print(f"解析报告失败：{error}", file=sys.stderr)
        else:
            self._queue.put_nowait(report)
        finally:
            self._messages.pop(message_id, None)


def compact_json(document: dict[str, Any]) -> bytes:
    return json.dumps(document, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def build_fragments(body: bytes, att_value_size: int) -> list[bytes]:
    if not body:
        raise ValueError("Board Link 消息不能为空")
    if att_value_size <= HEADER.size:
        raise ValueError("ATT value size 必须大于 16")

    payload_capacity = att_value_size - HEADER.size
    fragment_count = math.ceil(len(body) / payload_capacity)
    if fragment_count > 0xFFFF:
        raise ValueError("Board Link 消息分片过多")

    message_id = secrets.randbelow(0xFFFFFFFF) + 1
    fragments: list[bytes] = []
    for index in range(fragment_count):
        start = index * payload_capacity
        payload = body[start : start + payload_capacity]
        header = HEADER.pack(
            WIRE_MAGIC,
            PROTOCOL_MAJOR,
            COMMAND_KIND,
            0,
            message_id,
            index,
            fragment_count,
            len(payload),
            0,
        )
        fragments.append(header + payload)
    return fragments


async def resolve_device(scanner_type: Any, address: str | None, timeout: float) -> Any:
    normalized_address = address.upper() if address else None

    def matches(candidate: Any, advertisement: Any) -> bool:
        advertised_services = {
            value.lower() for value in (advertisement.service_uuids or [])
        }
        return (
            (normalized_address is not None and candidate.address.upper() == normalized_address)
            or advertisement.local_name == DEVICE_NAME
            or candidate.name == DEVICE_NAME
            or SERVICE_UUID in advertised_services
        )

    device = await scanner_type.find_device_by_filter(matches, timeout=timeout)
    if device is None:
        target = address or DEVICE_NAME
        raise RuntimeError(f"未发现 Playback BLE 设备：{target}")
    return device


async def negotiate_att_value_size(client: Any, override: int | None) -> int:
    if override is not None:
        if override <= HEADER.size or override > MAX_ATT_VALUE_SIZE:
            raise ValueError("--att-value-size 必须在 17..244 之间")
        return override

    backend = getattr(client, "_backend", None)
    acquire_mtu = getattr(backend, "_acquire_mtu", None)
    if acquire_mtu is not None:
        try:
            await acquire_mtu()
        except Exception:
            pass

    mtu = int(getattr(client, "mtu_size", 23) or 23)
    return min(max(mtu - 3, DEFAULT_ATT_VALUE_SIZE), MAX_ATT_VALUE_SIZE)


async def send_command(
    client: Any,
    document: dict[str, Any],
    att_value_size: int,
    fragment_delay_seconds: float,
) -> None:
    body = compact_json(document)
    fragments = build_fragments(body, att_value_size)
    print(
        f"发送 {document.get('type', 'UNKNOWN')}：{len(body)} bytes，"
        f"{len(fragments)} 个分片，ATT value={att_value_size}"
    )
    for fragment in fragments:
        await client.write_gatt_char(COMMAND_UUID, fragment, response=True)
        if fragment_delay_seconds > 0:
            await asyncio.sleep(fragment_delay_seconds)


async def wait_for_sequence(
    queue: asyncio.Queue[dict[str, Any]],
    sequence_id: int,
    timeout: float,
) -> dict[str, Any]:
    deadline = asyncio.get_running_loop().time() + timeout
    while True:
        remaining = deadline - asyncio.get_running_loop().time()
        if remaining <= 0:
            raise TimeoutError(f"等待 sequence_id={sequence_id} 报告超时")
        report = await asyncio.wait_for(queue.get(), timeout=remaining)
        print(json.dumps(report, ensure_ascii=False, indent=2))
        if report.get("sequence_id") == sequence_id:
            return report


def hello_command(sequence_id: int) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "type": "HELLO",
        "session_id": None,
        "sequence_id": sequence_id,
        "payload": {
            "protocol_major": 1,
            "protocol_minor": 0,
            "role": "TRIGGER",
            "boot_id": f"host-test-{int(time.time())}",
            "max_message_bytes": 4096,
            "capabilities": [],
        },
    }


def status_command(sequence_id: int) -> dict[str, Any]:
    return {
        "schema_version": 1,
        "type": "GET_STATUS",
        "session_id": None,
        "sequence_id": sequence_id,
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
    if not isinstance(sequence_id, int) or sequence_id <= 0:
        raise ValueError("命令 JSON 必须包含正整数 sequence_id")
    return document


async def run(args: argparse.Namespace) -> int:
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        print(
            "缺少 bleak；请使用：uv run --with bleak tools/playback_ble_test.py status",
            file=sys.stderr,
        )
        return 2

    device = await resolve_device(BleakScanner, args.address, args.scan_timeout)
    print(f"连接 {device.name or DEVICE_NAME} ({device.address})")

    queue: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
    assembler = ReportAssembler(queue)
    async with BleakClient(device, timeout=args.connect_timeout) as client:
        available_uuids = {
            characteristic.uuid.lower()
            for service in client.services
            for characteristic in service.characteristics
        }
        if COMMAND_UUID not in available_uuids or REPORT_UUID not in available_uuids:
            discovered = ", ".join(sorted(available_uuids))
            raise RuntimeError(f"未发现 Board Link 特征；设备特征：{discovered}")
        await client.start_notify(REPORT_UUID, assembler.feed)
        att_value_size = await negotiate_att_value_size(client, args.att_value_size)
        print(f"已连接，MTU={att_value_size + 3}，ATT value={att_value_size}")

        await send_command(
            client,
            hello_command(1),
            att_value_size,
            args.fragment_delay_ms / 1000.0,
        )
        hello_report = await wait_for_sequence(queue, 1, args.report_timeout)
        if hello_report.get("type") not in {"HELLO_ACK", "ACK"}:
            raise RuntimeError("Playback HELLO 握手未成功")

        if args.action == "status":
            document = status_command(2)
        else:
            document = load_document(args.json_file)

        await asyncio.sleep(0.1)
        await send_command(
            client,
            document,
            att_value_size,
            args.fragment_delay_ms / 1000.0,
        )
        await wait_for_sequence(queue, int(document["sequence_id"]), args.report_timeout)
        await asyncio.sleep(args.listen_seconds)
        while not queue.empty():
            print(json.dumps(queue.get_nowait(), ensure_ascii=False, indent=2))
        await client.stop_notify(REPORT_UUID)
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", help="Playback BLE 地址；省略时按设备名扫描")
    parser.add_argument("--scan-timeout", type=float, default=12.0)
    parser.add_argument("--connect-timeout", type=float, default=15.0)
    parser.add_argument("--report-timeout", type=float, default=8.0)
    parser.add_argument("--listen-seconds", type=float, default=1.0)
    parser.add_argument(
        "--fragment-delay-ms",
        type=float,
        default=5.0,
        help="每个 GATT 分片写入后的节流时间，默认 5 ms",
    )
    parser.add_argument(
        "--att-value-size",
        type=int,
        help="覆盖自动协商的 ATT value 大小，范围 17..244",
    )

    subparsers = parser.add_subparsers(dest="action", required=True)
    subparsers.add_parser("status", help="完成 HELLO 后读取 Playback 状态")
    send_parser = subparsers.add_parser("send", help="发送完整 Board Link 命令 JSON")
    send_parser.add_argument("json_file", type=Path)
    return parser.parse_args()


def main() -> int:
    try:
        return asyncio.run(run(parse_args()))
    except Exception as error:
        print(
            f"测试失败：{type(error).__name__}: {error!r}",
            file=sys.stderr,
        )
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
