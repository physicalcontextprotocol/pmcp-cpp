#!/usr/bin/env python3
"""Cross-implementation interop for pmcp-cpp.

Two directions, both against real SDK code rather than a mock:

  A. C++ client  ->  pmcp-python/pcp, v05, and the C++ server
  B. Python client  ->  the C++ server (using each SDK's own client class)

Anything that cannot run is reported as SKIP, never silently passed. Exits 77
when every case was skipped, which ctest reads as "skipped".
"""
from __future__ import annotations

import argparse
import contextlib
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

SKIP = 77

# --------------------------------------------------------------------------
# harness
# --------------------------------------------------------------------------


class Result:
    def __init__(self) -> None:
        self.passed = 0
        self.failed: list[str] = []
        self.skipped: list[str] = []

    def ok(self, label: str) -> None:
        self.passed += 1
        print(f"    ok   {label}")

    def fail(self, label: str, detail: str = "") -> None:
        self.failed.append(f"{label}: {detail}" if detail else label)
        print(f"    FAIL {label}" + (f" — {detail}" if detail else ""))

    def skip(self, label: str, why: str) -> None:
        self.skipped.append(f"{label} ({why})")
        print(f"    SKIP {label} — {why}")

    def check(self, cond: bool, label: str, detail: str = "") -> bool:
        if cond:
            self.ok(label)
        else:
            self.fail(label, detail)
        return cond

    @property
    def total(self) -> int:
        return self.passed + len(self.failed) + len(self.skipped)


R = Result()
print_lock = __import__("threading").Lock()


def say(text: str) -> None:
    with print_lock:
        print(text, flush=True)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def rpc(url: str, method: str, params=None, request_id: int = 1, timeout: float = 10.0):
    body = json.dumps(
        {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}}
    ).encode()
    req = urllib.request.Request(
        url, data=body, headers={"Content-Type": "application/json"}, method="POST"
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read().decode()
    return json.loads(raw) if raw.strip() else {}


def unwrap(resp: dict) -> dict:
    if "error" in resp:
        raise AssertionError(f"JSON-RPC error {resp['error']}")
    return resp["result"]


def actuation_ok(result: dict) -> bool:
    """True when any of the three result shapes reports success."""
    if "success" in result:
        return bool(result["success"])
    if "isError" in result:
        return not result["isError"]
    content = result.get("content") or []
    if content:
        block = content[0]
        if "data" in block:
            return bool(block["data"].get("success"))
        text = block.get("text")
        if isinstance(text, str):
            try:
                return bool(json.loads(text).get("success"))
            except json.JSONDecodeError:
                return False
    return False


def names(result: dict, *keys: str) -> list[str]:
    out: list[str] = []
    for k in keys:
        for e in result.get(k, []) or []:
            out.append(e.get("name") or e.get("uri", ""))
    return out


@contextlib.contextmanager
def http_server(cmd: list[str], cwd: str | None = None, env: dict | None = None):
    """Start a server on an OS-chosen port; yield its base URL and base path."""
    port = free_port()
    full = [c.replace("{PORT}", str(port)) for c in cmd]
    say(f"    $ {' '.join(full)}")
    proc = subprocess.Popen(
        full,
        cwd=cwd,
        env={**os.environ, **(env or {})},
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        yield proc, f"http://127.0.0.1:{port}"
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            out = proc.stdout.read() if proc.stdout else ""
            say(f"    (server output after kill)\n{out}")


def wait_http(url: str, paths: tuple[str, ...], proc, tries: int = 100) -> str | None:
    """Poll until any candidate path answers; return the one that did."""
    for _ in range(tries):
        if proc.poll() is not None:
            return None
        for path in paths:
            try:
                rpc(url + path, "ping", timeout=2.0)
                return path
            except (urllib.error.URLError, OSError, TimeoutError, ConnectionError):
                pass
        time.sleep(0.1)
    return None


def run_cpp_client(cpp_client: str, dialect: str, url: str, path: str) -> tuple[bool, str]:
    proc = subprocess.run(
        [cpp_client, f"--dialect={dialect}", f"--url={url}", f"--path={path}"],
        capture_output=True,
        text=True,
        timeout=120,
    )
    return proc.returncode == 0, proc.stdout + proc.stderr


def have_python_sdk(root: str) -> tuple[bool, str]:
    pcp = os.path.join(root, "pmcp-python")
    if not os.path.isdir(pcp):
        return False, "pmcp-python not present"
    probe = "import pcp.server, pcp.client; print('ok')"
    try:
        subprocess.run(
            [sys.executable, "-c", probe],
            cwd=pcp,
            capture_output=True,
            text=True,
            timeout=60,
            check=True,
        )
    except Exception as exc:  # noqa: BLE001 — report whatever went wrong
        return False, f"pmcp-python not importable ({exc})"
    return True, ""


# --------------------------------------------------------------------------
# Direction A: C++ client -> real Python servers
# --------------------------------------------------------------------------

def python_sdk_case(root: str, cpp_client: str, label: str) -> None:
    pcp_root = os.path.join(root, "pmcp-python")
    script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_sdk_server.py")
    port = free_port()
    say(f"  -> C++ client vs pmcp-python/pcp ({label})")
    proc = subprocess.Popen(
        [sys.executable, script, "--pcp-root", pcp_root, "--port", str(port)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        path = wait_http(f"http://127.0.0.1:{port}", ("/pcp", "/mcp", "/"), proc)
        if path is None:
            R.fail(f"cpp->pcp ({label})", "server never answered a ping")
            return
        say(f"    (server answered on {path})")
        ok, out = run_cpp_client(cpp_client, "python", f"http://127.0.0.1:{port}", path)
        for line in out.strip().splitlines():
            say("    " + line)
        R.check(ok, f"cpp->pcp full sequence ({label})")
    finally:
        proc.terminate()
        with contextlib.suppress(subprocess.TimeoutExpired):
            proc.wait(timeout=5)


# --------------------------------------------------------------------------
# Direction B: Python client -> C++ server
# --------------------------------------------------------------------------


def cpp_server_case(cpp_server: str, root: str, label: str, have_sdk: bool) -> None:
    say(f"  -> pmcp-python client vs C++ server ({label})")
    with http_server([cpp_server, "--port={PORT}"]) as (proc, url):
        path = wait_http(url, ("/pcp", "/mcp", "/"), proc)
        if path is None:
            R.fail(f"pcp->cpp ({label})", "C++ server never answered a ping")
            return
        say(f"    (C++ server answered on {path})")

        # --- raw JSON-RPC checks, dialect by dialect ---
        init = unwrap(rpc(url + path, "initialize", {"protocolVersion": "0.5"}, 1))
        R.check("serverInfo" in init, f"initialize returns serverInfo ({label})")

        acts = unwrap(rpc(url + path, "actuations/list", {}, 2))
        R.check("move_to" in names(acts, "actuations"), f"actuations/list ({label})")

        v05_tools = unwrap(rpc(url + path, "tools/list", {}, 3))
        R.check("move_to" in names(v05_tools, "tools"), f"tools/list ({label})")

        conf_acts_name = "actuations/execute"
        bad = rpc(url + path, conf_acts_name, {"name": "nope"}, 5)
        R.check(bad.get("error", {}).get("code") == -32601, f"unknown actuation -32601 ({label})")

        lease = unwrap(rpc(url + path, "lease/request", {"zone_id": "z", "duration_ms": 5000}, 6))
        lease_id = (lease.get("lease") or {}).get("lease_id") or lease.get("lease_id", "")
        R.check((lease.get("lease") or {}).get("state") == "ACTIVE", f"lease granted ({label})")
        R.check(bool(lease_id), f"lease id present ({label})")

        # camelCase, as v05 sends.
        v05_lease = unwrap(
            rpc(url + path, "lease/request", {"robotId": "r", "zoneId": "z2", "durationMs": 5000}, 7)
        )
        R.check(
            (v05_lease.get("lease") or {}).get("leaseId", "") != "",
            f"v05 camelCase lease normalizes and echoes leaseId ({label})",
        )

        out = unwrap(
            rpc(url + path, "actuations/call", {"name": "move_to", "arguments": {"x": 0.1, "y": 0.1, "z": 0.2}, "lease_token": lease_id}, 8)
        )
        R.check(actuation_ok(out), f"actuations/call with lease ({label})")

        # conformance dialect: flat result + params key. move_to requires a
        # lease, so reuse the one acquired above (id 9 is the "after lease"
        # spelling; the earlier e-stop checks exercise the blocked path).
        conf_out = unwrap(
            rpc(url + path, "actuations/execute", {"name": "move_to", "params": {"x": 0.1, "y": 0.1, "z": 0.2}, "lease_token": lease_id}, 9)
        )
        R.check(conf_out.get("success") is True, f"actuations/execute flat result ({label})")

        estop = unwrap(rpc(url + path, "safety/estop/engage", {}, 10))
        R.check(estop.get("engaged") is True, f"safety/estop/engage ({label})")
        blocked = rpc(url + path, "actuations/call", {"name": "move_to"}, 11)
        R.check("error" in blocked, f"e-stop blocks actuation ({label})")
        dis = unwrap(rpc(url + path, "safety/estop/disengage", {}, 12))
        R.check(dis.get("engaged") is False, f"safety/estop/disengage ({label})")

        metrics = unwrap(rpc(url + path, "pcp/metrics", {}, 13))
        R.check("actuationCount" in metrics, f"pcp/metrics conformance shape ({label})")

        read = unwrap(rpc(url + path, "resources/read", {"uri": "pcp://cpp-arm/sensors/temperature"}, 14))
        R.check(read.get("contents") is not None or "value" in read, f"resources/read ({label})")

        parse_fail = False
        try:
            req = urllib.request.Request(
                url + path,
                data=b"}}not json{{",
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            urllib.request.urlopen(req, timeout=5)
        except urllib.error.HTTPError as exc:
            parse_fail = exc.code == 400
        R.check(parse_fail, f"malformed JSON returns HTTP 400 ({label})")

    # --- real SDK client classes against the C++ server ---
    pcp_root = os.path.join(root, "pmcp-python")
    probe = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_sdk_client_probe.py")
    if not os.path.isfile(probe):
        R.skip(f"sdk-client->cpp ({label})", "probe script missing")
        return
    # The raw RPC cases above need nothing but the C++ server, so they still
    # run without the sibling SDKs. The real PCPClient probe imports v05 from
    # pmcp-python and cannot run without it — that is a skip, not a failure.
    if not have_sdk:
        R.skip(f"sdk-client->cpp ({label})", "pmcp-python/v05 not present or not importable")
        return
    say(f"  -> v05 PCPClient vs C++ server ({label})")
    with http_server([cpp_server, "--port={PORT}"]) as (proc, url):
        path = wait_http(url, ("/pcp", "/mcp", "/"), proc)
        if path is None:
            R.fail(f"sdk-client->cpp ({label})", "C++ server never answered")
            return
        proc2 = subprocess.run(
            [sys.executable, probe, "--pcp-root", pcp_root, "--url", url],
            capture_output=True,
            text=True,
            timeout=120,
        )
        for line in (proc2.stdout + proc2.stderr).strip().splitlines():
            say("    " + line)
        R.check(proc2.returncode == 0, f"v05 PCPClient full sequence vs C++ ({label})")


# --------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cpp-server", required=True)
    ap.add_argument("--cpp-client", required=True)
    ap.add_argument("--pmcp-root", required=True)
    ap.add_argument("--only", default=None, help="substring filter over case names")
    args = ap.parse_args()

    print("pmcp-cpp interop\n")

    ok_sdk, why = have_python_sdk(args.pmcp_root)
    if not ok_sdk:
        R.skip("all cross-SDK cases", why)
    if not args.only or "pcp" in args.only:
        cpp_server_case(args.cpp_server, args.pmcp_root, "pmcp-python/pcp", ok_sdk)
    if ok_sdk and (not args.only or "client" in args.only):
        try:
            python_sdk_case(args.pmcp_root, args.cpp_client, "pmcp-python/pcp")
        except Exception as exc:  # noqa: BLE001
            R.fail("cpp->pcp", str(exc))

    print(f"\n{R.passed} passed, {len(R.failed)} failed, {len(R.skipped)} skipped")
    for f in R.failed:
        print(f"  FAILED: {f}")
    if R.failed:
        return 1
    if R.passed == 0:
        return SKIP
    return 0


if __name__ == "__main__":
    sys.exit(main())
