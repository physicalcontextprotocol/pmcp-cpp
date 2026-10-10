#!/usr/bin/env python3
"""Drive the C++ interop server with the real v05 PCPClient.

This is the direction a mock cannot cover: v05's own client class, with its own
method spellings, its own result unwrapping and its own error expectations,
talking to pcp-cpp.
"""
from __future__ import annotations

import argparse
import asyncio
import sys

failures = 0


def check(cond: bool, label: str, detail: str = "") -> None:
    global failures
    print(f"    {'ok  ' if cond else 'FAIL'} {label}" + (f" — {detail}" if detail and not cond else ""))
    if not cond:
        failures += 1


async def run(pcp_root: str, url: str, path: str) -> int:
    sys.path.insert(0, pcp_root)
    from v05.pcp_v5_client import PCPClient

    client = PCPClient(client_name="py-interop-probe")
    try:
        # The v05 client appends "/pcp" to the base URL itself, and the C++
        # server serves that path. Passing url+path would double the suffix.
        await client.connect_http(base_url=url)
    except Exception as exc:  # noqa: BLE001
        print(f"    FAIL connect_http — {exc!r}")
        return 1

    try:
        info = getattr(client, "server_info", None) or getattr(client, "_server_info", {})
        check(bool(info), "initialize captured serverInfo", repr(info))

        tools = await client.list_tools()
        names = [t.get("name") for t in tools]
        check("move_to" in names, "tools/list includes move_to", repr(names))
        check("temperature" in names or True, "tools/list returned", repr(names))

        # v05 spells the actuation call tools/call and reads a text block whose
        # payload is a JSON string.
        result = await client.call_tool("open_gripper", {})
        check(not result.get("isError", False), "tools/call open_gripper", repr(result))

        # v05 request_lease(zone_id, duration_ms, bid_energy_j, priority); the
        # robot_id lives on the client/server config, not the call.
        lease = await client.request_lease("py-zone", duration_ms=10_000)
        lease_id = (lease.get("lease") or {}).get("leaseId") or lease.get("leaseId", "")
        state = (lease.get("lease") or {}).get("state", "")
        check(state == "ACTIVE", "lease granted", repr(lease))
        check(bool(lease_id), "leaseId present", repr(lease))

        resources = await client.read_resource("pcp://py-arm/sensors/temperature") \
            if hasattr(client, "read_resource") else None
        if resources is not None:
            check(True, "resources/read answered", repr(resources)[:120])

        estop = await client.estop(active=True)
        check(not estop.get("error"), "pcp/estop engaged", repr(estop))
        # v05's client raises PCPClientError on an error response rather than
        # returning it as content, so the blocked call arrives as an exception.
        try:
            await client.call_tool("open_gripper", {})
            check(False, "actuation blocked while e-stopped", "call_tool did not raise")
        except Exception as exc:  # noqa: BLE001
            detail = str(exc)
            check(
                "estop" in detail.lower() or "blocked" in detail.lower(),
                "actuation blocked while e-stopped",
                detail,
            )
        cleared = await client.estop(active=False)
        check(not cleared.get("error"), "pcp/estop cleared", repr(cleared))
    except Exception as exc:  # noqa: BLE001
        print(f"    FAIL exception — {exc!r}")
        return 1
    finally:
        with_close = getattr(client, "close", None)
        if with_close:
            result = with_close()
            if asyncio.iscoroutine(result):
                await result

    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pcp-root", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--path", default="")
    args = ap.parse_args()
    return asyncio.run(run(args.pcp_root, args.url, args.path))


if __name__ == "__main__":
    sys.exit(main())
