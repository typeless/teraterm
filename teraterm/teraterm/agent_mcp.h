/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 */

/* Native MCP transport (Streamable HTTP) layered on the agent_jsonrpc dispatch
 * core, so an MCP host (e.g. Claude Code) can connect directly with no shim.
 *
 * Two layers, both pure C (cJSON only, no Win32 -> host-testable):
 *   - agent_mcp_handle: the MCP JSON-RPC layer (initialize / tools/list /
 *     tools/call / ping). tools map 1:1 to agent_call methods.
 *   - agent_mcp_feed / AgentMcpConn: minimal HTTP/1.1 framing over a stream,
 *     with bearer-token auth. POST carries one JSON-RPC message; GET -> 405. */

#pragma once

#include "agent_jsonrpc.h" /* AgentBackend, cJSON */

#ifdef __cplusplus
extern "C" {
#endif

/* Pure MCP JSON-RPC handler. Parses one JSON-RPC message in body[0..len) and
 * writes a single JSON response line into out (<= outcap, NUL-terminated).
 * Returns the response length, or 0 if the message was a notification (no
 * response is due). Exposed for host tests. */
size_t agent_mcp_handle(const AgentBackend *be, const char *body, size_t len, char *out, size_t outcap);

/* HTTP transport connection state. */
typedef struct AgentMcpConn AgentMcpConn;

/* token: required bearer token, "" for none. bind_host: the address the server
 * is bound to (accepted as a valid Host in addition to loopback), "" for none.
 * be must outlive the connection. */
AgentMcpConn *agent_mcp_conn_new(const AgentBackend *be, const char *token, const char *bind_host);
void agent_mcp_conn_free(AgentMcpConn *c);

/* Feed received bytes. If a complete HTTP request is buffered, writes its full
 * HTTP response into out (<= outcap) and returns the length (> 0). Returns 0 if
 * more data is needed. After a > 0 return, call again with data=NULL,len=0 to
 * drain any further buffered requests. Sets *close_after to 1 when the
 * connection should be closed once the response has been sent. */
size_t agent_mcp_feed(AgentMcpConn *c, const char *data, size_t len, char *out, size_t outcap,
					  int *close_after);

#ifdef __cplusplus
}
#endif
