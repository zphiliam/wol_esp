#!/usr/bin/env python3
"""
产线身份采集工具：串口连接 ESP32-C3，发送 `id` 命令，解析机读单行
`[ID] model=... mqtt_id=... psk=...`，追加到 CSV，便于后续生成二维码贴标。

用法：
    # 自动识别 Espressif 设备（默认）
    python collect_id.py
    python collect_id.py --loop          # 产线连续采集，拔插自动推进

    # 显式指定端口
    python collect_id.py --port /dev/cu.usbmodem101

`id` 命令在正常运行 / BLE 配网 / 串口 CLI 三种模式下均可用，无需切模式。
"""
import argparse
import csv
import os
import re
import sys
import time
from datetime import datetime
from pathlib import Path

import serial  # pyserial
import serial.tools.list_ports as list_ports

BAUD = 115200
BOOT_DRAIN_SECONDS = 2.0      # 打开串口后等待 USB-CDC reset 完成的时间
ESP_VID = 0x303A              # Espressif 默认 USB VID（ESP32-C3 native USB-CDC）
ID_LINE_RE = re.compile(
    r"^\[ID\]\s+model=(\S+)\s+mqtt_id=(\S+)\s+psk=([0-9a-fA-F]+)\s*$"
)
QR_URL_TEMPLATE = "https://i.iot-c.top/p?m={model}&v=1&id={mqtt_id}&psk={psk}"
CSV_COLUMNS = ["timestamp", "model", "mqtt_id", "psk", "qr_url", "port"]


def find_esp_devices(vid: int = ESP_VID) -> list:
    """返回所有匹配 VID 的串口（pyserial ListPortInfo 列表）。"""
    return [p for p in list_ports.comports() if p.vid == vid]


def wait_for_device(vid: int = ESP_VID, poll: float = 0.4):
    """阻塞直到恰好一台匹配设备出现；多台则报错（产线场景应单插槽）。"""
    while True:
        devs = find_esp_devices(vid)
        if len(devs) == 1:
            time.sleep(0.3)  # 给 OS 创建设备节点的余地
            return devs[0]
        if len(devs) > 1:
            ports = ", ".join(d.device for d in devs)
            raise RuntimeError(f"检测到多台设备：{ports}；产线请单槽插，或用 --port 指定")
        time.sleep(poll)


def wait_for_disconnect(vid: int = ESP_VID, poll: float = 0.4) -> None:
    """阻塞直到没有匹配设备。"""
    while find_esp_devices(vid):
        time.sleep(poll)


def read_existing_ids(csv_path: Path) -> set[str]:
    if not csv_path.exists():
        return set()
    seen: set[str] = set()
    with csv_path.open(newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            mid = row.get("mqtt_id", "").strip()
            if mid:
                seen.add(mid)
    return seen


def append_row(csv_path: Path, row: dict) -> None:
    new_file = not csv_path.exists()
    with csv_path.open("a", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_COLUMNS)
        if new_file:
            writer.writeheader()
        writer.writerow(row)


def capture_one(port: str, timeout: float) -> tuple[str, str, str]:
    """打开串口、发 id、等 [ID] 行；返回 (model, mqtt_id, psk)。"""
    with serial.Serial(port, BAUD, timeout=0.2) as s:
        # 等待 USB-CDC reset / setup banner 完成，顺手清空缓冲
        time.sleep(BOOT_DRAIN_SECONDS)
        s.reset_input_buffer()

        deadline = time.monotonic() + timeout
        # 双发兜底：第一次可能落在 banner 输出过程中被吃掉
        s.write(b"id\n")
        s.flush()
        last_resend = time.monotonic()

        buf = b""
        while time.monotonic() < deadline:
            chunk = s.read(256)
            if chunk:
                buf += chunk
                # 按行处理：保留未完整的尾巴
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode("utf-8", errors="replace").rstrip("\r")
                    m = ID_LINE_RE.match(text)
                    if m:
                        model, mqtt_id, psk = m.group(1), m.group(2), m.group(3).lower()
                        return model, mqtt_id, psk
            # 每 2s 再发一次，应对设备刚 boot / 在 BLE 模式忙
            if time.monotonic() - last_resend > 2.0:
                s.write(b"id\n")
                s.flush()
                last_resend = time.monotonic()

    raise TimeoutError(f"未在 {timeout:.0f}s 内收到 [ID] 机读行")


def collect(port: str, csv_path: Path, timeout: float, emit_url: bool) -> bool:
    """采集一次并写 CSV；返回 True 表示成功。"""
    try:
        model, mqtt_id, psk = capture_one(port, timeout)
    except serial.SerialException as e:
        print(f"  [错误] 串口异常：{e}", file=sys.stderr)
        return False
    except TimeoutError as e:
        print(f"  [错误] {e}", file=sys.stderr)
        return False

    qr_url = QR_URL_TEMPLATE.format(model=model, mqtt_id=mqtt_id, psk=psk) if emit_url else ""

    seen = read_existing_ids(csv_path)
    is_dup = mqtt_id in seen

    row = {
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "model": model,
        "mqtt_id": mqtt_id,
        "psk": psk,
        "qr_url": qr_url,
        "port": port,
    }
    append_row(csv_path, row)

    tag = "[重复]" if is_dup else "[新增]"
    print(f"  {tag} model={model} mqtt_id={mqtt_id} psk={psk}")
    if qr_url:
        print(f"        {qr_url}")
    if is_dup:
        print("  ↑ 该 mqtt_id 已在 CSV 中出现过；如重新贴标请删除旧行")
    return True


def resolve_port(explicit: str | None) -> str:
    """显式给了就用；否则按 Espressif VID 自动识别。"""
    if explicit:
        return explicit
    devs = find_esp_devices()
    if len(devs) == 0:
        print(f"未检测到 Espressif 设备（VID=0x{ESP_VID:04x}）。请插入或用 --port 指定。", file=sys.stderr)
        sys.exit(2)
    if len(devs) > 1:
        print("检测到多台 Espressif 设备：", file=sys.stderr)
        for d in devs:
            print(f"  {d.device}  SER={d.serial_number}", file=sys.stderr)
        print("请用 --port 指定其中一台。", file=sys.stderr)
        sys.exit(2)
    print(f"自动识别：{devs[0].device}  SER={devs[0].serial_number}")
    return devs[0].device


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=None,
                    help="串口设备路径（默认按 VID=0x303A 自动识别）")
    ap.add_argument(
        "--csv",
        default=str(Path(__file__).resolve().parent / "devices.csv"),
        help="CSV 输出路径（默认与脚本同目录 devices.csv）",
    )
    ap.add_argument("--timeout", type=float, default=8.0, help="等待 [ID] 行的超时秒数（默认 8）")
    ap.add_argument("--loop", action="store_true",
                    help="产线模式：采完一台后等待拔出 → 等待下一台插入，自动推进")
    ap.add_argument("--no-url", action="store_true", help="不在 CSV 中预生成二维码 URL")
    args = ap.parse_args()

    csv_path = Path(args.csv)
    csv_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"CSV: {csv_path}")

    # ── 单台模式 ──
    if not args.loop:
        port = resolve_port(args.port)
        print(f"端口: {port}  波特率: {BAUD}")
        ok = collect(port, csv_path, args.timeout, not args.no_url)
        return 0 if ok else 1

    # ── 产线 loop：自动识别 + 拔插推进 ──
    if args.port:
        # 显式指定端口时，仍按拔插循环；只不过端口名固定
        fixed_port = args.port
        get_port = lambda: fixed_port
    else:
        get_port = lambda: None  # 每轮重新探测

    count = 0
    try:
        while True:
            print(f"\n--- 设备 #{count + 1} ---")
            if args.port is None:
                # 自动模式：等待设备插入（若已插入则立即返回）
                if not find_esp_devices():
                    print("  请插入设备...")
                dev = wait_for_device()
                port = dev.device
                print(f"  检测到：{port}  SER={dev.serial_number}")
            else:
                port = get_port()
                print(f"  端口：{port}")

            ok = collect(port, csv_path, args.timeout, not args.no_url)
            if ok:
                count += 1

            print("  请拔出设备...")
            wait_for_disconnect()
    except KeyboardInterrupt:
        print(f"\n结束，共采集 {count} 台。")
        return 0
    except RuntimeError as e:
        print(f"\n[错误] {e}", file=sys.stderr)
        print(f"已采集 {count} 台。", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
