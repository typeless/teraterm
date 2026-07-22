/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sddl.h>
#include <ntsecapi.h> /* RtlGenRandom */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "teraterm.h"
#include "tttypes.h"
#include "ttwinman.h" /* cv, ts, HVTWin */
#include "ttcommon.h" /* CommBinaryOut, CommTextOutW */
#include "codeconv.h" /* ToWcharU8 */
#include "filesys.h"  /* ZMODEMStartSend, ProtoGetProtoFlag, ProtoGetLastResult */

#include "agent_shmem.h"
#include "agent_jsonrpc.h"
#include "agent_mcp.h"
#include "agent_server.h"

#define AGENT_SHM_NAME "TeraTermAgentShmemV1"

#define WM_AGENT_ACCEPT (WM_APP + 8100)
#define WM_AGENT_IO     (WM_APP + 8101)

#define AGENT_MAX_CLIENTS 8
#define AGENT_RXLINE_MAX  (1024 * 1024) /* drop a client whose line grows past this */
#define AGENT_RESP_MAX    (2 * 1024 * 1024)

typedef struct {
	SOCKET sock;
	int is_mcp;         /* HTTP/MCP client vs raw line-JSON client */
	AgentMcpConn *mcp;  /* non-NULL for MCP clients */
	char *rxbuf;        /* line accumulator (raw clients) */
	size_t rxlen;
	size_t rxcap;
	char *txbuf; /* pending outbound */
	size_t txlen;
	size_t txoff;
	size_t txcap;
	AgentConn conn;
} AgentClient;

static struct {
	int enabled;
	int listening;
	char bindaddr[64];
	int port;     /* raw line-JSON port (0 = off) */
	int mcp_port; /* native MCP (HTTP) port (0 = off) */
	char token[768]; /* fits any 255-wchar ini value as UTF-8 */
	int require_token;
	int allow_send; /* ini */
	int send_armed; /* runtime */

	SOCKET listen_sock;     /* raw */
	SOCKET mcp_listen_sock; /* MCP */
	HWND wnd;
	WNDPROC old_wnd_proc;

	AgentClient clients[AGENT_MAX_CLIENTS];
	AgentBackend backend;
	char *resp; /* shared response scratch (single-threaded) */
	int wsa_started;

	/* multi-session shared segment */
	HANDLE shm_map;
	AgentShmem *shm;
	int slot;   /* our session slot, -1 if none */
	int broker; /* we own the listener(s) */
	DWORD last_maint_tick;
} A;

static AgentSession *my_session(void)
{
	return (A.shm != NULL && A.slot >= 0) ? &A.shm->sessions[A.slot] : NULL;
}

/* ---- session id + slot resolution ---- */

static uint64_t self_hwnd(void)
{
	return (uint64_t)(uintptr_t)HVTWin;
}

/* NULL/empty session => our own window; else look up by hwnd-hex id. */
static int resolve_slot(const char *session)
{
	if (A.shm == NULL) {
		return -1;
	}
	if (session == NULL || session[0] == 0) {
		return A.slot;
	}
	uint64_t h = (uint64_t)_strtoui64(session, NULL, 16);
	return agent_shm_find(A.shm, h);
}

/* ---- receive tap (runs on the main thread via CommRead1Byte) ---- */

static void AgentServerFeed(BYTE b)
{
	AgentSession *s = my_session();
	if (s != NULL) {
		agent_shm_ring_append(s, &b, 1);
	}
}

/* ---- backend over the shared roster + rings ---- */

static int send_allowed(void)
{
	return A.allow_send && A.send_armed;
}

/* Execute a send on this process's own connection. */
static int apply_send_local(uint8_t op, const void *data, size_t len)
{
	if (!cv.Ready) {
		return AGENT_ERR_NOTCONN;
	}
	if (op == AGENT_CMD_TEXT) {
		int wn = MultiByteToWideChar(CP_UTF8, 0, (const char *)data, (int)len, NULL, 0);
		if (wn <= 0) {
			return 0;
		}
		wchar_t *w = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
		if (w == NULL) {
			return 0;
		}
		MultiByteToWideChar(CP_UTF8, 0, (const char *)data, (int)len, w, wn);
		CommTextOutW(&cv, w, wn);
		free(w);
		return (int)len;
	}
	return CommBinaryOut(&cv, (PCHAR)data, (int)len);
}

static int be_check_token(void *ctx, const char *token)
{
	(void)ctx;
	return token != NULL && agent_ct_eq(token, A.token);
}

/* Copy a shared-memory string field written concurrently by another process.
 * Bounded to srccap so a torn write that momentarily lacks a NUL can't make us
 * read past the field into adjacent shmem; result is always NUL-terminated. */
static void copy_shm_str(char *dst, size_t dstcap, const char *src, size_t srccap)
{
	size_t n = dstcap - 1;
	if (n > srccap - 1) {
		n = srccap - 1;
	}
	size_t i = 0;
	for (; i < n && src[i] != 0; i++) {
		dst[i] = src[i];
	}
	dst[i] = 0;
}

static int be_get_status(void *ctx, const char *session, AgentStatus *out)
{
	(void)ctx;
	memset(out, 0, sizeof(*out));
	int slot = resolve_slot(session);
	if (slot < 0) {
		return AGENT_ERR_NOSESSION;
	}
	AgentSession *s = &A.shm->sessions[slot];
	out->ready = s->ready;
	out->port_type = s->port_type;
	out->offset = agent_shm_ring_total(s);
	copy_shm_str(out->host, sizeof(out->host), s->host, sizeof(s->host));
	if (slot == A.slot) {
		out->cols = ts.TerminalWidth;
		out->rows = ts.TerminalHeight;
		out->xfer_active = ProtoGetProtoFlag() ? 1 : 0;
		out->xfer_last_result = ProtoGetLastResult();
	} else {
		/* Another window's transfer state is not visible from this process. */
		out->xfer_last_result = -1;
	}
	return 0;
}

static int be_list_sessions(void *ctx, AgentSessionInfo *out, int max)
{
	(void)ctx;
	if (A.shm == NULL) {
		return 0;
	}
	int n = 0;
	for (int i = 0; i < AGENT_MAX_SESSIONS && n < max; i++) {
		AgentSession *s = &A.shm->sessions[i];
		if (!s->in_use) {
			continue;
		}
		memset(&out[n], 0, sizeof(out[n]));
		snprintf(out[n].session, sizeof(out[n].session), "%016llx", (unsigned long long)s->hwnd);
		copy_shm_str(out[n].host, sizeof(out[n].host), s->host, sizeof(s->host));
		copy_shm_str(out[n].title, sizeof(out[n].title), s->title, sizeof(s->title));
		out[n].port_type = s->port_type;
		out[n].ready = s->ready;
		n++;
	}
	return n;
}

static int be_read_since(void *ctx, const char *session, uint64_t since, void *out, size_t max,
						 uint64_t *from, int *gap)
{
	(void)ctx;
	int slot = resolve_slot(session);
	if (slot < 0) {
		return AGENT_ERR_NOSESSION;
	}
	return (int)agent_shm_ring_read(&A.shm->sessions[slot], since, out, max, from, gap);
}

static int be_read_scrollback(void *ctx, const char *session, void *out, size_t max, uint64_t *from)
{
	(void)ctx;
	int slot = resolve_slot(session);
	if (slot < 0) {
		return AGENT_ERR_NOSESSION;
	}
	AgentSession *s = &A.shm->sessions[slot];
	uint64_t total = agent_shm_ring_total(s);
	uint64_t oldest = agent_shm_ring_oldest(s);
	uint64_t since = oldest;
	if (total - oldest > (uint64_t)max) {
		since = total - (uint64_t)max;
	}
	int gap = 0;
	return (int)agent_shm_ring_read(s, since, out, max, from, &gap);
}

static int be_send_common(const char *session, uint8_t op, const void *data, size_t len)
{
	int slot = resolve_slot(session);
	if (slot < 0) {
		return AGENT_ERR_NOSESSION;
	}
	AgentSession *s = &A.shm->sessions[slot];
	if (!s->ready) {
		return AGENT_ERR_NOTCONN;
	}
	if (!s->send_armed) {
		return AGENT_ERR_NOTALLOWED;
	}
	if (slot == A.slot) {
		return apply_send_local(op, data, len); /* our own window: send now */
	}
	/* another window: hand off; it drains and applies on its idle loop */
	if (agent_shm_cmd_push(s, op, data, len) != 0) {
		return -100; /* queue full -> "send failed" */
	}
	return (int)len;
}

static int be_send_text(void *ctx, const char *session, const char *utf8, size_t len)
{
	(void)ctx;
	return be_send_common(session, AGENT_CMD_TEXT, utf8, len);
}

static int be_send_bytes(void *ctx, const char *session, const void *data, size_t len)
{
	(void)ctx;
	return be_send_common(session, AGENT_CMD_BINARY, data, len);
}

static int be_zmodem_send(void *ctx, const char *session, const char *pathU8, int binary)
{
	(void)ctx;
	int slot = resolve_slot(session);
	if (slot < 0) {
		return AGENT_ERR_NOSESSION;
	}
	AgentSession *s = &A.shm->sessions[slot];
	if (!s->ready) {
		return AGENT_ERR_NOTCONN;
	}
	if (!s->send_armed) {
		return AGENT_ERR_NOTALLOWED;
	}
	/* Local window only. ZMODEMStartSend arms the protocol pump on this GUI
	 * thread against this process's cv/FileVar, and the transfer's outcome is
	 * observable only through this window's own status. A foreign window's
	 * transfer would be neither driven nor visible here, so cross-window ZMODEM
	 * is deferred (send_bytes/text still hand off via the shm queue). */
	if (slot != A.slot) {
		return AGENT_ERR_NOTALLOWED;
	}
	wchar_t *w = ToWcharU8(pathU8);
	if (w == NULL) {
		return AGENT_ERR_BUSY;
	}
	/* FALSE start => a transfer is already in flight (FileVar != NULL) or a
	 * resource failure; both surface to the caller as "busy". A bad path is
	 * NOT caught here -- the send starts, then fails via ProtoEnd, which the
	 * caller observes as transfer.state=done, ok=false. */
	BOOL ok = ZMODEMStartSend(w, binary ? 1 : 0, FALSE);
	free(w);
	return ok ? 0 : AGENT_ERR_BUSY;
}

static void init_backend(void)
{
	memset(&A.backend, 0, sizeof(A.backend));
	A.backend.ctx = NULL;
	A.backend.require_token = A.require_token;
	A.backend.check_token = be_check_token;
	A.backend.get_status = be_get_status;
	A.backend.list_sessions = be_list_sessions;
	A.backend.read_since = be_read_since;
	A.backend.read_scrollback = be_read_scrollback;
	A.backend.send_text = be_send_text;
	A.backend.send_bytes = be_send_bytes;
	A.backend.zmodem_send = be_zmodem_send;
}

/* ---- client bookkeeping ---- */

static AgentClient *find_client(SOCKET s)
{
	for (int i = 0; i < AGENT_MAX_CLIENTS; i++) {
		if (A.clients[i].sock == s) {
			return &A.clients[i];
		}
	}
	return NULL;
}

static AgentClient *alloc_client(SOCKET s)
{
	for (int i = 0; i < AGENT_MAX_CLIENTS; i++) {
		if (A.clients[i].sock == INVALID_SOCKET) {
			AgentClient *c = &A.clients[i];
			memset(c, 0, sizeof(*c));
			c->sock = s;
			return c;
		}
	}
	return NULL;
}

static void close_client(AgentClient *c)
{
	if (c->sock != INVALID_SOCKET) {
		closesocket(c->sock);
	}
	if (c->mcp != NULL) {
		agent_mcp_conn_free(c->mcp);
	}
	free(c->rxbuf);
	free(c->txbuf);
	memset(c, 0, sizeof(*c));
	c->sock = INVALID_SOCKET;
}

/* Append to the client's outbound buffer and try to flush. */
static int queue_send(AgentClient *c, const char *data, size_t len)
{
	if (c->txlen + len > c->txcap) {
		size_t need = c->txlen + len;
		char *nb = (char *)realloc(c->txbuf, need);
		if (nb == NULL) {
			return -1;
		}
		c->txbuf = nb;
		c->txcap = need;
	}
	memcpy(c->txbuf + c->txlen, data, len);
	c->txlen += len;
	return 0;
}

static void flush_client(AgentClient *c)
{
	while (c->txoff < c->txlen) {
		int r = send(c->sock, c->txbuf + c->txoff, (int)(c->txlen - c->txoff), 0);
		if (r > 0) {
			c->txoff += (size_t)r;
		}
		else if (r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
			return; /* FD_WRITE will re-notify */
		}
		else {
			close_client(c);
			return;
		}
	}
	c->txoff = 0;
	c->txlen = 0;
}

/* Process one complete request line. */
static void handle_line(AgentClient *c, const char *line, size_t len)
{
	size_t n = agent_dispatch(&A.backend, &c->conn, line, len, A.resp, AGENT_RESP_MAX);
	if (n > 0) {
		if (queue_send(c, A.resp, n) != 0 || queue_send(c, "\n", 1) != 0) {
			close_client(c);
			return;
		}
		flush_client(c);
	}
	if (c->conn.should_close && c->sock != INVALID_SOCKET) {
		flush_client(c);
		if (c->sock != INVALID_SOCKET) {
			close_client(c);
		}
	}
}

/* Drive an MCP (HTTP) client: feed received bytes, queue HTTP responses. */
static void on_readable_mcp(AgentClient *c)
{
	char tmp[8192];
	int r = recv(c->sock, tmp, sizeof(tmp), 0);
	if (r == 0) {
		close_client(c);
		return;
	}
	if (r == SOCKET_ERROR) {
		if (WSAGetLastError() == WSAEWOULDBLOCK) {
			return;
		}
		close_client(c);
		return;
	}

	const char *data = tmp;
	size_t len = (size_t)r;
	for (;;) {
		int close_after = 0;
		size_t n = agent_mcp_feed(c->mcp, data, len, A.resp, AGENT_RESP_MAX, &close_after);
		data = NULL; /* subsequent iterations drain the buffer */
		len = 0;
		if (n == 0) {
			break; /* need more data */
		}
		if (queue_send(c, A.resp, n) != 0) {
			close_client(c);
			return;
		}
		if (close_after) {
			flush_client(c);
			if (c->sock != INVALID_SOCKET) {
				close_client(c);
			}
			return;
		}
	}
	flush_client(c);
}

static void on_readable(AgentClient *c)
{
	if (c->is_mcp) {
		on_readable_mcp(c);
		return;
	}

	char tmp[8192];
	int r = recv(c->sock, tmp, sizeof(tmp), 0);
	if (r == 0) {
		close_client(c);
		return;
	}
	if (r == SOCKET_ERROR) {
		if (WSAGetLastError() == WSAEWOULDBLOCK) {
			return;
		}
		close_client(c);
		return;
	}

	/* Append and split on newline. */
	if (c->rxlen + (size_t)r > c->rxcap) {
		size_t need = c->rxlen + (size_t)r;
		if (need > AGENT_RXLINE_MAX) {
			close_client(c);
			return;
		}
		char *nb = (char *)realloc(c->rxbuf, need);
		if (nb == NULL) {
			close_client(c);
			return;
		}
		c->rxbuf = nb;
		c->rxcap = need;
	}
	memcpy(c->rxbuf + c->rxlen, tmp, (size_t)r);
	c->rxlen += (size_t)r;

	size_t start = 0;
	for (size_t i = 0; i < c->rxlen; i++) {
		if (c->rxbuf[i] == '\n') {
			size_t linelen = i - start;
			/* trim a trailing CR */
			if (linelen > 0 && c->rxbuf[start + linelen - 1] == '\r') {
				linelen--;
			}
			if (linelen > 0) {
				handle_line(c, c->rxbuf + start, linelen);
			}
			if (c->sock == INVALID_SOCKET) {
				return; /* client closed during handling */
			}
			start = i + 1;
		}
	}
	if (start > 0) {
		memmove(c->rxbuf, c->rxbuf + start, c->rxlen - start);
		c->rxlen -= start;
	}
}

/* ---- socket event window ---- */

static void on_accept(SOCKET listen_sock, int is_mcp)
{
	struct sockaddr_storage addr;
	int addrlen = sizeof(addr);
	SOCKET s = accept(listen_sock, (struct sockaddr *)&addr, &addrlen);
	if (s == INVALID_SOCKET) {
		return;
	}
	AgentClient *c = alloc_client(s);
	if (c == NULL) {
		closesocket(s); /* too many clients */
		return;
	}
	c->is_mcp = is_mcp;
	if (is_mcp) {
		c->mcp = agent_mcp_conn_new(&A.backend, A.token, A.bindaddr);
		if (c->mcp == NULL) {
			close_client(c);
			return;
		}
	}
	WSAAsyncSelect(s, A.wnd, WM_AGENT_IO, FD_READ | FD_WRITE | FD_CLOSE);
}

static LRESULT CALLBACK agent_wnd_proc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_AGENT_ACCEPT) {
		if (HIWORD(lParam) == 0 && LOWORD(lParam) == FD_ACCEPT) {
			SOCKET ls = (SOCKET)wParam;
			on_accept(ls, ls == A.mcp_listen_sock);
		}
		return 0;
	}
	if (msg == WM_AGENT_IO) {
		AgentClient *c = find_client((SOCKET)wParam);
		if (c == NULL) {
			return 0;
		}
		if (HIWORD(lParam) != 0) {
			close_client(c);
			return 0;
		}
		switch (LOWORD(lParam)) {
		case FD_READ:
			on_readable(c);
			break;
		case FD_WRITE:
			flush_client(c);
			break;
		case FD_CLOSE:
			on_readable(c); /* drain any final bytes */
			if (c->sock != INVALID_SOCKET) {
				close_client(c);
			}
			break;
		}
		return 0;
	}
	return CallWindowProc(A.old_wnd_proc, wnd, msg, wParam, lParam);
}

static HWND make_wnd(void)
{
	HWND w = CreateWindow("STATIC", "TeraTerm Agent", WS_DISABLED | WS_POPUP, 0, 0, 1, 1, NULL, NULL,
						  GetModuleHandle(NULL), NULL);
	if (w == NULL) {
		return NULL;
	}
	A.old_wnd_proc = (WNDPROC)SetWindowLongPtr(w, GWLP_WNDPROC, (LONG_PTR)agent_wnd_proc);
	return w;
}

/* ---- config ---- */

static void read_config(void)
{
	const wchar_t *ini = ts.SetupFNameW;
	wchar_t wbuf[256];

	A.enabled = 0;
	GetPrivateProfileStringW(L"Agent", L"Enable", L"off", wbuf, _countof(wbuf), ini);
	A.enabled = (_wcsicmp(wbuf, L"on") == 0);

	GetPrivateProfileStringW(L"Agent", L"BindAddress", L"127.0.0.1", wbuf, _countof(wbuf), ini);
	WideCharToMultiByte(CP_ACP, 0, wbuf, -1, A.bindaddr, sizeof(A.bindaddr), NULL, NULL);

	A.port = GetPrivateProfileIntW(L"Agent", L"Port", 0, ini);
	A.mcp_port = GetPrivateProfileIntW(L"Agent", L"McpPort", 0, ini);

	GetPrivateProfileStringW(L"Agent", L"Token", L"", wbuf, _countof(wbuf), ini);
	WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, A.token, sizeof(A.token), NULL, NULL);
	A.require_token = (A.token[0] != 0);

	GetPrivateProfileStringW(L"Agent", L"AllowSend", L"off", wbuf, _countof(wbuf), ini);
	A.allow_send = (_wcsicmp(wbuf, L"on") == 0);
}

static int bind_is_loopback(void)
{
	return strncmp(A.bindaddr, "127.", 4) == 0 || strcmp(A.bindaddr, "::1") == 0;
}

/* Blank Token when the agent starts => generate one and persist it to the ini,
 * so the shipped default config gets real auth with zero typing. Token=none
 * opts out of auth explicitly (e.g. security delegated to an SSH tunnel), but
 * only on a loopback bind -- an open port with no auth is refused outright.
 * The named mutex serializes generate-if-blank across windows; without it two
 * first-launch windows could persist different tokens and the broker would
 * keep authenticating against one the ini no longer holds. */
static int ensure_token(void)
{
	if (_stricmp(A.token, "none") == 0) {
		if (!bind_is_loopback()) {
			return -1;
		}
		A.token[0] = 0;
		A.require_token = 0;
		return 0;
	}
	if (A.token[0] != 0) {
		return 0;
	}

	HANDLE mtx = CreateMutexW(NULL, FALSE, L"TeraTermAgentTokenV1");
	if (mtx != NULL) {
		WaitForSingleObject(mtx, 5000);
	}
	wchar_t wbuf[256];
	GetPrivateProfileStringW(L"Agent", L"Token", L"", wbuf, _countof(wbuf), ts.SetupFNameW);
	if (wbuf[0] == 0) {
		unsigned char rnd[16];
		if (RtlGenRandom(rnd, sizeof(rnd))) {
			static const wchar_t hexdig[] = L"0123456789abcdef";
			wchar_t tok[2 * sizeof(rnd) + 1];
			for (size_t i = 0; i < sizeof(rnd); i++) {
				tok[i * 2] = hexdig[rnd[i] >> 4];
				tok[i * 2 + 1] = hexdig[rnd[i] & 0xf];
			}
			tok[2 * sizeof(rnd)] = 0;
			if (WritePrivateProfileStringW(L"Agent", L"Token", tok, ts.SetupFNameW)) {
				wcscpy_s(wbuf, _countof(wbuf), tok);
			}
		}
	}
	if (mtx != NULL) {
		ReleaseMutex(mtx);
		CloseHandle(mtx);
	}
	WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, A.token, sizeof(A.token), NULL, NULL);
	A.require_token = (A.token[0] != 0);
	return A.require_token ? 0 : -1;
}

/* ---- shared segment + roster ---- */

/* Build an owner-only security descriptor for the shared segment. NOTE: this
 * does NOT stop a same-user process from opening the mapping -- every process
 * the user runs carries the user's SID, and Windows cannot wall a session-named
 * object off from same-user code (see the security note in ttagent-README.md).
 * It only drops the incidental SYSTEM / Administrators access the default DACL
 * would grant. Returns a LocalAlloc'd descriptor (LocalFree it), or NULL to fall
 * back to the default DACL. */
static PSECURITY_DESCRIPTOR build_owner_sd(void)
{
	HANDLE tok = NULL;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
		return NULL;
	}
	DWORD sz = 0;
	GetTokenInformation(tok, TokenUser, NULL, 0, &sz);
	TOKEN_USER *tu = (TOKEN_USER *)malloc(sz);
	PSECURITY_DESCRIPTOR sd = NULL;
	char *sidstr = NULL;
	if (tu != NULL && GetTokenInformation(tok, TokenUser, tu, sz, &sz) &&
		ConvertSidToStringSidA(tu->User.Sid, &sidstr)) {
		char sddl[256];
		/* D:P = protected DACL (no inheritance); grant all access only to the user SID. */
		snprintf(sddl, sizeof(sddl), "D:P(A;;GA;;;%s)", sidstr);
		ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, NULL);
	}
	if (sidstr != NULL) {
		LocalFree(sidstr);
	}
	free(tu);
	CloseHandle(tok);
	return sd;
}

static int shm_open(void)
{
	SECURITY_ATTRIBUTES sa;
	PSECURITY_DESCRIPTOR sd = build_owner_sd();
	LPSECURITY_ATTRIBUTES psa = NULL;
	if (sd != NULL) {
		sa.nLength = sizeof(sa);
		sa.lpSecurityDescriptor = sd;
		sa.bInheritHandle = FALSE;
		psa = &sa;
	}
	A.shm_map = CreateFileMappingA(INVALID_HANDLE_VALUE, psa, PAGE_READWRITE, 0,
								   (DWORD)sizeof(AgentShmem), AGENT_SHM_NAME);
	if (sd != NULL) {
		LocalFree(sd);
	}
	if (A.shm_map == NULL) {
		return -1;
	}
	BOOL first = (GetLastError() != ERROR_ALREADY_EXISTS);
	A.shm = (AgentShmem *)MapViewOfFile(A.shm_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AgentShmem));
	if (A.shm == NULL) {
		CloseHandle(A.shm_map);
		A.shm_map = NULL;
		return -1;
	}
	if (first) {
		memset(A.shm, 0, sizeof(AgentShmem));
		A.shm->magic = AGENT_SHM_MAGIC;
	}
	return 0;
}

/* Publish this window's live status into its slot (called on idle). */
static void publish_status(void)
{
	AgentSession *s = my_session();
	if (s == NULL) {
		return;
	}
	s->ready = cv.Ready ? 1 : 0;
	s->port_type = cv.PortType;
	s->send_armed = send_allowed() ? 1 : 0;
	snprintf(s->host, sizeof(s->host), "%s", ts.HostName);
	GetWindowTextA(HVTWin, s->title, sizeof(s->title));
}

/* Apply any sends other windows queued for us. */
static void drain_commands(void)
{
	AgentSession *s = my_session();
	if (s == NULL) {
		return;
	}
	static char buf[AGENT_SHM_CMDQ];
	uint8_t op;
	int n;
	while ((n = agent_shm_cmd_pop(s, &op, buf, sizeof(buf))) >= 0) {
		if (send_allowed()) {
			apply_send_local(op, buf, (size_t)n);
		}
	}
}

/* ---- lifecycle ---- */

static SOCKET start_listener(int port)
{
	struct addrinfo hints, *res = NULL;
	char portstr[16];
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;
	snprintf(portstr, sizeof(portstr), "%d", port);
	if (getaddrinfo(A.bindaddr, portstr, &hints, &res) != 0 || res == NULL) {
		return INVALID_SOCKET;
	}

	SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (s == INVALID_SOCKET) {
		freeaddrinfo(res);
		return INVALID_SOCKET;
	}
	BOOL excl = TRUE;
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof(excl));

	if (bind(s, res->ai_addr, (int)res->ai_addrlen) == SOCKET_ERROR) {
		freeaddrinfo(res);
		closesocket(s);
		return INVALID_SOCKET;
	}
	freeaddrinfo(res);

	if (WSAAsyncSelect(s, A.wnd, WM_AGENT_ACCEPT, FD_ACCEPT) == SOCKET_ERROR ||
		listen(s, SOMAXCONN) == SOCKET_ERROR) {
		closesocket(s);
		return INVALID_SOCKET;
	}
	return s;
}

static int other_session_exists(void)
{
	if (A.shm == NULL) {
		return 0;
	}
	for (int i = 0; i < AGENT_MAX_SESSIONS; i++) {
		if (i != A.slot && A.shm->sessions[i].in_use) {
			return 1;
		}
	}
	return 0;
}

/* Try to grab the well-known port(s). The single winner is the broker that
 * serves every window's session; the others just maintain their own slot. */
static void try_become_broker(void)
{
	if (A.broker || A.wnd == NULL) {
		return;
	}
	SOCKET raw = INVALID_SOCKET, mcp = INVALID_SOCKET;
	if (A.port > 0) {
		raw = start_listener(A.port);
		if (raw == INVALID_SOCKET) {
			return;
		}
	}
	if (A.mcp_port > 0) {
		mcp = start_listener(A.mcp_port);
		if (mcp == INVALID_SOCKET) {
			if (raw != INVALID_SOCKET) {
				closesocket(raw);
			}
			return;
		}
	}
	A.listen_sock = raw;
	A.mcp_listen_sock = mcp;
	A.broker = 1;
	A.listening = 1;
}

static AgentStartResult agent_start(int force_enable)
{
	memset(&A, 0, sizeof(A));
	A.listen_sock = INVALID_SOCKET;
	A.mcp_listen_sock = INVALID_SOCKET;
	A.slot = -1;
	for (int i = 0; i < AGENT_MAX_CLIENTS; i++) {
		A.clients[i].sock = INVALID_SOCKET;
	}

	read_config();
	if (force_enable) {
		A.enabled = 1;
	}
	if (A.port <= 0 && A.mcp_port <= 0) {
		A.enabled = 0;
	}
	if (!A.enabled) {
		return AGENT_START_ERR_CONFIG;
	}
	if (ensure_token() != 0) {
		A.enabled = 0;
		return AGENT_START_ERR_CONFIG;
	}
	/* Sending is armed at startup iff the ini opted in; the menu toggle
	 * (AgentServerArmSend) can disarm/rearm within that grant. */
	A.send_armed = A.allow_send;

	WSADATA wsadata;
	if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) {
		AgentServerEnd();
		return AGENT_START_ERR_RESOURCE;
	}
	A.wsa_started = 1;

	if (shm_open() != 0) {
		AgentServerEnd();
		return AGENT_START_ERR_RESOURCE;
	}
	A.slot = agent_shm_claim(A.shm, (uint32_t)GetCurrentProcessId(), self_hwnd());
	if (A.slot < 0) {
		AgentServerEnd(); /* too many sessions */
		return AGENT_START_ERR_RESOURCE;
	}

	A.resp = (char *)malloc(AGENT_RESP_MAX);
	if (A.resp == NULL) {
		AgentServerEnd();
		return AGENT_START_ERR_RESOURCE;
	}

	init_backend();
	A.wnd = make_wnd();
	if (A.wnd == NULL) {
		AgentServerEnd();
		return AGENT_START_ERR_RESOURCE;
	}

	/* Every window taps its own received stream and publishes its status;
	 * exactly one window wins the port and becomes the broker. */
	cv.AgentRecv1 = AgentServerFeed;
	publish_status();
	try_become_broker();
	return AGENT_START_OK;
}

void AgentServerInit(void)
{
	agent_start(0);
}

AgentStartResult AgentServerStart(void)
{
	if (A.enabled) {
		return AGENT_START_OK;
	}
	int prev_armed = A.send_armed; /* runtime arm state left by the previous run */
	AgentStartResult r = agent_start(1);
	if (r != AGENT_START_OK) {
		return r;
	}
	/* Sole window that failed to bind: nobody serves the port, so surface it
	 * instead of sitting enabled-but-dead. With other sessions alive, not
	 * being broker is the normal state and the 1 Hz retry stands. */
	if (!A.broker && !other_session_exists()) {
		AgentServerEnd();
		return AGENT_START_ERR_PORT;
	}
	A.send_armed = prev_armed && A.allow_send;
	publish_status();
	return AGENT_START_OK;
}

void AgentServerIdle(void)
{
	if (!A.enabled || A.shm == NULL) {
		return;
	}
	drain_commands(); /* apply queued sends promptly */

	/* Heavier maintenance at ~1 Hz: refresh status, retry becoming broker. */
	DWORD now = GetTickCount();
	if (now - A.last_maint_tick >= 1000) {
		A.last_maint_tick = now;
		publish_status();
		if (!A.broker && (A.port > 0 || A.mcp_port > 0)) {
			try_become_broker();
		}
	}
}

void AgentServerEnd(void)
{
	cv.AgentRecv1 = NULL;

	for (int i = 0; i < AGENT_MAX_CLIENTS; i++) {
		if (A.clients[i].sock != INVALID_SOCKET) {
			close_client(&A.clients[i]);
		}
	}
	if (A.listen_sock != INVALID_SOCKET) {
		closesocket(A.listen_sock);
		A.listen_sock = INVALID_SOCKET;
	}
	if (A.mcp_listen_sock != INVALID_SOCKET) {
		closesocket(A.mcp_listen_sock);
		A.mcp_listen_sock = INVALID_SOCKET;
	}
	if (A.wnd != NULL) {
		DestroyWindow(A.wnd);
		A.wnd = NULL;
	}
	if (A.shm != NULL) {
		if (A.slot >= 0) {
			agent_shm_release(A.shm, A.slot);
			A.slot = -1;
		}
		UnmapViewOfFile(A.shm);
		A.shm = NULL;
	}
	if (A.shm_map != NULL) {
		CloseHandle(A.shm_map);
		A.shm_map = NULL;
	}
	A.broker = 0;
	free(A.resp);
	A.resp = NULL;
	if (A.wsa_started) {
		WSACleanup();
		A.wsa_started = 0;
	}
	A.listening = 0;
	A.enabled = 0;
}

void AgentServerArmSend(int arm)
{
	A.send_armed = arm ? 1 : 0;
	publish_status();
}

int AgentServerIsEnabled(void)
{
	return A.enabled;
}

int AgentServerIsListening(void)
{
	return A.listening;
}

/* True when the ini permits sending, so the runtime arm toggle is meaningful. */
int AgentServerCanArm(void)
{
	return A.enabled && A.allow_send;
}

/* Title-bar indicator: " [agent:off]" / " [agent]" / " [agent:send]". */
const wchar_t *AgentServerTitleTagW(void)
{
	if (!A.enabled) {
		return L" [agent:off]";
	}
	return send_allowed() ? L" [agent:send]" : L" [agent]";
}

int AgentServerIsSendArmed(void)
{
	return A.send_armed;
}
