/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Host unit test for agent_jsonrpc. Compile & run natively:
 *   cc -I.. -I../../../libs/cJSON -o t test_agent_jsonrpc.c ../agent_jsonrpc.c \
 *      ../../../libs/cJSON/cJSON.c && ./t
 */

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

/* ---- fake backend ---- */

typedef struct {
	int require_token;
	int allow_send;
	int connected;
	/* recorded last send */
	char last_text[256];
	size_t last_text_len;
	unsigned char last_bytes[256];
	size_t last_bytes_len;
	/* recorded last zmodem_send */
	char last_zsend_path[256];
	int last_zsend_binary;
	int last_zsend_called;
	/* transfer state reported by get_status */
	int xfer_active;
	int xfer_last_result;
} Fake;

static int fk_check_token(void *ctx, const char *token)
{
	(void)ctx;
	return token != NULL && strcmp(token, "s3cret") == 0;
}

static int fk_get_status(void *ctx, const char *session, AgentStatus *out)
{
	(void)session;
	Fake *f = (Fake *)ctx;
	memset(out, 0, sizeof(*out));
	out->ready = f->connected;
	out->port_type = 2;
	out->offset = 42;
	out->cols = 80;
	out->rows = 24;
	strcpy(out->host, "example.com");
	out->xfer_active = f->xfer_active;
	out->xfer_last_result = f->xfer_last_result;
	return 0;
}

static int fk_list(void *ctx, AgentSessionInfo *out, int max)
{
	(void)ctx;
	if (max < 1)
		return 0;
	memset(&out[0], 0, sizeof(out[0]));
	strcpy(out[0].session, "w1");
	strcpy(out[0].host, "example.com");
	strcpy(out[0].title, "prod");
	out[0].port_type = 2;
	out[0].ready = 1;
	return 1;
}

static int fk_read_since(void *ctx, const char *session, uint64_t since, void *out, size_t max,
						 uint64_t *from, int *gap)
{
	(void)ctx;
	(void)session;
	(void)since;
	const char *data = "world";
	size_t n = strlen(data);
	if (n > max)
		n = max;
	memcpy(out, data, n);
	*from = 10;
	*gap = 0;
	return (int)n;
}

static int fk_read_scrollback(void *ctx, const char *session, void *out, size_t max, uint64_t *from)
{
	(void)ctx;
	(void)session;
	const char *data = "history";
	size_t n = strlen(data);
	if (n > max)
		n = max;
	memcpy(out, data, n);
	*from = 0;
	return (int)n;
}

static int fk_send_text(void *ctx, const char *session, const char *utf8, size_t len)
{
	(void)session;
	Fake *f = (Fake *)ctx;
	if (!f->connected)
		return AGENT_ERR_NOTCONN;
	if (!f->allow_send)
		return AGENT_ERR_NOTALLOWED;
	memcpy(f->last_text, utf8, len);
	f->last_text[len] = 0;
	f->last_text_len = len;
	return (int)len;
}

static int fk_send_bytes(void *ctx, const char *session, const void *data, size_t len)
{
	(void)session;
	Fake *f = (Fake *)ctx;
	if (!f->connected)
		return AGENT_ERR_NOTCONN;
	if (!f->allow_send)
		return AGENT_ERR_NOTALLOWED;
	memcpy(f->last_bytes, data, len);
	f->last_bytes_len = len;
	return (int)len;
}

static int fk_zmodem_send(void *ctx, const char *session, const char *pathU8, int binary)
{
	(void)session;
	Fake *f = (Fake *)ctx;
	if (!f->connected)
		return AGENT_ERR_NOTCONN;
	if (!f->allow_send)
		return AGENT_ERR_NOTALLOWED;
	strncpy(f->last_zsend_path, pathU8, sizeof(f->last_zsend_path) - 1);
	f->last_zsend_path[sizeof(f->last_zsend_path) - 1] = 0;
	f->last_zsend_binary = binary;
	f->last_zsend_called = 1;
	return 0; /* transfer started */
}

static void fill_backend(AgentBackend *be, Fake *f)
{
	memset(be, 0, sizeof(*be));
	be->ctx = f;
	be->require_token = f->require_token;
	be->check_token = fk_check_token;
	be->get_status = fk_get_status;
	be->list_sessions = fk_list;
	be->read_since = fk_read_since;
	be->read_scrollback = fk_read_scrollback;
	be->send_text = fk_send_text;
	be->send_bytes = fk_send_bytes;
	be->zmodem_send = fk_zmodem_send;
}

/* ---- helpers ---- */

static cJSON *dispatch(const AgentBackend *be, AgentConn *conn, const char *req, char *respbuf, size_t cap)
{
	size_t n = agent_dispatch(be, conn, req, strlen(req), respbuf, cap);
	if (n == 0)
		return NULL;
	return cJSON_Parse(respbuf);
}

static int result_ok(cJSON *resp)
{
	cJSON *ok = cJSON_GetObjectItem(resp, "ok");
	return cJSON_IsTrue(ok);
}

/* ---- tests ---- */

static void test_hello_token(void)
{
	Fake f = {0};
	f.require_token = 1;
	AgentBackend be;
	fill_backend(&be, &f);
	AgentConn conn = {0};
	char buf[1024];

	/* wrong token -> ok:false + should_close, not authed */
	cJSON *r = dispatch(&be, &conn, "{\"id\":1,\"method\":\"hello\",\"params\":{\"token\":\"nope\"}}", buf, sizeof(buf));
	CHECK(r != NULL);
	CHECK(!result_ok(r));
	CHECK(conn.authed == 0);
	CHECK(conn.should_close == 1);
	cJSON_Delete(r);

	/* correct token -> ok:true, authed */
	conn.should_close = 0;
	r = dispatch(&be, &conn, "{\"id\":2,\"method\":\"hello\",\"params\":{\"token\":\"s3cret\"}}", buf, sizeof(buf));
	CHECK(r != NULL);
	CHECK(result_ok(r));
	CHECK(conn.authed == 1);
	cJSON *res = cJSON_GetObjectItem(r, "result");
	CHECK(cJSON_GetObjectItem(res, "version") != NULL);
	cJSON_Delete(r);
}

static void test_requires_auth(void)
{
	Fake f = {0};
	f.require_token = 1;
	AgentBackend be;
	fill_backend(&be, &f);
	AgentConn conn = {0}; /* not authed */
	char buf[1024];

	cJSON *r = dispatch(&be, &conn, "{\"id\":3,\"method\":\"status\"}", buf, sizeof(buf));
	CHECK(r != NULL);
	CHECK(!result_ok(r));
	cJSON_Delete(r);
}

static void test_status(void)
{
	Fake f = {0};
	f.connected = 1;
	AgentBackend be;
	fill_backend(&be, &f);
	AgentConn conn = {0};
	conn.authed = 1;
	char buf[1024];

	cJSON *r = dispatch(&be, &conn, "{\"id\":4,\"method\":\"status\"}", buf, sizeof(buf));
	CHECK(r != NULL);
	CHECK(result_ok(r));
	cJSON *res = cJSON_GetObjectItem(r, "result");
	CHECK(cJSON_IsTrue(cJSON_GetObjectItem(res, "ready")));
	CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(res, "host")), "example.com") == 0);
	CHECK((uint64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(res, "offset")) == 42);
	cJSON_Delete(r);
}

static void test_read_new_output(void)
{
	Fake f = {0};
	AgentBackend be;
	fill_backend(&be, &f);
	AgentConn conn = {0};
	conn.authed = 1;
	char buf[1024];

	cJSON *r = dispatch(&be, &conn, "{\"id\":5,\"method\":\"read_new_output\",\"params\":{\"since\":0}}", buf, sizeof(buf));
	CHECK(r != NULL);
	CHECK(result_ok(r));
	cJSON *res = cJSON_GetObjectItem(r, "result");
	CHECK((uint64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(res, "from")) == 10);
	CHECK((uint64_t)cJSON_GetNumberValue(cJSON_GetObjectItem(res, "next")) == 15); /* 10 + 5 */
	CHECK(cJSON_IsFalse(cJSON_GetObjectItem(res, "gap")));
	const char *b64 = cJSON_GetStringValue(cJSON_GetObjectItem(res, "data_b64"));
	unsigned char decoded[64];
	size_t dn = agent_b64_decode(b64, decoded);
	CHECK(dn == 5);
	CHECK(memcmp(decoded, "world", 5) == 0);
	cJSON_Delete(r);
}

static void test_send_line(void)
{
	Fake f = {0};
	f.connected = 1;
	f.allow_send = 1;
	AgentBackend be;
	fill_backend(&be, &f);
	AgentConn conn = {0};
	conn.authed = 1;
	char buf[1024];

	cJSON *r = dispatch(&be, &conn, "{\"id\":6,\"method\":\"send_line\",\"params\":{\"text\":\"ls\"}}", buf, sizeof(buf));
	CHECK(r != NULL);
	CHECK(result_ok(r));
	/* default newline is CR */
	CHECK(f.last_text_len == 3);
	CHECK(memcmp(f.last_text, "ls\r", 3) == 0);
	cJSON_Delete(r);
}

static void test_send_bytes(void)
{
	Fake f = {0};
	f.connected = 1;
	f.allow_send = 1;
	AgentBackend be;
	fill_backend(&be, &f);
	AgentConn conn = {0};
	conn.authed = 1;
	char buf[1024];

	char b64[64];
	agent_b64_encode("\x03\xff\x00Z", 4, b64); /* ctrl-c, IAC, NUL, 'Z' */
	char req[256];
	snprintf(req, sizeof(req), "{\"id\":7,\"method\":\"send_bytes\",\"params\":{\"data_b64\":\"%s\"}}", b64);
	cJSON *r = dispatch(&be, &conn, req, buf, sizeof(buf));
	CHECK(r != NULL);
	CHECK(result_ok(r));
	CHECK(f.last_bytes_len == 4);
	CHECK(memcmp(f.last_bytes, "\x03\xff\x00Z", 4) == 0);
	cJSON_Delete(r);
}

static void test_send_key(void)
{
	Fake f = {0};
	f.connected = 1;
	f.allow_send = 1;
	AgentBackend be;
	fill_backend(&be, &f);
	AgentConn conn = {0};
	conn.authed = 1;
	char buf[1024];

	cJSON *r = dispatch(&be, &conn, "{\"id\":8,\"method\":\"send_key\",\"params\":{\"key\":\"ctrl-c\"}}", buf, sizeof(buf));
	CHECK(r != NULL);
	CHECK(result_ok(r));
	CHECK(f.last_bytes_len == 1);
	CHECK(f.last_bytes[0] == 0x03);
	cJSON_Delete(r);
}

static void test_send_not_allowed(void)
{
	Fake f = {0};
	f.connected = 1;
	f.allow_send = 0; /* not armed */
	AgentBackend be;
	fill_backend(&be, &f);
	AgentConn conn = {0};
	conn.authed = 1;
	char buf[1024];

	cJSON *r = dispatch(&be, &conn, "{\"id\":9,\"method\":\"send_line\",\"params\":{\"text\":\"rm -rf\"}}", buf, sizeof(buf));
	CHECK(r != NULL);
	CHECK(!result_ok(r));
	CHECK(f.last_text_len == 0);
	cJSON_Delete(r);
}

static void test_status_transfer(void)
{
	AgentConn conn = {0};
	conn.authed = 1;
	char buf[1024];
	struct {
		int active, last;
		const char *state;
		int has_ok, ok;
	} cases[] = {
		{0, -1, "idle", 0, 0},
		{1, -1, "active", 0, 0},
		{0, 1, "done", 1, 1},
		{0, 0, "done", 1, 0},
	};
	for (int i = 0; i < 4; i++) {
		Fake f = {0};
		f.connected = 1;
		f.xfer_active = cases[i].active;
		f.xfer_last_result = cases[i].last;
		AgentBackend be;
		fill_backend(&be, &f);
		cJSON *r = dispatch(&be, &conn, "{\"id\":30,\"method\":\"status\"}", buf, sizeof(buf));
		CHECK(r != NULL);
		cJSON *res = cJSON_GetObjectItem(r, "result");
		cJSON *t = cJSON_GetObjectItem(res, "transfer");
		CHECK(t != NULL);
		cJSON *stt = cJSON_GetObjectItem(t, "state");
		CHECK(cJSON_IsString(stt) && strcmp(stt->valuestring, cases[i].state) == 0);
		cJSON *ok = cJSON_GetObjectItem(t, "ok");
		if (cases[i].has_ok) {
			CHECK(cJSON_IsBool(ok));
			CHECK((cJSON_IsTrue(ok) ? 1 : 0) == cases[i].ok);
		} else {
			CHECK(ok == NULL);
		}
		cJSON_Delete(r);
	}
}

static void test_zmodem_send(void)
{
	AgentConn conn = {0};
	conn.authed = 1;
	char buf[1024];

	/* happy path: armed, path parsed, binary defaults on, backend invoked once */
	{
		Fake f = {0};
		f.connected = 1;
		f.allow_send = 1;
		AgentBackend be;
		fill_backend(&be, &f);
		cJSON *r = dispatch(&be, &conn,
			"{\"id\":20,\"method\":\"zmodem_send\",\"params\":{\"path\":\"C:\\\\payload\\\\fw.bin\"}}",
			buf, sizeof(buf));
		CHECK(r != NULL);
		CHECK(result_ok(r));
		CHECK(f.last_zsend_called == 1);
		CHECK(strcmp(f.last_zsend_path, "C:\\payload\\fw.bin") == 0);
		CHECK(f.last_zsend_binary == 1);
		cJSON_Delete(r);
	}

	/* explicit binary:false is honored */
	{
		Fake f = {0};
		f.connected = 1;
		f.allow_send = 1;
		AgentBackend be;
		fill_backend(&be, &f);
		cJSON *r = dispatch(&be, &conn,
			"{\"id\":21,\"method\":\"zmodem_send\",\"params\":{\"path\":\"/tmp/x\",\"binary\":false}}",
			buf, sizeof(buf));
		CHECK(r != NULL);
		CHECK(result_ok(r));
		CHECK(f.last_zsend_called == 1);
		CHECK(f.last_zsend_binary == 0);
		cJSON_Delete(r);
	}

	/* gate: send not armed -> error, backend NOT invoked */
	{
		Fake f = {0};
		f.connected = 1;
		f.allow_send = 0;
		AgentBackend be;
		fill_backend(&be, &f);
		cJSON *r = dispatch(&be, &conn,
			"{\"id\":22,\"method\":\"zmodem_send\",\"params\":{\"path\":\"C:\\\\x\"}}",
			buf, sizeof(buf));
		CHECK(r != NULL);
		CHECK(!result_ok(r));
		CHECK(f.last_zsend_called == 0);
		cJSON_Delete(r);
	}

	/* malformed: missing path -> error, backend NOT invoked */
	{
		Fake f = {0};
		f.connected = 1;
		f.allow_send = 1;
		AgentBackend be;
		fill_backend(&be, &f);
		cJSON *r = dispatch(&be, &conn,
			"{\"id\":23,\"method\":\"zmodem_send\",\"params\":{}}", buf, sizeof(buf));
		CHECK(r != NULL);
		CHECK(!result_ok(r));
		CHECK(f.last_zsend_called == 0);
		cJSON_Delete(r);
	}
}

static void test_unknown_and_malformed(void)
{
	Fake f = {0};
	AgentBackend be;
	fill_backend(&be, &f);
	AgentConn conn = {0};
	conn.authed = 1;
	char buf[1024];

	cJSON *r = dispatch(&be, &conn, "{\"id\":10,\"method\":\"frobnicate\"}", buf, sizeof(buf));
	CHECK(r != NULL);
	CHECK(!result_ok(r));
	cJSON_Delete(r);

	r = dispatch(&be, &conn, "{not json", buf, sizeof(buf));
	CHECK(r != NULL);
	CHECK(!result_ok(r));
	cJSON_Delete(r);
}

static void test_list_sessions(void)
{
	Fake f = {0};
	AgentBackend be;
	fill_backend(&be, &f);
	AgentConn conn = {0};
	conn.authed = 1;
	char buf[1024];

	cJSON *r = dispatch(&be, &conn, "{\"id\":11,\"method\":\"list_sessions\"}", buf, sizeof(buf));
	CHECK(r != NULL);
	CHECK(result_ok(r));
	cJSON *res = cJSON_GetObjectItem(r, "result");
	CHECK(cJSON_IsArray(res));
	CHECK(cJSON_GetArraySize(res) == 1);
	cJSON *s0 = cJSON_GetArrayItem(res, 0);
	CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(s0, "host")), "example.com") == 0);
	cJSON_Delete(r);
}

static void test_b64_roundtrip(void)
{
	const char *samples[] = {"", "a", "ab", "abc", "abcd", "\x00\x01\x02\xff", "Hello, World!"};
	size_t lens[] = {0, 1, 2, 3, 4, 4, 13};
	for (int i = 0; i < 7; i++) {
		char enc[128];
		agent_b64_encode(samples[i], lens[i], enc);
		unsigned char dec[128];
		size_t dn = agent_b64_decode(enc, dec);
		CHECK(dn == lens[i]);
		CHECK(memcmp(dec, samples[i], lens[i]) == 0);
	}
}

static void test_ct_eq(void)
{
	CHECK(agent_ct_eq("", ""));
	CHECK(agent_ct_eq("secret", "secret"));
	CHECK(agent_ct_eq("a-longer-token-value-123", "a-longer-token-value-123"));
	CHECK(!agent_ct_eq("secret", "secreT")); /* same length, last byte differs */
	CHECK(!agent_ct_eq("Xecret", "secret")); /* same length, first byte differs */
	CHECK(!agent_ct_eq("secret", "secre"));  /* longer vs its own prefix */
	CHECK(!agent_ct_eq("secre", "secret"));  /* shorter vs longer */
	CHECK(!agent_ct_eq("", "x"));
	CHECK(!agent_ct_eq("x", ""));
}

int main(void)
{
	test_hello_token();
	test_requires_auth();
	test_status();
	test_read_new_output();
	test_send_line();
	test_send_bytes();
	test_send_key();
	test_send_not_allowed();
	test_status_transfer();
	test_zmodem_send();
	test_unknown_and_malformed();
	test_list_sessions();
	test_b64_roundtrip();
	test_ct_eq();

	if (failures == 0) {
		printf("all agent_jsonrpc tests passed\n");
		return 0;
	}
	printf("%d agent_jsonrpc assertion(s) failed\n", failures);
	return 1;
}
