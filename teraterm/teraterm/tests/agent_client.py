#!/usr/bin/env python3
"""Minimal manual test client for the Tera Term agent control socket.

Speaks the line-delimited JSON-RPC protocol (see ttagent-README.md). Intended
for hand-verifying the server end-to-end on Windows, not as the MCP server.

Examples:
    python agent_client.py --port 5333 --token secret status
    python agent_client.py --port 5333 --token secret list
    python agent_client.py --port 5333 --token secret send-line "echo hi"
    python agent_client.py --port 5333 --token secret send-key ctrl-c
    python agent_client.py --port 5333 --token secret read          # from offset 0
    python agent_client.py --port 5333 --token secret follow        # stream new output
"""

import argparse
import base64
import json
import socket
import sys
import time


class Client:
    def __init__(self, host, port, token):
        self.sock = socket.create_connection((host, port))
        self.f = self.sock.makefile("rwb")
        self._id = 0
        if token:
            r = self.call("hello", token=token)
            if not r.get("ok"):
                raise SystemExit("hello failed: %s" % r.get("error"))

    def call(self, method, **params):
        self._id += 1
        req = {"id": self._id, "method": method}
        if params:
            req["params"] = params
        self.f.write((json.dumps(req) + "\n").encode("utf-8"))
        self.f.flush()
        line = self.f.readline()
        if not line:
            raise SystemExit("connection closed")
        return json.loads(line)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--token", default="")
    ap.add_argument("cmd")
    ap.add_argument("arg", nargs="?", default="")
    a = ap.parse_args()

    c = Client(a.host, a.port, a.token)

    if a.cmd == "status":
        print(json.dumps(c.call("status").get("result"), indent=2))
    elif a.cmd == "list":
        print(json.dumps(c.call("list_sessions").get("result"), indent=2))
    elif a.cmd == "send-line":
        print(c.call("send_line", text=a.arg))
    elif a.cmd == "send-key":
        print(c.call("send_key", key=a.arg))
    elif a.cmd == "read":
        r = c.call("read_new_output", since=0).get("result", {})
        sys.stdout.write(base64.b64decode(r.get("data_b64", "")).decode("utf-8", "replace"))
    elif a.cmd == "follow":
        since = c.call("status").get("result", {}).get("offset", 0)
        while True:
            r = c.call("read_new_output", since=since).get("result", {})
            data = base64.b64decode(r.get("data_b64", ""))
            if data:
                sys.stdout.write(data.decode("utf-8", "replace"))
                sys.stdout.flush()
                since = r.get("next", since)
            else:
                time.sleep(0.2)
    else:
        raise SystemExit("unknown command: %s" % a.cmd)


if __name__ == "__main__":
    main()
