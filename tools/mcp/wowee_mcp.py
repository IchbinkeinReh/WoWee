#!/usr/bin/env python3
"""Talk to the MCP server of a running WoWee client (docs/MCP_SERVER.md).

The client has to have been started with --mcp. Where it listens and the token
to show it are read from mcp_server.json in the client's config root
(%APPDATA%\\wowee, ~/.wowee, $WOWEE_CONFIG_ROOT, or --file); the token is never
printed.

    wowee_mcp.py list [PART]                 the tools, or those with PART in them
    wowee_mcp.py call TOOL [JSON]            call one; arguments as JSON, or k=v ...
    wowee_mcp.py loop                        calls from standard input, a line each:
                                             TOOL JSON
    wowee_mcp.py config                      the `claude mcp add` line for this client
                                             (its token is left for you to fill in)

Pictures are saved beside --out (shot.jpg by default, numbered; with --overwrite
the last one is written over), and the path
is printed in their place.

Standard library only.
"""

import argparse
import base64
import http.client
import json
import os
import pathlib
import sys
import time


def config_root():
    root = os.environ.get("WOWEE_CONFIG_ROOT")
    if root:
        return pathlib.Path(root)
    if sys.platform == "win32":
        return pathlib.Path(os.environ.get("APPDATA", ".")) / "wowee"
    return pathlib.Path.home() / ".wowee"


class McpError(Exception):
    pass


class Client:
    """One connection, kept open; made again when the client was restarted."""

    def __init__(self, path=None, timeout=320.0):
        self.path = pathlib.Path(path) if path else config_root() / "mcp_server.json"
        self.timeout = timeout
        self._conn = None
        self._id = 0
        self._load()

    def _load(self):
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except OSError as e:
            raise McpError(f"no server: {self.path} cannot be read ({e.strerror}); was the client started with --mcp?")
        self.port = int(data["port"])
        self.pid = data.get("pid")
        self._token = data["token"]

    def _post(self, body):
        payload = json.dumps(body).encode("utf-8")
        headers = {
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
            "Authorization": "Bearer " + self._token,
        }
        for attempt in (0, 1):
            if self._conn is None:
                self._conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=self.timeout)
            try:
                self._conn.request("POST", "/mcp", payload, headers)
                response = self._conn.getresponse()
                data = response.read()
                return response.status, data
            except (http.client.HTTPException, ConnectionError, OSError) as e:
                self.close()
                if attempt:
                    raise McpError(f"no answer from 127.0.0.1:{self.port}: {e}")
        raise McpError("unreachable")

    def close(self):
        if self._conn is not None:
            self._conn.close()
            self._conn = None

    def request(self, method, params=None):
        self._id += 1
        body = {"jsonrpc": "2.0", "id": self._id, "method": method}
        if params is not None:
            body["params"] = params
        status, data = self._post(body)
        if status != 200:
            raise McpError(f"HTTP {status}: {data.decode('utf-8', 'replace').strip()}")
        answer = json.loads(data)
        if "error" in answer:
            raise McpError(answer["error"].get("message", "error"))
        return answer["result"]

    def notify(self, method):
        self._post({"jsonrpc": "2.0", "method": method})

    def initialize(self):
        result = self.request("initialize", {
            "protocolVersion": "2025-06-18",
            "capabilities": {},
            "clientInfo": {"name": "wowee_mcp.py", "version": "1"},
        })
        self.notify("notifications/initialized")
        return result

    def tools(self):
        return self.request("tools/list")["tools"]

    def call(self, name, arguments=None):
        """(value, failed, pictures): the text parsed where it is JSON."""
        result = self.request("tools/call", {"name": name, "arguments": arguments or {}})
        value = None
        pictures = []
        for part in result.get("content", []):
            if part.get("type") == "text":
                try:
                    value = json.loads(part["text"])
                except ValueError:
                    value = part["text"]
            elif part.get("type") == "image":
                pictures.append((part["mimeType"], base64.b64decode(part["data"])))
        return value, bool(result.get("isError")), pictures


def save_pictures(pictures, out, overwrite=False):
    saved = []
    for mime, data in pictures:
        path = pathlib.Path(out)
        suffix = ".png" if mime.endswith("png") else ".jpg"
        path = path.with_suffix(suffix)
        n = 1
        # The pictures of one call never over one another.
        while str(path) in saved or (path.exists() and not overwrite):
            path = path.with_name(f"{pathlib.Path(out).stem}_{n}{suffix}")
            n += 1
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        saved.append(str(path))
    return saved


def parse_arguments(words, schema=None):
    """One JSON object, or k=v words: text where the tool takes text (ui
    filter=25), else JSON where it parses."""
    properties = (schema or {}).get("properties", {})
    if not words:
        return {}
    text = " ".join(words).strip()
    if text.startswith("{"):
        return json.loads(text)
    out = {}
    for word in words:
        key, sep, value = word.partition("=")
        if not sep:
            raise McpError(f"not key=value: {word}")
        if properties.get(key, {}).get("type") == "string":
            out[key] = value
            continue
        try:
            out[key] = json.loads(value)
        except ValueError:
            out[key] = value
    return out


def schema_of(client, name):
    """The tool's own, for the words that are to be read by it."""
    for tool in client.tools():
        if tool["name"] == name:
            return tool["inputSchema"]
    return None


def show(value, failed, pictures, args):
    for path in save_pictures(pictures, args.out, args.overwrite):
        print(f"[picture] {path}")
    if value is not None:
        text = value if isinstance(value, str) else json.dumps(value, ensure_ascii=False,
                                                               indent=None if args.compact else 1)
        print(("FAILED: " if failed else "") + text)
    sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--file", help="mcp_server.json of the client")
    parser.add_argument("--out", default="shot.jpg", help="where pictures are saved")
    parser.add_argument("--overwrite", action="store_true", help="the picture over the last one at --out")
    parser.add_argument("--compact", action="store_true", help="JSON on one line")
    parser.add_argument("--wait", type=float, default=0.0, help="seconds to wait for the server to be there")
    parser.add_argument("command", choices=["list", "call", "loop", "config"])
    parser.add_argument("words", nargs="*")
    args = parser.parse_args()
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")

    until = time.monotonic() + args.wait
    while True:
        try:
            client = Client(args.file)
            client.initialize()
            break
        except McpError as e:
            if time.monotonic() >= until:
                print(e, file=sys.stderr)
                return 2
            time.sleep(0.5)

    try:
        if args.command == "config":
            print(f"claude mcp add --transport http wowee http://127.0.0.1:{client.port}/mcp "
                  f"--header \"Authorization: Bearer <token in {client.path}>\"")
        elif args.command == "list":
            for tool in client.tools():
                if args.words and not any(w.lower() in json.dumps(tool).lower() for w in args.words):
                    continue
                print(f"{tool['name']}: {tool['description']}")
                wanted = tool["inputSchema"].get("required", [])
                for name, schema in tool["inputSchema"].get("properties", {}).items():
                    kind = "|".join(schema["enum"]) if "enum" in schema else schema.get("type", "")
                    mark = "*" if name in wanted else " "
                    print(f"   {mark}{name} ({kind}): {schema.get('description', '')}")
        elif args.command == "call":
            if not args.words:
                parser.error("call TOOL [JSON]")
            plain = any("=" in w for w in args.words[1:]) and not " ".join(args.words[1:]).strip().startswith("{")
            schema = schema_of(client, args.words[0]) if plain else None
            value, failed, pictures = client.call(args.words[0], parse_arguments(args.words[1:], schema))
            show(value, failed, pictures, args)
            return 1 if failed else 0
        else:
            for line in sys.stdin:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                name, _, rest = line.partition(" ")
                try:
                    value, failed, pictures = client.call(name, json.loads(rest) if rest.strip() else {})
                    show(value, failed, pictures, args)
                except (McpError, ValueError) as e:
                    print(f"ERROR: {e}")
                    sys.stdout.flush()
    except (McpError, ValueError) as e:
        print(e, file=sys.stderr)
        return 2
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
