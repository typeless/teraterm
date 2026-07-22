/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 */

#include "agent_mcp.h"
#include "cJSON.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MCP_DEFAULT_PROTOCOL "2025-06-18"
#define MCP_MAX_HEADER (32 * 1024)
#define MCP_MAX_BODY   (8 * 1024 * 1024)

static int str_ieq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		char x = *a, y = *b;
		if (x >= 'A' && x <= 'Z')
			x = (char)(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z')
			y = (char)(y - 'A' + 'a');
		if (x != y)
			return 0;
	}
	return *a == 0 && *b == 0;
}

/* ---- MCP JSON-RPC layer ---- */

typedef struct {
	const char *name;
	const char *desc;
	const char *schema; /* JSON Schema for inputSchema, as a JSON string */
} McpTool;

static const McpTool TOOLS[] = {
	{"status", "Connection status for a session (ready, host, byte offset, size).",
	 "{\"type\":\"object\",\"properties\":{\"session\":{\"type\":\"string\"}}}"},
	{"list_sessions", "List open Tera Term sessions (id, host, title).",
	 "{\"type\":\"object\",\"properties\":{}}"},
	{"read_new_output",
	 "Read received bytes since a byte offset. Returns base64 data plus the next offset; poll with next as since.",
	 "{\"type\":\"object\",\"properties\":{\"session\":{\"type\":\"string\"},"
	 "\"since\":{\"type\":\"integer\"},\"max_bytes\":{\"type\":\"integer\"}}}"},
	{"read_scrollback", "Read the most recent received bytes (base64).",
	 "{\"type\":\"object\",\"properties\":{\"session\":{\"type\":\"string\"},\"max_bytes\":{\"type\":\"integer\"}}}"},
	{"send_line", "Send a line of text to the connection (a newline is appended; default CR).",
	 "{\"type\":\"object\",\"properties\":{\"session\":{\"type\":\"string\"},"
	 "\"text\":{\"type\":\"string\"},\"newline\":{\"type\":\"string\"}},\"required\":[\"text\"]}"},
	{"send_bytes", "Send raw bytes (base64) to the connection.",
	 "{\"type\":\"object\",\"properties\":{\"session\":{\"type\":\"string\"},"
	 "\"data_b64\":{\"type\":\"string\"}},\"required\":[\"data_b64\"]}"},
	{"send_key", "Send a named key (enter, tab, esc, ctrl-c, up, down, left, right, ...).",
	 "{\"type\":\"object\",\"properties\":{\"session\":{\"type\":\"string\"},"
	 "\"key\":{\"type\":\"string\"}},\"required\":[\"key\"]}"},
	{"zmodem_send",
	 "Start a ZMODEM send of a local file over the connection (the peer must be "
	 "running a ZMODEM receiver, e.g. 'rz'). Async: returns once started; poll "
	 "status.transfer for state (active -> done) and ok.",
	 "{\"type\":\"object\",\"properties\":{\"session\":{\"type\":\"string\"},"
	 "\"path\":{\"type\":\"string\"},\"binary\":{\"type\":\"boolean\"}},\"required\":[\"path\"]}"},
};

static cJSON *build_tools_array(void)
{
	cJSON *arr = cJSON_CreateArray();
	for (size_t i = 0; i < sizeof(TOOLS) / sizeof(TOOLS[0]); i++) {
		cJSON *t = cJSON_CreateObject();
		cJSON_AddStringToObject(t, "name", TOOLS[i].name);
		cJSON_AddStringToObject(t, "description", TOOLS[i].desc);
		cJSON *schema = cJSON_Parse(TOOLS[i].schema);
		cJSON_AddItemToObject(t, "inputSchema", schema != NULL ? schema : cJSON_CreateObject());
		cJSON_AddItemToArray(arr, t);
	}
	return arr;
}

/* Serialize resp and copy into out; free resp. Returns length. */
static size_t emit(cJSON *resp, char *out, size_t cap)
{
	char *s = cJSON_PrintUnformatted(resp);
	cJSON_Delete(resp);
	if (s == NULL) {
		out[0] = 0;
		return 0;
	}
	size_t n = strlen(s);
	if (n >= cap)
		n = cap - 1;
	memcpy(out, s, n);
	out[n] = 0;
	cJSON_free(s);
	return n;
}

static cJSON *envelope(const cJSON *id)
{
	cJSON *r = cJSON_CreateObject();
	cJSON_AddStringToObject(r, "jsonrpc", "2.0");
	if (id != NULL)
		cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, 1));
	else
		cJSON_AddNullToObject(r, "id");
	return r;
}

static size_t emit_rpc_error(const cJSON *id, int code, const char *msg, char *out, size_t cap)
{
	cJSON *r = envelope(id);
	cJSON *e = cJSON_CreateObject();
	cJSON_AddNumberToObject(e, "code", code);
	cJSON_AddStringToObject(e, "message", msg);
	cJSON_AddItemToObject(r, "error", e);
	return emit(r, out, cap);
}

static size_t emit_rpc_result(const cJSON *id, cJSON *result, char *out, size_t cap)
{
	cJSON *r = envelope(id);
	cJSON_AddItemToObject(r, "result", result);
	return emit(r, out, cap);
}

static size_t handle_initialize(const cJSON *id, const cJSON *params, char *out, size_t cap)
{
	const cJSON *pv = cJSON_GetObjectItem(params, "protocolVersion");
	const char *ver = cJSON_IsString(pv) ? pv->valuestring : MCP_DEFAULT_PROTOCOL;

	cJSON *result = cJSON_CreateObject();
	cJSON_AddStringToObject(result, "protocolVersion", ver);
	cJSON *caps = cJSON_CreateObject();
	cJSON *tools = cJSON_CreateObject();
	cJSON_AddBoolToObject(tools, "listChanged", 0);
	cJSON_AddItemToObject(caps, "tools", tools);
	cJSON_AddItemToObject(result, "capabilities", caps);
	cJSON *info = cJSON_CreateObject();
	cJSON_AddStringToObject(info, "name", "teraterm-agent");
	cJSON_AddStringToObject(info, "version", "1.0");
	cJSON_AddItemToObject(result, "serverInfo", info);
	return emit_rpc_result(id, result, out, cap);
}

static size_t handle_tools_call(const AgentBackend *be, const cJSON *id, const cJSON *params, char *out,
								size_t cap)
{
	const cJSON *nj = cJSON_GetObjectItem(params, "name");
	if (!cJSON_IsString(nj))
		return emit_rpc_error(id, -32602, "missing tool name", out, cap);
	const cJSON *args = cJSON_GetObjectItem(params, "arguments");

	const char *err = NULL;
	cJSON *inner = agent_call(be, nj->valuestring, args, &err);

	cJSON *result = cJSON_CreateObject();
	cJSON *content = cJSON_CreateArray();
	cJSON *block = cJSON_CreateObject();
	cJSON_AddStringToObject(block, "type", "text");

	if (inner != NULL) {
		char *text = cJSON_PrintUnformatted(inner);
		cJSON_AddStringToObject(block, "text", text != NULL ? text : "{}");
		if (text != NULL)
			cJSON_free(text);
		cJSON_AddItemToArray(content, block);
		cJSON_AddItemToObject(result, "content", content);
		cJSON_AddItemToObject(result, "structuredContent", inner);
		cJSON_AddBoolToObject(result, "isError", 0);
	}
	else {
		cJSON_AddStringToObject(block, "text", err != NULL ? err : "error");
		cJSON_AddItemToArray(content, block);
		cJSON_AddItemToObject(result, "content", content);
		cJSON_AddBoolToObject(result, "isError", 1);
	}
	return emit_rpc_result(id, result, out, cap);
}

size_t agent_mcp_handle(const AgentBackend *be, const char *body, size_t len, char *out, size_t outcap)
{
	cJSON *req = cJSON_ParseWithLength(body, len);
	if (req == NULL)
		return emit_rpc_error(NULL, -32700, "parse error", out, outcap);

	const cJSON *mj = cJSON_GetObjectItem(req, "method");
	const cJSON *id = cJSON_GetObjectItem(req, "id");
	const cJSON *params = cJSON_GetObjectItem(req, "params");

	if (!cJSON_IsString(mj)) {
		size_t n = emit_rpc_error(id, -32600, "invalid request", out, outcap);
		cJSON_Delete(req);
		return n;
	}
	const char *method = mj->valuestring;

	/* Notifications carry no id and expect no response. */
	if (id == NULL || strncmp(method, "notifications/", 14) == 0) {
		cJSON_Delete(req);
		return 0;
	}

	size_t n;
	if (strcmp(method, "initialize") == 0) {
		n = handle_initialize(id, params, out, outcap);
	}
	else if (strcmp(method, "ping") == 0) {
		n = emit_rpc_result(id, cJSON_CreateObject(), out, outcap);
	}
	else if (strcmp(method, "tools/list") == 0) {
		cJSON *result = cJSON_CreateObject();
		cJSON_AddItemToObject(result, "tools", build_tools_array());
		n = emit_rpc_result(id, result, out, outcap);
	}
	else if (strcmp(method, "tools/call") == 0) {
		n = handle_tools_call(be, id, params, out, outcap);
	}
	else {
		n = emit_rpc_error(id, -32601, "method not found", out, outcap);
	}

	cJSON_Delete(req);
	return n;
}

/* ---- HTTP transport ---- */

struct AgentMcpConn {
	const AgentBackend *be;
	char token[768]; /* fits any 255-wchar ini value as UTF-8 */
	int require_token;
	char bind_host[64];
	char *buf; /* request accumulator */
	size_t len;
	size_t cap;
};

AgentMcpConn *agent_mcp_conn_new(const AgentBackend *be, const char *token, const char *bind_host)
{
	AgentMcpConn *c = (AgentMcpConn *)calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;
	c->be = be;
	if (token != NULL) {
		snprintf(c->token, sizeof(c->token), "%s", token);
	}
	if (bind_host != NULL) {
		snprintf(c->bind_host, sizeof(c->bind_host), "%s", bind_host);
	}
	c->require_token = (c->token[0] != 0);
	return c;
}

/* Anti-DNS-rebinding: a browser tricked into hitting the loopback port still
 * sends the attacker's hostname in Host/Origin, so accept only loopback (and
 * the configured bind address). Non-browser clients send no Origin. */
static int host_token_ok(const char *host, const char *tok)
{
	size_t n = strlen(tok);
	return strncmp(host, tok, n) == 0 && (host[n] == 0 || host[n] == ':');
}

static int host_allowed(const char *host, const char *bind_host)
{
	if (host[0] == 0) {
		return 1; /* absent Host: not a browser */
	}
	if (host_token_ok(host, "127.0.0.1") || host_token_ok(host, "localhost") ||
		host_token_ok(host, "[::1]")) {
		return 1;
	}
	return bind_host[0] != 0 && host_token_ok(host, bind_host);
}

static int origin_allowed(const char *origin, const char *bind_host)
{
	if (origin[0] == 0) {
		return 1; /* no Origin header: non-browser client */
	}
	const char *p = strstr(origin, "://");
	if (p == NULL) {
		return 0;
	}
	return host_allowed(p + 3, bind_host);
}

void agent_mcp_conn_free(AgentMcpConn *c)
{
	if (c == NULL)
		return;
	free(c->buf);
	free(c);
}

/* Case-insensitive search of a header value in the (NUL-free) header block.
 * headers points at the start of the first header line; hlen its length. */
static int header_value(const char *headers, size_t hlen, const char *name, char *val, size_t valcap)
{
	size_t nlen = strlen(name);
	size_t i = 0;
	while (i < hlen) {
		size_t j = i;
		while (j < hlen && headers[j] != '\r' && headers[j] != '\n')
			j++;
		/* line is [i, j) */
		if (j - i > nlen && headers[i + nlen] == ':') {
			int match = 1;
			for (size_t k = 0; k < nlen; k++) {
				char a = headers[i + k], b = name[k];
				if (a >= 'A' && a <= 'Z')
					a = (char)(a - 'A' + 'a');
				if (b >= 'A' && b <= 'Z')
					b = (char)(b - 'A' + 'a');
				if (a != b) {
					match = 0;
					break;
				}
			}
			if (match) {
				size_t v = i + nlen + 1;
				while (v < j && (headers[v] == ' ' || headers[v] == '\t'))
					v++;
				size_t vl = j - v;
				if (vl >= valcap)
					vl = valcap - 1;
				memcpy(val, headers + v, vl);
				val[vl] = 0;
				return 1;
			}
		}
		/* advance past CRLF */
		i = j;
		while (i < hlen && (headers[i] == '\r' || headers[i] == '\n'))
			i++;
	}
	return 0;
}

/* Append a formatted fragment at *pos, clamping so out+*pos never runs past the
 * buffer and outcap-*pos never underflows (snprintf returns the would-be length,
 * which can exceed the space left). */
static void http_append(char *out, size_t outcap, size_t *pos, const char *fmt, ...)
{
	if (*pos >= outcap)
		return;
	va_list ap;
	va_start(ap, fmt);
	int w = vsnprintf(out + *pos, outcap - *pos, fmt, ap);
	va_end(ap);
	if (w < 0)
		return;
	*pos += (size_t)w;
	if (*pos > outcap)
		*pos = outcap; /* truncated; further appends no-op via the guard above */
}

static size_t build_http(char *out, size_t outcap, int code, const char *status, const char *ctype,
						 const char *extra, const char *body, size_t bodylen, int keepalive)
{
	size_t pos = 0;
	http_append(out, outcap, &pos, "HTTP/1.1 %d %s\r\n", code, status);
	if (ctype != NULL)
		http_append(out, outcap, &pos, "Content-Type: %s\r\n", ctype);
	http_append(out, outcap, &pos, "Content-Length: %zu\r\n", bodylen);
	http_append(out, outcap, &pos, "Connection: %s\r\n", keepalive ? "keep-alive" : "close");
	if (extra != NULL)
		http_append(out, outcap, &pos, "%s", extra);
	http_append(out, outcap, &pos, "\r\n");
	if (body != NULL && bodylen > 0 && pos + bodylen <= outcap) {
		memcpy(out + pos, body, bodylen);
		pos += bodylen;
	}
	return pos;
}

/* Try to process one buffered HTTP request. Returns response length (>0), or 0
 * if incomplete. Consumes the request from c->buf on success. */
static size_t process_one(AgentMcpConn *c, char *out, size_t outcap, int *close_after)
{
	*close_after = 0;
	if (c->len == 0)
		return 0;

	/* find end of headers */
	const char *hdrend = NULL;
	for (size_t i = 0; i + 3 < c->len; i++) {
		if (c->buf[i] == '\r' && c->buf[i + 1] == '\n' && c->buf[i + 2] == '\r' && c->buf[i + 3] == '\n') {
			hdrend = c->buf + i + 4;
			break;
		}
	}
	if (hdrend == NULL) {
		if (c->len > MCP_MAX_HEADER) {
			*close_after = 1;
			return build_http(out, outcap, 431, "Request Header Fields Too Large", NULL, NULL, NULL, 0, 0);
		}
		return 0; /* need more */
	}

	size_t hlen = (size_t)(hdrend - c->buf) - 4;
	const char *reqline_end = (const char *)memchr(c->buf, '\r', hlen);
	if (reqline_end == NULL) {
		*close_after = 1;
		return build_http(out, outcap, 400, "Bad Request", NULL, NULL, NULL, 0, 0);
	}

	/* method = first token of request line */
	char method[8] = {0};
	size_t mi = 0;
	for (const char *p = c->buf; p < reqline_end && *p != ' ' && mi < sizeof(method) - 1; p++)
		method[mi++] = *p;

	const char *headers = reqline_end; /* header lines start after the request line */
	size_t headers_len = hlen - (size_t)(reqline_end - c->buf);

	char clbuf[32];
	size_t content_length = 0;
	if (header_value(headers, headers_len, "Content-Length", clbuf, sizeof(clbuf)))
		content_length = (size_t)strtoull(clbuf, NULL, 10);
	if (content_length > MCP_MAX_BODY) {
		*close_after = 1;
		return build_http(out, outcap, 413, "Payload Too Large", NULL, NULL, NULL, 0, 0);
	}

	size_t have_body = c->len - (size_t)(hdrend - c->buf);
	if (have_body < content_length)
		return 0; /* wait for full body */

	char conbuf[32];
	int keepalive = 1;
	if (header_value(headers, headers_len, "Connection", conbuf, sizeof(conbuf))) {
		if (str_ieq(conbuf, "close"))
			keepalive = 0;
	}

	size_t rn;

	/* anti-rebinding: validate Host/Origin before doing anything else */
	char hostbuf[256];
	hostbuf[0] = 0;
	header_value(headers, headers_len, "Host", hostbuf, sizeof(hostbuf));
	char originbuf[256];
	originbuf[0] = 0;
	header_value(headers, headers_len, "Origin", originbuf, sizeof(originbuf));
	if (!host_allowed(hostbuf, c->bind_host) || !origin_allowed(originbuf, c->bind_host)) {
		rn = build_http(out, outcap, 403, "Forbidden", "text/plain", NULL, "forbidden\n", 10, keepalive);
		goto consume;
	}

	/* auth */
	if (c->require_token) {
		char authbuf[256];
		int ok = 0;
		if (header_value(headers, headers_len, "Authorization", authbuf, sizeof(authbuf))) {
			const char *bearer = "Bearer ";
			if (strncmp(authbuf, bearer, 7) == 0 && agent_ct_eq(authbuf + 7, c->token))
				ok = 1;
		}
		if (!ok) {
			rn = build_http(out, outcap, 401, "Unauthorized", "text/plain",
							"WWW-Authenticate: Bearer\r\n", "unauthorized\n", 13, keepalive);
			goto consume;
		}
	}

	if (strcmp(method, "POST") == 0) {
		/* Stateless server: no Mcp-Session-Id is assigned. JSON-RPC body -> MCP response. */
		char *rpc = (char *)malloc(outcap);
		if (rpc == NULL) {
			rn = build_http(out, outcap, 500, "Internal Server Error", NULL, NULL, NULL, 0, keepalive);
			goto consume;
		}
		size_t bl = agent_mcp_handle(c->be, hdrend, content_length, rpc, outcap);
		if (bl == 0) {
			/* notification: no JSON-RPC response is due */
			rn = build_http(out, outcap, 202, "Accepted", NULL, NULL, NULL, 0, keepalive);
		}
		else {
			rn = build_http(out, outcap, 200, "OK", "application/json", NULL, rpc, bl, keepalive);
		}
		free(rpc);
	}
	else if (strcmp(method, "GET") == 0) {
		/* No server-initiated stream is offered. */
		rn = build_http(out, outcap, 405, "Method Not Allowed", "text/plain", "Allow: POST\r\n",
						"method not allowed\n", 19, keepalive);
	}
	else {
		rn = build_http(out, outcap, 405, "Method Not Allowed", "text/plain", "Allow: POST\r\n",
						"method not allowed\n", 19, keepalive);
	}

consume: {
	size_t consumed = (size_t)(hdrend - c->buf) + content_length;
	memmove(c->buf, c->buf + consumed, c->len - consumed);
	c->len -= consumed;
}
	if (!keepalive)
		*close_after = 1;
	return rn;
}

size_t agent_mcp_feed(AgentMcpConn *c, const char *data, size_t len, char *out, size_t outcap, int *close_after)
{
	if (len > 0) {
		if (c->len + len > c->cap) {
			size_t need = c->len + len;
			char *nb = (char *)realloc(c->buf, need);
			if (nb == NULL) {
				*close_after = 1;
				return build_http(out, outcap, 500, "Internal Server Error", NULL, NULL, NULL, 0, 0);
			}
			c->buf = nb;
			c->cap = need;
		}
		memcpy(c->buf + c->len, data, len);
		c->len += len;
	}
	return process_one(c, out, outcap, close_after);
}
