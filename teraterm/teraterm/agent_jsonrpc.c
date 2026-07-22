/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 */

#include "agent_jsonrpc.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>

#define READ_MAX_DEFAULT 65536
#define READ_MAX_CAP     (1024 * 1024)

/* ---- base64 ---- */

static const char b64_enc_tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t agent_b64_encode(const void *src, size_t srclen, char *dst)
{
	const unsigned char *p = (const unsigned char *)src;
	size_t di = 0;
	size_t i = 0;
	while (i + 3 <= srclen) {
		unsigned v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
		dst[di++] = b64_enc_tab[(v >> 18) & 0x3f];
		dst[di++] = b64_enc_tab[(v >> 12) & 0x3f];
		dst[di++] = b64_enc_tab[(v >> 6) & 0x3f];
		dst[di++] = b64_enc_tab[v & 0x3f];
		i += 3;
	}
	size_t rem = srclen - i;
	if (rem == 1) {
		unsigned v = p[i] << 16;
		dst[di++] = b64_enc_tab[(v >> 18) & 0x3f];
		dst[di++] = b64_enc_tab[(v >> 12) & 0x3f];
		dst[di++] = '=';
		dst[di++] = '=';
	}
	else if (rem == 2) {
		unsigned v = (p[i] << 16) | (p[i + 1] << 8);
		dst[di++] = b64_enc_tab[(v >> 18) & 0x3f];
		dst[di++] = b64_enc_tab[(v >> 12) & 0x3f];
		dst[di++] = b64_enc_tab[(v >> 6) & 0x3f];
		dst[di++] = '=';
	}
	dst[di] = 0;
	return di;
}

static int b64_val(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

size_t agent_b64_decode(const char *src, void *dst)
{
	unsigned char *out = (unsigned char *)dst;
	size_t oi = 0;
	int quad[4];
	int qn = 0;
	for (const char *s = src; *s; s++) {
		if (*s == '=' || *s == '\r' || *s == '\n' || *s == ' ' || *s == '\t')
			continue;
		int v = b64_val(*s);
		if (v < 0)
			return (size_t)-1;
		quad[qn++] = v;
		if (qn == 4) {
			out[oi++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
			out[oi++] = (unsigned char)((quad[1] << 4) | (quad[2] >> 2));
			out[oi++] = (unsigned char)((quad[2] << 6) | quad[3]);
			qn = 0;
		}
	}
	if (qn == 2) {
		out[oi++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
	}
	else if (qn == 3) {
		out[oi++] = (unsigned char)((quad[0] << 2) | (quad[1] >> 4));
		out[oi++] = (unsigned char)((quad[1] << 4) | (quad[2] >> 2));
	}
	else if (qn != 0) {
		return (size_t)-1;
	}
	return oi;
}

int agent_ct_eq(const char *a, const char *b)
{
	size_t la = strlen(a);
	size_t lb = strlen(b);
	size_t n = la < lb ? la : lb;
	volatile unsigned char diff = (unsigned char)(la != lb);
	for (size_t i = 0; i < n; i++)
		diff |= (unsigned char)((unsigned char)a[i] ^ (unsigned char)b[i]);
	return diff == 0;
}

/* ---- response building ---- */

static cJSON *make_response(const cJSON *req)
{
	cJSON *resp = cJSON_CreateObject();
	const cJSON *id = cJSON_GetObjectItem(req, "id");
	if (id != NULL) {
		cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id, 1));
	}
	else {
		cJSON_AddNullToObject(resp, "id");
	}
	return resp;
}

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

static size_t emit_error(const cJSON *req, const char *msg, char *out, size_t cap)
{
	cJSON *resp = make_response(req);
	cJSON_AddBoolToObject(resp, "ok", 0);
	cJSON_AddStringToObject(resp, "error", msg);
	return emit(resp, out, cap);
}

static size_t emit_result(const cJSON *req, cJSON *result, char *out, size_t cap)
{
	cJSON *resp = make_response(req);
	cJSON_AddBoolToObject(resp, "ok", 1);
	cJSON_AddItemToObject(resp, "result", result);
	return emit(resp, out, cap);
}

/* Map a shared AGENT_ERR_* code (or a non-negative success value) to a message,
 * or NULL for success. Used by every backend call that returns count-or-error,
 * so the message reflects the actual code rather than the call site's guess. */
static const char *agent_err_msg(int rc)
{
	switch (rc) {
	case AGENT_ERR_NOTCONN:
		return "not connected";
	case AGENT_ERR_NOTALLOWED:
		return "not allowed";
	case AGENT_ERR_NOSESSION:
		return "unknown session";
	case AGENT_ERR_BUSY:
		return "transfer in progress";
	default:
		return rc < 0 ? "operation failed" : NULL;
	}
}

static const char *param_session(const cJSON *params)
{
	const cJSON *s = cJSON_GetObjectItem(params, "session");
	if (cJSON_IsString(s))
		return s->valuestring;
	return NULL;
}

/* key name -> byte sequence; returns length, 0 if unknown. */
static size_t key_bytes(const char *key, unsigned char *out)
{
	struct {
		const char *name;
		const char *seq;
		size_t len;
	} table[] = {
		{"enter", "\r", 1},        {"return", "\r", 1},   {"tab", "\t", 1},
		{"esc", "\x1b", 1},        {"escape", "\x1b", 1}, {"space", " ", 1},
		{"backspace", "\x7f", 1},  {"delete", "\x1b[3~", 4},
		{"ctrl-c", "\x03", 1},     {"ctrl-d", "\x04", 1}, {"ctrl-z", "\x1a", 1},
		{"ctrl-l", "\x0c", 1},     {"up", "\x1b[A", 3},   {"down", "\x1b[B", 3},
		{"right", "\x1b[C", 3},    {"left", "\x1b[D", 3}, {"home", "\x1b[H", 3},
		{"end", "\x1b[F", 3},
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
		if (strcmp(key, table[i].name) == 0) {
			memcpy(out, table[i].seq, table[i].len);
			return table[i].len;
		}
	}
	return 0;
}

/* ---- method logic (transport-independent) ---- */

static cJSON *call_status(const AgentBackend *be, const cJSON *params, const char **err)
{
	AgentStatus st;
	if (be->get_status(be->ctx, param_session(params), &st) != 0) {
		*err = "unknown session";
		return NULL;
	}
	cJSON *r = cJSON_CreateObject();
	cJSON_AddBoolToObject(r, "ready", st.ready);
	cJSON_AddNumberToObject(r, "port_type", st.port_type);
	cJSON_AddStringToObject(r, "host", st.host);
	cJSON_AddNumberToObject(r, "offset", (double)st.offset);
	cJSON_AddNumberToObject(r, "cols", st.cols);
	cJSON_AddNumberToObject(r, "rows", st.rows);

	/* Transfer state as a tagged union: "ok" is meaningful only once done. */
	cJSON *t = cJSON_CreateObject();
	if (st.xfer_active) {
		cJSON_AddStringToObject(t, "state", "active");
	} else if (st.xfer_last_result < 0) {
		cJSON_AddStringToObject(t, "state", "idle");
	} else {
		cJSON_AddStringToObject(t, "state", "done");
		cJSON_AddBoolToObject(t, "ok", st.xfer_last_result == 1);
	}
	cJSON_AddItemToObject(r, "transfer", t);
	return r;
}

static cJSON *call_list_sessions(const AgentBackend *be)
{
	AgentSessionInfo info[64];
	int n = be->list_sessions(be->ctx, info, 64);
	cJSON *arr = cJSON_CreateArray();
	for (int i = 0; i < n; i++) {
		cJSON *o = cJSON_CreateObject();
		cJSON_AddStringToObject(o, "session", info[i].session);
		cJSON_AddStringToObject(o, "host", info[i].host);
		cJSON_AddStringToObject(o, "title", info[i].title);
		cJSON_AddNumberToObject(o, "port_type", info[i].port_type);
		cJSON_AddBoolToObject(o, "ready", info[i].ready);
		cJSON_AddItemToArray(arr, o);
	}
	return arr;
}

static cJSON *call_read(const AgentBackend *be, const cJSON *params, int scrollback, const char **err)
{
	const cJSON *mbj = cJSON_GetObjectItem(params, "max_bytes");
	size_t max_bytes = cJSON_IsNumber(mbj) ? (size_t)mbj->valuedouble : READ_MAX_DEFAULT;
	if (max_bytes > READ_MAX_CAP)
		max_bytes = READ_MAX_CAP;
	if (max_bytes == 0)
		max_bytes = 1;

	unsigned char *buf = (unsigned char *)malloc(max_bytes);
	if (buf == NULL) {
		*err = "out of memory";
		return NULL;
	}

	uint64_t from = 0;
	int gap = 0;
	int n;
	if (scrollback) {
		n = be->read_scrollback(be->ctx, param_session(params), buf, max_bytes, &from);
	}
	else {
		const cJSON *sj = cJSON_GetObjectItem(params, "since");
		uint64_t since = cJSON_IsNumber(sj) ? (uint64_t)sj->valuedouble : 0;
		n = be->read_since(be->ctx, param_session(params), since, buf, max_bytes, &from, &gap);
	}
	if (n < 0) {
		free(buf);
		*err = agent_err_msg(n);
		return NULL;
	}

	char *b64 = (char *)malloc(((size_t)n + 2) / 3 * 4 + 1);
	if (b64 == NULL) {
		free(buf);
		*err = "out of memory";
		return NULL;
	}
	agent_b64_encode(buf, (size_t)n, b64);

	cJSON *r = cJSON_CreateObject();
	cJSON_AddNumberToObject(r, "from", (double)from);
	cJSON_AddNumberToObject(r, "next", (double)(from + (uint64_t)n));
	cJSON_AddStringToObject(r, "data_b64", b64);
	if (!scrollback)
		cJSON_AddBoolToObject(r, "gap", gap);

	free(b64);
	free(buf);
	return r;
}

static cJSON *call_send_line(const AgentBackend *be, const cJSON *params, const char **err)
{
	const cJSON *tj = cJSON_GetObjectItem(params, "text");
	if (!cJSON_IsString(tj)) {
		*err = "missing text";
		return NULL;
	}
	const cJSON *nj = cJSON_GetObjectItem(params, "newline");
	const char *nl = cJSON_IsString(nj) ? nj->valuestring : "\r";

	size_t tlen = strlen(tj->valuestring);
	size_t nlen = strlen(nl);
	char *combined = (char *)malloc(tlen + nlen + 1);
	if (combined == NULL) {
		*err = "out of memory";
		return NULL;
	}
	memcpy(combined, tj->valuestring, tlen);
	memcpy(combined + tlen, nl, nlen);

	int rc = be->send_text(be->ctx, param_session(params), combined, tlen + nlen);
	free(combined);

	*err = agent_err_msg(rc);
	if (*err != NULL)
		return NULL;

	cJSON *r = cJSON_CreateObject();
	cJSON_AddNumberToObject(r, "sent", rc);
	return r;
}

static cJSON *call_send_bytes(const AgentBackend *be, const cJSON *params, const char **err)
{
	const cJSON *dj = cJSON_GetObjectItem(params, "data_b64");
	if (!cJSON_IsString(dj)) {
		*err = "missing data_b64";
		return NULL;
	}

	size_t declen = strlen(dj->valuestring);
	unsigned char *raw = (unsigned char *)malloc(declen / 4 * 3 + 3);
	if (raw == NULL) {
		*err = "out of memory";
		return NULL;
	}
	size_t rn = agent_b64_decode(dj->valuestring, raw);
	if (rn == (size_t)-1) {
		free(raw);
		*err = "invalid base64";
		return NULL;
	}

	int rc = be->send_bytes(be->ctx, param_session(params), raw, rn);
	free(raw);

	*err = agent_err_msg(rc);
	if (*err != NULL)
		return NULL;

	cJSON *r = cJSON_CreateObject();
	cJSON_AddNumberToObject(r, "sent", rc);
	return r;
}

static cJSON *call_send_key(const AgentBackend *be, const cJSON *params, const char **err)
{
	const cJSON *kj = cJSON_GetObjectItem(params, "key");
	if (!cJSON_IsString(kj)) {
		*err = "missing key";
		return NULL;
	}
	unsigned char seq[8];
	size_t klen = key_bytes(kj->valuestring, seq);
	if (klen == 0) {
		*err = "unknown key";
		return NULL;
	}
	int rc = be->send_bytes(be->ctx, param_session(params), seq, klen);
	*err = agent_err_msg(rc);
	if (*err != NULL)
		return NULL;

	cJSON *r = cJSON_CreateObject();
	cJSON_AddNumberToObject(r, "sent", rc);
	return r;
}

static cJSON *call_zmodem_send(const AgentBackend *be, const cJSON *params, const char **err)
{
	const cJSON *pj = cJSON_GetObjectItem(params, "path");
	if (!cJSON_IsString(pj) || pj->valuestring[0] == 0) {
		*err = "path required";
		return NULL;
	}
	int binary = 1;
	const cJSON *bj = cJSON_GetObjectItem(params, "binary");
	if (cJSON_IsBool(bj))
		binary = cJSON_IsTrue(bj) ? 1 : 0;

	int rc = be->zmodem_send(be->ctx, param_session(params), pj->valuestring, binary);
	*err = agent_err_msg(rc);
	if (*err != NULL)
		return NULL;

	cJSON *r = cJSON_CreateObject();
	cJSON_AddTrueToObject(r, "started");
	return r;
}

cJSON *agent_call(const AgentBackend *be, const char *method, const cJSON *params, const char **err)
{
	*err = NULL;
	if (strcmp(method, "status") == 0)
		return call_status(be, params, err);
	if (strcmp(method, "list_sessions") == 0)
		return call_list_sessions(be);
	if (strcmp(method, "read_new_output") == 0)
		return call_read(be, params, 0, err);
	if (strcmp(method, "read_scrollback") == 0)
		return call_read(be, params, 1, err);
	if (strcmp(method, "send_line") == 0)
		return call_send_line(be, params, err);
	if (strcmp(method, "send_bytes") == 0)
		return call_send_bytes(be, params, err);
	if (strcmp(method, "send_key") == 0)
		return call_send_key(be, params, err);
	if (strcmp(method, "zmodem_send") == 0)
		return call_zmodem_send(be, params, err);
	*err = "unknown method";
	return NULL;
}

/* ---- dispatch ---- */

size_t agent_dispatch(const AgentBackend *be, AgentConn *conn, const char *line, size_t len,
					  char *resp, size_t respcap)
{
	cJSON *req = cJSON_ParseWithLength(line, len);
	if (req == NULL)
		return emit_error(NULL, "parse error", resp, respcap);

	const cJSON *mj = cJSON_GetObjectItem(req, "method");
	if (!cJSON_IsString(mj)) {
		size_t r = emit_error(req, "missing method", resp, respcap);
		cJSON_Delete(req);
		return r;
	}
	const char *method = mj->valuestring;
	const cJSON *params = cJSON_GetObjectItem(req, "params");

	size_t rn;

	if (strcmp(method, "hello") == 0) {
		const cJSON *tj = cJSON_GetObjectItem(params, "token");
		const char *token = cJSON_IsString(tj) ? tj->valuestring : NULL;
		if (be->require_token && !be->check_token(be->ctx, token)) {
			conn->should_close = 1;
			rn = emit_error(req, "authentication failed", resp, respcap);
		}
		else {
			conn->authed = 1;
			cJSON *r = cJSON_CreateObject();
			cJSON_AddNumberToObject(r, "version", AGENT_PROTOCOL_VERSION);
			cJSON_AddStringToObject(r, "server", "teraterm");
			rn = emit_result(req, r, resp, respcap);
		}
		cJSON_Delete(req);
		return rn;
	}

	if (be->require_token && !conn->authed) {
		rn = emit_error(req, "unauthorized", resp, respcap);
		cJSON_Delete(req);
		return rn;
	}

	const char *err = NULL;
	cJSON *result = agent_call(be, method, params, &err);
	if (result != NULL)
		rn = emit_result(req, result, resp, respcap);
	else
		rn = emit_error(req, err != NULL ? err : "error", resp, respcap);

	cJSON_Delete(req);
	return rn;
}
