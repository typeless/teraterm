/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 */

/* Agent control: line-delimited JSON-RPC dispatch core.
 *
 * Pure logic layered on cJSON and a small AgentBackend vtable, so it is
 * unit-testable with a fake backend (no sockets, no PComVar). agent_server
 * supplies the real backend that reads the ring and calls CommBinaryOut /
 * CommTextOutW; the native MCP transport (Phase 3) reuses the same dispatch. */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_PROTOCOL_VERSION 1

/* send_text / send_bytes return >= 0 (bytes accepted) or one of: */
#define AGENT_ERR_NOTCONN   (-1)  /* no live connection (cv null / !Ready) */
#define AGENT_ERR_NOTALLOWED (-2) /* send not armed for this session */
#define AGENT_ERR_NOSESSION (-3)  /* unknown session id */

typedef struct {
	int ready;
	int port_type;   /* cv->PortType (IdTCPIP / IdSerial / ...) */
	uint64_t offset; /* ring total; next unread offset */
	int cols, rows;
	char host[256];
} AgentStatus;

typedef struct {
	char session[64];
	char host[256];
	char title[256];
	int port_type;
	int ready;
} AgentSessionInfo;

/* Backend the dispatch calls. ctx is opaque (pvar in the server, a fake in
 * tests). session is the caller-supplied id; NULL/"" means "the default/self". */
typedef struct {
	void *ctx;

	/* Non-zero if a token is required before any read/send. */
	int require_token;
	/* Returns non-zero if token matches. */
	int (*check_token)(void *ctx, const char *token);

	/* Fills out; returns 0 on success, AGENT_ERR_NOSESSION if unknown. */
	int (*get_status)(void *ctx, const char *session, AgentStatus *out);

	/* Enumerate sessions into out[0..max); returns count written. */
	int (*list_sessions)(void *ctx, AgentSessionInfo *out, int max);

	/* Copy up to max bytes with offset >= since into out; returns bytes copied,
	 * sets *from (offset of first byte) and *gap (1 if bytes were dropped).
	 * Returns AGENT_ERR_NOSESSION if session unknown. */
	int (*read_since)(void *ctx, const char *session, uint64_t since, void *out, size_t max,
					  uint64_t *from, int *gap);

	/* Copy up to max most-recent bytes into out; returns bytes copied, sets *from. */
	int (*read_scrollback)(void *ctx, const char *session, void *out, size_t max, uint64_t *from);

	/* Send UTF-8 text (dispatch already appended any newline). Returns bytes
	 * accepted or a negative AGENT_ERR_*. */
	int (*send_text)(void *ctx, const char *session, const char *utf8, size_t len);

	/* Send raw bytes. Returns bytes accepted or a negative AGENT_ERR_*. */
	int (*send_bytes)(void *ctx, const char *session, const void *data, size_t len);
} AgentBackend;

/* Per-connection state threaded across requests on one socket. */
typedef struct {
	int authed;       /* set once hello succeeds (or when no token required) */
	int should_close; /* dispatch sets this to ask the server to drop the client */
} AgentConn;

/* Dispatch one JSON request line. Writes a single JSON response line (no
 * trailing newline) into resp, returns its length, or 0 if nothing to send.
 * Never writes more than respcap-1 bytes; always NUL-terminates. */
size_t agent_dispatch(const AgentBackend *be, AgentConn *conn, const char *line, size_t len,
					  char *resp, size_t respcap);

/* Execute one method against the backend, independent of any transport
 * envelope (used by both agent_dispatch and the MCP tools/call handler).
 * On success returns a newly-allocated cJSON result (caller owns, cJSON_Delete);
 * on failure returns NULL and sets *err to a static message. params may be NULL. */
cJSON *agent_call(const AgentBackend *be, const char *method, const cJSON *params, const char **err);

/* Base64 helpers (used by the protocol and tests). */

/* Encode src[0..srclen) into dst as a NUL-terminated string. dst must hold at
 * least ((srclen + 2) / 3) * 4 + 1 bytes. Returns the string length. */
size_t agent_b64_encode(const void *src, size_t srclen, char *dst);

/* Decode a base64 string into dst (must hold at least (strlen(src)/4)*3 bytes).
 * Returns bytes written, or (size_t)-1 on invalid input. */
size_t agent_b64_decode(const char *src, void *dst);

/* Compare two NUL-terminated strings for equality without short-circuiting on
 * the first differing byte, so a bearer-token check leaks no content timing.
 * Returns 1 if equal, 0 otherwise. (Loop length still reflects the shorter
 * string, so token length is not fully hidden -- content is.) */
int agent_ct_eq(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
