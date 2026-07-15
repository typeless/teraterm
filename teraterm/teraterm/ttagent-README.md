# Tera Term Agent Control (Phase 1)

Lets an external agent read a connection's received stream and inject data into
the connection over a local TCP socket, using a line-delimited JSON-RPC
protocol. Built into `ttermpro.exe`. The shipped `TERATERM.INI` enables the MCP
listener (loopback, read-only, auth required); sending stays off until armed.

## Enabling

The `Control → Agent server` menu toggles the agent at runtime (check mark =
running); the title bar always shows the state: ` [agent:off]` / ` [agent]` /
` [agent:send]`. `Control → Agent send` arms/disarms injection within the
`AllowSend` grant. Configuration lives in the `[Agent]` section of
`TERATERM.INI`; the shipped default is:

```ini
[Agent]
Enable=on
McpPort=5334
Token=
AllowSend=off
```

All keys:

```ini
[Agent]
Enable=on
BindAddress=127.0.0.1
Port=5333
McpPort=5334
Token=
AllowSend=on
RingBytes=1048576
```

| Key | Default | Meaning |
|-----|---------|---------|
| `Enable` | `off` | Master switch. A listener starts only when `on` **and** the port is `>0`. |
| `BindAddress` | `127.0.0.1` | Interface to bind. Keep loopback; tunnel for remote access. |
| `Port` | `0` | Raw line-JSON TCP port (0 = disabled). |
| `McpPort` | `0` | Native MCP (Streamable HTTP) port for direct MCP hosts (0 = disabled). |
| `Token` | *(empty)* | Bearer token, required on every request (raw `hello` / MCP `Authorization`). Left blank, a random token is generated and saved here the first time the agent starts. `Token=none` disables auth explicitly (e.g. when access is tunneled through SSH and the machine is single-user) — allowed only with a loopback `BindAddress`; the agent refuses to start otherwise. |
| `AllowSend` | `off` | Permit injecting data into the connection. Read-only if `off`. |
| `RingBytes` | `1048576` | Scrollback capacity (min 65536). |

Set `Port`, `McpPort`, or both. `Port` speaks the raw line-JSON protocol below
(good for scripts / `agent_client.py`); `McpPort` speaks MCP over HTTP so an MCP
host such as Claude Code connects directly with no shim. Both share the same
received-stream ring and send-arming.

The listener binds loopback only by default. Injecting bytes into a live SSH or
serial session is equivalent to typing at the keyboard — treat the port like a
remote shell. Every request carries the `Token` (auto-generated if you don't set
one); read it out of `TERATERM.INI` to configure your client. Never bind a
public interface. The token's job is to keep *other local users* (shared or
Terminal Server machines) off the port — an SSH tunnel authenticates the remote
end, but any local process can reach loopback directly. On a single-user
machine `Token=none` is a reasonable trade; it is refused on a non-loopback
bind.

The MCP endpoint also rejects (403) any request whose `Host`/`Origin` header is
not loopback or the configured `BindAddress`, so a malicious web page cannot
reach it via DNS rebinding from a victim's browser.

**Local exposure of the received stream.** While the agent is enabled, each
window mirrors its received bytes into a named shared segment
(`TeraTermAgentShmemV1`) so one broker window can serve every session. The
segment carries an owner-only DACL, but a session-named object cannot be walled
off from *other processes running as the same Windows user* — any such process
can map it and read every session's received stream (decrypted SSH/serial
output), regardless of the network token, loopback binding, or send-arming. This
is the same trust boundary as the rest of your user session, but it is a
low-effort, always-on read channel: only enable the agent on machines where you
trust the code running under your account, and disable it (`Enable=off`) when not
in use.

## Remote access over SSH

The server binds Windows loopback (`127.0.0.1`), so it never touches the network
directly — an SSH tunnel carries the traffic. Two ways, both keeping the endpoint
on loopback:

### Local forwarding (`ssh -L`) — recommended when Windows runs an SSH server

The machine running the agent opens an SSH session *into* Windows and forwards a
local port to Tera Term's loopback listener:

```
# on the agent/Claude host; Windows runs OpenSSH Server (reachable over Tailnet)
ssh -L 5333:127.0.0.1:5333 user@windows-box
# the agent then connects to the local port the ssh client opened:
#   127.0.0.1:5333
```

The tunnel is independent of Tera Term's own session (survives reconnects) and
needs no Tera Term configuration. Requires an SSH server on the Windows box.

### Remote forwarding (`ssh -R`) — rides Tera Term's own SSH session

If Tera Term (the SSH client) is already connected to the same server the agent
runs on, remote-forward the port back through that session (ttssh supports it):

- Configure the Tera Term SSH session to remote-forward
  `remote-server:5333 -> 127.0.0.1:5333`.
- The agent connects to `127.0.0.1:5333` on that server.

Needs no Windows SSH server, but the tunnel dies when that Tera Term session ends.

(Alternative to both: bind a Tailscale IP and connect to `100.x.x.x:5333` over
WireGuard, protected by the token + Tailscale ACLs.)

## Connecting Claude Code (native MCP)

With `McpPort` set and the port tunnelled to loopback on the Claude host (see
above), register it as a remote MCP server:

```
claude mcp add --transport http teraterm http://127.0.0.1:5334/mcp \
  --header "Authorization: Bearer change-me-to-a-random-secret"
```

Claude then sees the tools `status`, `list_sessions`, `read_new_output`,
`read_scrollback`, `send_line`, `send_bytes`, `send_key`. The transport is
Streamable HTTP (POST for requests; no server-initiated stream — `GET` returns
405). To drive a full-screen wait, have the agent poll `read_new_output` with
`since = next` until the expected text appears.

## Raw protocol (the `Port` listener)

Newline-delimited JSON. One request object per line, one response line back.
Binary payloads are base64; text is UTF-8.

Request: `{"id":<any>,"method":"<name>","params":{...}}`
Response: `{"id":<same>,"ok":true,"result":{...}}` or `{"id":<same>,"ok":false,"error":"..."}`

| method | params | result |
|--------|--------|--------|
| `hello` | `token` | `{version,server}` — required first if a token is set |
| `list_sessions` | — | `[{session,host,title,port_type,ready}]` |
| `status` | `session?` | `{ready,port_type,host,offset,cols,rows}` |
| `read_new_output` | `since,max_bytes?` | `{from,next,data_b64,gap}` |
| `read_scrollback` | `max_bytes?` | `{from,next,data_b64}` |
| `send_line` | `text,newline?` | `{sent}` — default newline `\r`; needs `AllowSend` |
| `send_bytes` | `data_b64` | `{sent}` — raw; `0xFF` is the telnet IAC byte |
| `send_key` | `key` | `{sent}` — `enter,tab,esc,ctrl-c,up,down,left,right,...` |

`offset`/`from`/`next` are monotonic byte offsets into the received stream. Poll
`read_new_output` with `since = next` from the previous reply to stream output;
`gap:true` means the ring wrapped and some bytes between your `since` and `from`
were dropped.

### Multiple sessions

Every open Tera Term window is a separate session. One window (whichever wins
the port) acts as the broker and serves all of them through the single endpoint;
if it closes, another window takes over within ~1s. `list_sessions` returns one
entry per window with a `session` id (the window handle in hex), its `host`,
`title`, and `ready`. Pass that `session` in `status`/`read_*`/`send_*` to act on
a specific window; omit it (or pass empty) to act on the broker's own window.
Reads come straight from each window's shared-memory ring; sends are handed to
the target window, which applies them only if its own `AllowSend` is armed.

Blocking waits (`wait_for`) are intentionally not server-side — poll
`read_new_output` from the client so the terminal UI thread never stalls.

## Quick test

`tests/agent_client.py` is a minimal client:

```
python tests/agent_client.py --port 5333 --token change-me status
python tests/agent_client.py --port 5333 --token change-me send-line "echo HELLO_$$"
python tests/agent_client.py --port 5333 --token change-me read
```
