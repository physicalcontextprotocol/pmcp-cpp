#!/usr/bin/env python3
"""A real pmcp-python/pcp server for the interop harness.

Same registrations as tests/interop/server_main.cpp so the C++ client sees an
identical catalog whichever side is serving. Everything protocol-shaped comes
from the real pcp.server.PCPServer — only the two actuators and one sensor are
ours.
"""
from __future__ import annotations

import argparse
import asyncio
import sys


def build(pcp_root: str):
    sys.path.insert(0, pcp_root)
    from pcp.server import PCPServer
    from pcp.types import ActuationResult, SensorReading

    server = PCPServer(name="py-interop-arm", version="1.0.0", robot_id="py-arm")

    @server.actuation("move_to", description="Move the TCP to an XYZ target")
    async def move_to(x: float, y: float, z: float):
        return ActuationResult(success=True, final_pose={"x": x, "y": y, "z": z})

    @server.actuation("open_gripper", description="Open the gripper", requires_lease=False,
                       shadow_required=False)
    async def open_gripper():
        return ActuationResult(success=True, output={"open": True})

    @server.sensor("temperature", description="Motor temperature", unit="C")
    async def temperature():
        return SensorReading(value=40.0, unit="C")

    return server


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pcp-root", required=True)
    ap.add_argument("--port", type=int, required=True)
    args = ap.parse_args()

    server = build(args.pcp_root)
    print(f"py-interop-arm: {len(server._actuations)} actuations, "
          f"{len(server._sensors)} sensors", flush=True)
    asyncio.run(server.run(transport="http", host="127.0.0.1", port=args.port))
    return 0


if __name__ == "__main__":
    sys.exit(main())
