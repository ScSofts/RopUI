#!/usr/bin/env python3
import argparse
import asyncio
import os
import random
import string
from dataclasses import dataclass
from typing import Optional


def _rand_payload(n: int) -> bytes:
    alphabet = string.ascii_letters + string.digits
    s = "".join(random.choice(alphabet) for _ in range(n))
    return s.encode("ascii")

@dataclass
class Stats:
    ok: int = 0
    mismatch: int = 0
    connect_fail: int = 0
    read_fail: int = 0


async def _one_client(
    idx: int,
    host: str,
    port: int,
    payload_bytes: int,
    hold_ms: int,
    timeout_s: float,
    stats: Stats,
) -> None:
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port), timeout=timeout_s
        )
    except Exception as e:
        stats.connect_fail += 1
        return

    try:
        if payload_bytes > 0:
            payload = _rand_payload(payload_bytes)
            writer.write(payload)
            await writer.drain()
            try:
                echoed = await asyncio.wait_for(
                    reader.readexactly(len(payload)), timeout=timeout_s
                )
                if echoed == payload:
                    stats.ok += 1
                else:
                    stats.mismatch += 1
            except Exception:
                stats.read_fail += 1

        if hold_ms > 0:
            await asyncio.sleep(hold_ms / 1000.0)
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


async def main() -> None:
    parser = argparse.ArgumentParser(description="Burst-create many TCP connections quickly.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("-n", "--clients", type=int, default=100)
    parser.add_argument("--payload-bytes", type=int, default=16, help="0 to disable sending payload")
    parser.add_argument("--hold-ms", type=int, default=1000, help="how long to keep each connection open")
    parser.add_argument("--timeout-s", type=float, default=2.0)
    args = parser.parse_args()

    stats = Stats()

    tasks = [
        asyncio.create_task(
            _one_client(
                idx=i,
                host=args.host,
                port=args.port,
                payload_bytes=args.payload_bytes,
                hold_ms=args.hold_ms,
                timeout_s=args.timeout_s,
                stats=stats,
            )
        )
        for i in range(args.clients)
    ]
    await asyncio.gather(*tasks)
    print(
        f"ok={stats.ok} mismatch={stats.mismatch} connect_fail={stats.connect_fail} read_fail={stats.read_fail}"
    )


if __name__ == "__main__":
    if os.name == "nt":
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    asyncio.run(main())
