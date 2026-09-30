#!/usr/bin/env python3
"""TabEngine Automation CLI: a standard-library client for the loopback
JSON-RPC profile.

Connects to an instance started with the automation transport enabled:

    python te.py --endpoint http://127.0.0.1:53123 --token <token> info
    python te.py --descriptor build/automation-descriptor.json windows
    TABENGINE_TOKEN=<token> python te.py --endpoint ... screenshot 1 --out shot.png

Only the Python standard library is used; this tool is not a runtime
dependency of any product. All commands print human-readable summaries and
with --json the raw result object. Exit codes: 0 success, 1 RPC/transport
error, 2 usage error.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

PROTOCOL_PREFIX = "/automation/v1"


class AutomationError(Exception):
    def __init__(self, message: str, code: int | None = None, data=None):
        super().__init__(message)
        self.code = code
        self.data = data


class AutomationClient:
    """One JSON-RPC request per HTTP request; connection closes after each."""

    def __init__(self, endpoint: str, token: str, timeout_s: float = 30.0,
                 session: str | None = None):
        self.endpoint = endpoint.rstrip("/")
        self.token = token
        self.timeout_s = timeout_s
        self.session = session
        self._next_id = 1

    def _request(self, method: str, path: str, body: bytes | None,
                 content_type: str) -> tuple[int, bytes, dict]:
        request = urllib.request.Request(self.endpoint + path, data=body, method=method)
        request.add_header("Authorization", "Bearer " + self.token)
        if self.session:
            request.add_header("X-TabEngine-Session", self.session)
        if body is not None:
            request.add_header("Content-Type", content_type)
        try:
            with urllib.request.urlopen(request, timeout=self.timeout_s) as response:
                return response.status, response.read(), dict(response.headers)
        except urllib.error.HTTPError as error:
            payload = error.read()
            try:
                detail = json.loads(payload.decode("utf-8", "replace"))
                message = detail.get("error", payload.decode("utf-8", "replace"))
            except (ValueError, AttributeError):
                message = payload.decode("utf-8", "replace")
            raise AutomationError(f"HTTP {error.code}: {message}", code=error.code) from None
        except urllib.error.URLError as error:
            raise AutomationError(f"cannot reach {self.endpoint}: {error.reason}") from None

    def call(self, method: str, params: dict | None = None):
        payload = {"jsonrpc": "2.0", "id": f"cli-{self._next_id}",
                   "method": method, "params": params or {}}
        self._next_id += 1
        body = json.dumps(payload).encode("utf-8")
        status, raw, _ = self._request("POST", f"{PROTOCOL_PREFIX}/rpc", body,
                                       "application/json")
        envelope = json.loads(raw.decode("utf-8"))
        if "error" in envelope:
            error = envelope["error"]
            raise AutomationError(error.get("message", "RPC error"),
                                  code=error.get("code"), data=error.get("data"))
        return envelope.get("result")

    def schema(self) -> dict:
        _, raw, _ = self._request("GET", f"{PROTOCOL_PREFIX}/schema", None, "")
        return json.loads(raw.decode("utf-8"))

    def artifact(self, artifact_id: str) -> tuple[bytes, str]:
        status, raw, headers = self._request(
            "GET", f"{PROTOCOL_PREFIX}/artifacts/{artifact_id}", None, "")
        return raw, headers.get("Content-Type", "application/octet-stream")

    def session_begin(self) -> str:
        _, raw, _ = self._request("POST", f"{PROTOCOL_PREFIX}/session", None, "")
        return json.loads(raw.decode("utf-8"))["sessionId"]

    def session_end(self, session: str) -> None:
        saved, self.session = self.session, session
        try:
            self._request("DELETE", f"{PROTOCOL_PREFIX}/session", None, "")
        finally:
            self.session = saved

    def info(self) -> dict:
        return self.call("System.getInfo")


def _print(value, as_json: bool) -> None:
    if as_json:
        print(json.dumps(value, indent=2))
    else:
        print(json.dumps(value, indent=2))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--endpoint", help="e.g. http://127.0.0.1:53123")
    parser.add_argument("--token", help="bearer token (or TABENGINE_TOKEN)")
    parser.add_argument("--token-file", help="file containing the token")
    parser.add_argument("--descriptor",
                        help="connection descriptor JSON written by the host")
    parser.add_argument("--json", action="store_true", help="raw JSON output")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--session",
                        help="logical session id (see 'session begin'); keeps raw input "
                             "gestures on one connection identity")

    commands = parser.add_subparsers(dest="command", required=True)
    session_commands = commands.add_parser("session", help="begin/end a logical session")
    session_commands.add_argument("action", choices=("begin", "end"))
    session_commands.add_argument("session_id", nargs="?",
                                  help="session id to end (begin prints a new one)")

    commands.add_parser("info", help="instance identity and capabilities")
    commands.add_parser("schema", help="machine-readable method schema")
    commands.add_parser("windows", help="list windows")
    commands.add_parser("tabs", help="list tabs of a window").add_argument("window")
    snapshot = commands.add_parser("snapshot", help="semantic snapshot of a window")
    snapshot.add_argument("window")
    snapshot.add_argument("--max-nodes", type=int, default=0)
    click = commands.add_parser("click", help="click at a point or semantic node")
    click.add_argument("window")
    click.add_argument("--x", type=int)
    click.add_argument("--y", type=int)
    click.add_argument("--node")
    click.add_argument("--key", dest="stable_key")
    click.add_argument("--double", action="store_true")
    key = commands.add_parser("key", help="synthesize a key")
    key.add_argument("window")
    key.add_argument("name")
    key.add_argument("--kind", default="press", choices=("press", "down", "up"))
    key.add_argument("--ctrl", action="store_true")
    key.add_argument("--shift", action="store_true")
    key.add_argument("--alt", action="store_true")
    type_text = commands.add_parser("type", help="deliver text input")
    type_text.add_argument("window")
    type_text.add_argument("text")
    wheel = commands.add_parser("wheel", help="synthesize a wheel event")
    wheel.add_argument("window")
    wheel.add_argument("--x", type=int, default=0)
    wheel.add_argument("--y", type=int, default=0)
    wheel.add_argument("--delta-y", type=int, required=True)
    wheel.add_argument("--delta-x", type=int, default=0)
    drag = commands.add_parser("drag", help="drag between client points")
    drag.add_argument("window")
    drag.add_argument("--from-x", type=int, required=True)
    drag.add_argument("--from-y", type=int, required=True)
    drag.add_argument("--to-x", type=int, required=True)
    drag.add_argument("--to-y", type=int, required=True)
    shot = commands.add_parser("screenshot", help="capture a PNG artifact")
    shot.add_argument("window")
    shot.add_argument("--out", help="write the PNG here (default: info only)")
    shot.add_argument("--frame", choices=("current", "fresh"), default="current")
    shot.add_argument("--node")
    shot.add_argument("--region", help="x,y,width,height")
    wait = commands.add_parser("wait", help="wait for a typed condition")
    wait.add_argument("condition",
                      choices=("activityQuiet", "nodeAppears", "operationDone",
                               "animationSettled", "frameStable"))
    wait.add_argument("--window")
    wait.add_argument("--scope")
    wait.add_argument("--stable-key")
    wait.add_argument("--operation")
    wait.add_argument("--frames", type=int, default=1)
    wait.add_argument("--timeout", dest="timeout_s", type=float, default=30.0)
    op_get = commands.add_parser("op-get", help="read operation status")
    op_get.add_argument("operation")
    op_wait = commands.add_parser("op-wait", help="wait for an operation")
    op_wait.add_argument("operation")
    op_cancel = commands.add_parser("op-cancel", help="cancel an operation")
    op_cancel.add_argument("operation")
    logs = commands.add_parser("logs", help="read engine/application logs")
    logs.add_argument("--after", type=int, default=0)
    logs.add_argument("--source", choices=("engine", "app"))
    logs.add_argument("--max", type=int, default=100)
    events = commands.add_parser("events", help="poll observation events")
    events.add_argument("--since", type=int, default=0)
    events.add_argument("--window")
    trace_start = commands.add_parser("trace-start", help="start a trace session")
    trace_start.add_argument("--max-spans", type=int, default=512)
    commands.add_parser("trace-status", help="trace activity")
    commands.add_parser("trace-stop", help="stop the trace and print spans")
    call = commands.add_parser("call", help="raw method call with JSON params")
    call.add_argument("method")
    call.add_argument("params", nargs="?", default="{}")
    return parser


def resolve_connection(args) -> AutomationClient:
    endpoint = args.endpoint
    token = args.token
    if args.descriptor:
        with open(args.descriptor, "r", encoding="utf-8") as handle:
            descriptor = json.load(handle)
        endpoint = endpoint or descriptor.get("endpoint")
    if not token and args.token_file:
        with open(args.token_file, "r", encoding="utf-8") as handle:
            token = handle.read().strip()
    token = token or os.environ.get("TABENGINE_TOKEN")
    if not endpoint:
        raise SystemExit("provide --endpoint or --descriptor")
    if not token:
        raise SystemExit("provide --token, --token-file or TABENGINE_TOKEN")
    return AutomationClient(endpoint, token, timeout_s=args.timeout,
                            session=getattr(args, "session", None))


def run(args) -> int:
    client = resolve_connection(args)
    command = args.command
    if command == "session":
        if args.action == "begin":
            session_id = client.session_begin()
            _print({"sessionId": session_id,
                    "hint": "pass it as --session <id> on later commands"}, args.json)
        else:
            if not args.session_id:
                raise SystemExit("session end needs the session id")
            client.session_end(args.session_id)
            _print({"ended": args.session_id}, args.json)
    elif command == "info":
        _print(client.info(), args.json)
    elif command == "schema":
        _print(client.schema(), args.json)
    elif command == "windows":
        _print(client.call("Window.list"), args.json)
    elif command == "tabs":
        _print(client.call("Tab.list", {"windowId": str(args.window)}), args.json)
    elif command == "snapshot":
        params = {"windowId": str(args.window)}
        if args.max_nodes:
            params["maxNodes"] = args.max_nodes
        _print(client.call("UI.snapshot", params), args.json)
    elif command == "click":
        params = {"windowId": str(args.window)}
        if args.node:
            params["node"] = args.node
        elif args.stable_key:
            params["stableKey"] = args.stable_key
        elif args.x is not None and args.y is not None:
            params["x"] = args.x
            params["y"] = args.y
        else:
            raise SystemExit("click needs --x/--y, --node or --key")
        if args.double:
            params["count"] = 2
        _print(client.call("Input.click", params), args.json)
    elif command == "key":
        params = {"windowId": str(args.window), "key": args.name, "kind": args.kind}
        for flag in ("ctrl", "shift", "alt"):
            if getattr(args, flag):
                params[flag] = True
        _print(client.call("Input.key", params), args.json)
    elif command == "type":
        _print(client.call("Input.typeText",
                           {"windowId": str(args.window), "text": args.text}), args.json)
    elif command == "wheel":
        _print(client.call("Input.wheel", {
            "windowId": str(args.window), "x": args.x, "y": args.y,
            "deltaY": args.delta_y, "deltaX": args.delta_x}), args.json)
    elif command == "drag":
        _print(client.call("Input.drag", {
            "windowId": str(args.window),
            "from": {"x": args.from_x, "y": args.from_y},
            "to": {"x": args.to_x, "y": args.to_y}}), args.json)
    elif command == "screenshot":
        params = {"windowId": str(args.window), "frame": args.frame}
        if args.node:
            params["node"] = args.node
        if args.region:
            x, y, width, height = (int(part) for part in args.region.split(","))
            params["region"] = {"x": x, "y": y, "width": width, "height": height}
        result = client.call("Capture.screenshot", params)
        if args.out:
            payload, media_type = client.artifact(result["artifactId"])
            with open(args.out, "wb") as handle:
                handle.write(payload)
            result["savedTo"] = args.out
            result["savedBytes"] = len(payload)
            if payload[:8] != b"\x89PNG\r\n\x1a\n":
                raise AutomationError("artifact bytes are not a PNG")
        _print(result, args.json)
    elif command == "wait":
        params = {"condition": args.condition, "timeoutS": args.timeout_s}
        if args.window:
            params["windowId"] = str(args.window)
        if args.scope:
            params["scope"] = args.scope
        if args.stable_key:
            params["stableKey"] = args.stable_key
        if args.operation:
            params["operationId"] = args.operation
        if args.condition == "frameStable":
            params["frames"] = args.frames
        _print(client.call("Wait.condition", params), args.json)
    elif command == "op-get":
        _print(client.call("Operation.get", {"operationId": args.operation}), args.json)
    elif command == "op-wait":
        _print(client.call("Operation.wait", {"operationId": args.operation}), args.json)
    elif command == "op-cancel":
        _print(client.call("Operation.cancel", {"operationId": args.operation}), args.json)
    elif command == "logs":
        params = {"after": args.after, "max": args.max}
        if args.source:
            params["source"] = args.source
        _print(client.call("Log.read", params), args.json)
    elif command == "events":
        params = {"since": args.since}
        if args.window:
            params["windowId"] = str(args.window)
        _print(client.call("Event.poll", params), args.json)
    elif command == "trace-start":
        _print(client.call("Trace.start", {"maxSpans": args.max_spans}), args.json)
    elif command == "trace-status":
        _print(client.call("Trace.status"), args.json)
    elif command == "trace-stop":
        _print(client.call("Trace.stop"), args.json)
    elif command == "call":
        _print(client.call(args.method, json.loads(args.params)), args.json)
    return 0


def main() -> int:
    args = build_parser().parse_args()
    try:
        return run(args)
    except AutomationError as error:
        print(f"error: {error}", file=sys.stderr)
        if error.data is not None:
            print(json.dumps(error.data, indent=2), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
