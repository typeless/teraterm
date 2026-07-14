/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Host unit test for agent_mcp. Compile & run natively:
 *   cc -I.. -I../../../libs/cJSON -o t test_agent_mcp.c ../agent_mcp.c \
 *      ../agent_jsonrpc.c ../../../libs/cJSON/cJSON.c && ./t
 */

#include "agent_mcp.h"
#include "agent_jsonrpc.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(cond)                                                \
	do {                                                           \
		if (!(cond)) {                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			failures++;                                            \
		}                                                          \
	} while (0)

/* ---- fake backend (mirrors the agent_jsonrpc test) ---- */

typedef struct {
	int connected;
	int allow_send;
	char last_text[256];
	size_t last_text_len;
} Fake;

static int fk_check_token(void *c, const char *t) { (void)c; return t && strcmp(t, "secret") == 0; }
static int fk_status(void *c, const char *s, AgentStatus *o)
{
	(void)s;
	Fake *f = (Fake *)c;
	memset(o, 0, sizeof(*o));
	o->ready = f->connected;
	o->offset = 7;
	strcpy(o->host, "host.example");
	return 0;
}
static int fk_list(void *c, AgentSessionInfo *o, int max)
{
	(void)c;
	if (max < 1) return 0;
	memset(o, 0, sizeof(*o));
	strcpy(o[0].session, "w1");
	strcpy(o[0].host, "host.example");
	return 1;
}
static int fk_read_since(void *c, const char *s, uint64_t since, void *o, size_t max, uint64_t *from, int *gap)
{
	(void)c; (void)s; (void)since;
	const char *d = "hi";
	size_t n = strlen(d); if (n > max) n = max;
	memcpy(o, d, n); *from = 3; *gap = 0; return (int)n;
}
static int fk_read_sb(void *c, const char *s, void *o, size_t max, uint64_t *from)
{
	(void)c; (void)s;
	const char *d = "sb"; size_t n = strlen(d); if (n > max) n = max;
	memcpy(o, d, n); *from = 0; return (int)n;
}
static int fk_send_text(void *c, const char *s, const char *u, size_t len)
{
	(void)s; Fake *f = (Fake *)c;
	if (!f->connected) return AGENT_ERR_NOTCONN;
	if (!f->allow_send) return AGENT_ERR_NOTALLOWED;
	memcpy(f->last_text, u, len); f->last_text[len] = 0; f->last_text_len = len; return (int)len;
}
static int fk_send_bytes(void *c, const char *s, const void *d, size_t len)
{
	(void)s; (void)d; Fake *f = (Fake *)c;
	if (!f->connected) return AGENT_ERR_NOTCONN;
	if (!f->allow_send) return AGENT_ERR_NOTALLOWED;
	return (int)len;
}
static void fill(AgentBackend *be, Fake *f)
{
	memset(be, 0, sizeof(*be));
	be->ctx = f;
	be->check_token = fk_check_token;
	be->get_status = fk_status;
	be->list_sessions = fk_list;
	be->read_since = fk_read_since;
	be->read_scrollback = fk_read_sb;
	be->send_text = fk_send_text;
	be->send_bytes = fk_send_bytes;
}

/* ---- MCP JSON-RPC layer ---- */

static void test_initialize(void)
{
	Fake f = {0}; AgentBackend be; fill(&be, &f);
	char out[4096];
	const char *req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
					  "\"params\":{\"protocolVersion\":\"2025-06-18\"}}";
	size_t n = agent_mcp_handle(&be, req, strlen(req), out, sizeof(out));
	CHECK(n > 0);
	cJSON *r = cJSON_Parse(out);
	cJSON *res = cJSON_GetObjectItem(r, "result");
	CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(res, "protocolVersion")), "2025-06-18") == 0);
	cJSON *caps = cJSON_GetObjectItem(res, "capabilities");
	CHECK(cJSON_GetObjectItem(caps, "tools") != NULL);
	cJSON *info = cJSON_GetObjectItem(res, "serverInfo");
	CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(info, "name")), "teraterm-agent") == 0);
	cJSON_Delete(r);
}

static void test_tools_list(void)
{
	Fake f = {0}; AgentBackend be; fill(&be, &f);
	char out[8192];
	const char *req = "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}";
	agent_mcp_handle(&be, req, strlen(req), out, sizeof(out));
	cJSON *r = cJSON_Parse(out);
	cJSON *tools = cJSON_GetObjectItem(cJSON_GetObjectItem(r, "result"), "tools");
	CHECK(cJSON_IsArray(tools));
	CHECK(cJSON_GetArraySize(tools) == 7);
	int found_send_line = 0;
	cJSON *t;
	cJSON_ArrayForEach(t, tools) {
		const char *nm = cJSON_GetStringValue(cJSON_GetObjectItem(t, "name"));
		CHECK(cJSON_GetObjectItem(t, "inputSchema") != NULL);
		if (nm && strcmp(nm, "send_line") == 0) found_send_line = 1;
	}
	CHECK(found_send_line);
	cJSON_Delete(r);
}

static void test_tools_call_status(void)
{
	Fake f = {0}; f.connected = 1; AgentBackend be; fill(&be, &f);
	char out[4096];
	const char *req = "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
					  "\"params\":{\"name\":\"status\",\"arguments\":{}}}";
	agent_mcp_handle(&be, req, strlen(req), out, sizeof(out));
	cJSON *r = cJSON_Parse(out);
	cJSON *res = cJSON_GetObjectItem(r, "result");
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(res, "isError")));
	cJSON *sc = cJSON_GetObjectItem(res, "structuredContent");
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(sc, "ready")));
	cJSON *content = cJSON_GetObjectItem(res, "content");
	CHECK(cJSON_IsArray(content) && cJSON_GetArraySize(content) == 1);
	cJSON_Delete(r);
}

static void test_tools_call_send_line(void)
{
	Fake f = {0}; f.connected = 1; f.allow_send = 1; AgentBackend be; fill(&be, &f);
	char out[4096];
	const char *req = "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
					  "\"params\":{\"name\":\"send_line\",\"arguments\":{\"text\":\"ls\"}}}";
	agent_mcp_handle(&be, req, strlen(req), out, sizeof(out));
	CHECK(f.last_text_len == 3);
	CHECK(memcmp(f.last_text, "ls\r", 3) == 0);
}

static void test_tools_call_unknown(void)
{
	Fake f = {0}; AgentBackend be; fill(&be, &f);
	char out[4096];
	const char *req = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
					  "\"params\":{\"name\":\"nope\",\"arguments\":{}}}";
	agent_mcp_handle(&be, req, strlen(req), out, sizeof(out));
	cJSON *r = cJSON_Parse(out);
	cJSON *res = cJSON_GetObjectItem(r, "result");
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(res, "isError")));
	cJSON_Delete(r);
}

static void test_notification(void)
{
	Fake f = {0}; AgentBackend be; fill(&be, &f);
	char out[1024];
	const char *req = "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
	size_t n = agent_mcp_handle(&be, req, strlen(req), out, sizeof(out));
	CHECK(n == 0);
}

/* ---- HTTP transport ---- */

static int contains(const char *hay, const char *needle) { return strstr(hay, needle) != NULL; }

static void test_http_post(void)
{
	Fake f = {0}; f.connected = 1; AgentBackend be; fill(&be, &f);
	AgentMcpConn *c = agent_mcp_conn_new(&be, "secret", "");
	char out[8192];
	int close_after = 0;

	const char *body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}";
	char req[512];
	int rn = snprintf(req, sizeof(req),
					  "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:5334\r\nAuthorization: Bearer secret\r\n"
					  "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n%s",
					  strlen(body), body);
	size_t n = agent_mcp_feed(c, req, (size_t)rn, out, sizeof(out), &close_after);
	out[n] = 0;
	CHECK(contains(out, "HTTP/1.1 200"));
	CHECK(contains(out, "application/json"));
	CHECK(contains(out, "\"tools\""));
	agent_mcp_conn_free(c);
}

static void test_http_auth_fail(void)
{
	Fake f = {0}; AgentBackend be; fill(&be, &f);
	AgentMcpConn *c = agent_mcp_conn_new(&be, "secret", "");
	char out[4096];
	int close_after = 0;
	const char *body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}";
	char req[512];
	int rn = snprintf(req, sizeof(req),
					  "POST /mcp HTTP/1.1\r\nAuthorization: Bearer wrong\r\nContent-Length: %zu\r\n\r\n%s",
					  strlen(body), body);
	size_t n = agent_mcp_feed(c, req, (size_t)rn, out, sizeof(out), &close_after);
	out[n] = 0;
	CHECK(contains(out, "HTTP/1.1 401"));
	agent_mcp_conn_free(c);
}

static void test_http_get_405(void)
{
	Fake f = {0}; AgentBackend be; fill(&be, &f);
	AgentMcpConn *c = agent_mcp_conn_new(&be, "", ""); /* no token */
	char out[4096];
	int close_after = 0;
	const char *req = "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
	size_t n = agent_mcp_feed(c, req, strlen(req), out, sizeof(out), &close_after);
	out[n] = 0;
	CHECK(contains(out, "HTTP/1.1 405"));
	agent_mcp_conn_free(c);
}

static void test_http_partial(void)
{
	Fake f = {0}; f.connected = 1; AgentBackend be; fill(&be, &f);
	AgentMcpConn *c = agent_mcp_conn_new(&be, "", ""); /* no token */
	char out[8192];
	int close_after = 0;

	const char *body = "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}";
	char req[512];
	int rn = snprintf(req, sizeof(req),
					  "POST /mcp HTTP/1.1\r\nContent-Length: %zu\r\n\r\n%s", strlen(body), body);
	/* feed in two chunks: split mid-headers */
	size_t split = 20;
	size_t n = agent_mcp_feed(c, req, split, out, sizeof(out), &close_after);
	CHECK(n == 0); /* incomplete */
	n = agent_mcp_feed(c, req + split, (size_t)rn - split, out, sizeof(out), &close_after);
	out[n] = 0;
	CHECK(contains(out, "HTTP/1.1 200"));
	agent_mcp_conn_free(c);
}

static void test_http_rebinding_rejected(void)
{
	Fake f = {0}; f.connected = 1; AgentBackend be; fill(&be, &f);
	AgentMcpConn *c = agent_mcp_conn_new(&be, "", ""); /* no token: rely on Host/Origin */
	char out[4096];
	int close_after = 0;
	const char *body = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}";

	/* attacker hostname in Host (DNS rebinding) -> 403 */
	char req[512];
	int rn = snprintf(req, sizeof(req),
					  "POST /mcp HTTP/1.1\r\nHost: evil.example.com\r\nContent-Length: %zu\r\n\r\n%s",
					  strlen(body), body);
	size_t n = agent_mcp_feed(c, req, (size_t)rn, out, sizeof(out), &close_after);
	out[n] = 0;
	CHECK(contains(out, "HTTP/1.1 403"));
	agent_mcp_conn_free(c);

	/* browser Origin from another site -> 403 even with a loopback Host */
	c = agent_mcp_conn_new(&be, "", "");
	rn = snprintf(req, sizeof(req),
				  "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: https://evil.example.com\r\n"
				  "Content-Length: %zu\r\n\r\n%s",
				  strlen(body), body);
	n = agent_mcp_feed(c, req, (size_t)rn, out, sizeof(out), &close_after);
	out[n] = 0;
	CHECK(contains(out, "HTTP/1.1 403"));
	agent_mcp_conn_free(c);

	/* opaque browser Origin "null" (sandboxed iframe, data:/file:) is a real
	   browser origin, not an absent one -> must be rejected, not allowed */
	c = agent_mcp_conn_new(&be, "", "");
	rn = snprintf(req, sizeof(req),
				  "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: null\r\n"
				  "Content-Length: %zu\r\n\r\n%s",
				  strlen(body), body);
	n = agent_mcp_feed(c, req, (size_t)rn, out, sizeof(out), &close_after);
	out[n] = 0;
	CHECK(contains(out, "HTTP/1.1 403"));
	agent_mcp_conn_free(c);
}

int main(void)
{
	test_initialize();
	test_tools_list();
	test_tools_call_status();
	test_tools_call_send_line();
	test_tools_call_unknown();
	test_notification();
	test_http_post();
	test_http_auth_fail();
	test_http_get_405();
	test_http_partial();
	test_http_rebinding_rejected();

	if (failures == 0) {
		printf("all agent_mcp tests passed\n");
		return 0;
	}
	printf("%d agent_mcp assertion(s) failed\n", failures);
	return 1;
}
