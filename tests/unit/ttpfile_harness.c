/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Host-lane driver for the file-transfer protocol state machines. Builds a fake
 * TFileIO (scriptable clean-EOF vs error-shaped reads) and a fake TComm (scripted
 * RX, captured TX), then runs a real protocol session end to end so the pure
 * protocol logic is exercised without Win32. See ttpfile_harness.h for the API.
 */
#include <stddef.h>
#include <string.h>
#include <assert.h>

#include "tttypes.h"
#include "filesys_proto.h"
#include "protolog.h"
#include "xmodem.h"

#include "ttpfile_harness.h"

/* --- link stubs: symbols the protocol TUs reference but that live in Win32 /
 *     GUI translation units excluded from this lane. None are on a path the
 *     protocol logic under test reaches. --- */
static unsigned long g_tick;
unsigned long GetTickCount(void) { return g_tick += 100; }
void ProtoEnd(void) {}
TProtoLog *ProtoLogCreate(void) { return NULL; }

/* --- scriptable fake TFileIO --- */
typedef struct {
	const unsigned char *content;
	size_t len;
	size_t pos;
	size_t fail_at;     /* (size_t)-1 = never; else an error-0 read once pos reaches here */
	int io_error_flag;  /* records that the error path fired (the bit a fixed impl would expose) */
} FakeFile;

static BOOL ff_open_read(TFileIO *fv, const char *n) { FakeFile *f = (FakeFile *)fv->data; (void)n; f->pos = 0; return TRUE; }
static BOOL ff_open_write(TFileIO *fv, const char *n) { (void)fv; (void)n; return TRUE; }
static FioStatus ff_read(TFileIO *fv, void *buf, size_t bytes, size_t *read_bytes)
{
	FakeFile *f = (FakeFile *)fv->data;
	if (f->pos >= f->fail_at) {
		f->io_error_flag = 1;
		*read_bytes = 0;
		return FIO_ERROR;  /* models a failed Win32 ReadFile, now distinct from EOF */
	}
	size_t n = f->len - f->pos;
	if (n == 0) {
		*read_bytes = 0;
		return FIO_EOF;
	}
	if (n > bytes) {
		n = bytes;
	}
	memcpy(buf, f->content + f->pos, n);
	f->pos += n;
	*read_bytes = n;
	return FIO_OK;
}
static size_t ff_write(TFileIO *fv, const void *buf, size_t bytes) { (void)fv; (void)buf; return bytes; }
static void ff_close(TFileIO *fv) { (void)fv; }
static int ff_seek(TFileIO *fv, size_t off) { FakeFile *f = (FakeFile *)fv->data; f->pos = off; return 0; }
static size_t ff_fsize(TFileIO *fv, const char *n) { FakeFile *f = (FakeFile *)fv->data; (void)n; return f->len; }
static char *ff_send_name(TFileIO *fv, const char *n, BOOL u8, BOOL sp, BOOL up) { (void)fv; (void)u8; (void)sp; (void)up; return _strdup(n); }
static void ff_destroy(TFileIO *fv) { (void)fv; }

/* --- fake TComm: scripted RX queue, captured TX buffer --- */
typedef struct {
	unsigned char rx[4096];
	int rx_head, rx_tail;
	unsigned char tx[TTP_TX_MAX];
	int tx_len;
} FakeComm;

static int fc_out(TComm *c, const CHAR *buf, size_t len)
{
	FakeComm *f = (FakeComm *)c->private_data;
	memcpy(f->tx + f->tx_len, buf, len);
	f->tx_len += (int)len;
	return (int)len;
}
static int fc_read1(TComm *c, BYTE *b)
{
	FakeComm *f = (FakeComm *)c->private_data;
	if (f->rx_head == f->rx_tail) {
		return 0;
	}
	*b = f->rx[f->rx_head++];
	return 1;
}
static void fc_insert1(TComm *c, BYTE b) { (void)c; (void)b; }
static void fc_flush(TComm *c) { FakeComm *f = (FakeComm *)c->private_data; f->rx_head = f->rx_tail = 0; }
static const CommOp FC_OP = { fc_out, fc_read1, fc_insert1, fc_flush };

/* --- fake services / InfoOp (all no-ops: progress UI is out of scope here) --- */
static char *fv_next_fname(PFileVarProto fv) { (void)fv; return _strdup("probe.bin"); }
static void fv_set_timeout(PFileVarProto fv, int t) { (void)fv; (void)t; }
static void io_init_prog(PFileVarProto fv, int *p) { (void)fv; (void)p; }
static void io_time(PFileVarProto fv, DWORD e, int b) { (void)fv; (void)e; (void)b; }
static void io_pkt(PFileVarProto fv, LONG n) { (void)fv; (void)n; }
static void io_bytes(PFileVarProto fv, LONG n) { (void)fv; (void)n; }
static void io_pct(PFileVarProto fv, LONG a, LONG b, int *p) { (void)fv; (void)a; (void)b; (void)p; }
static void io_text(PFileVarProto fv, const char *t) { (void)fv; (void)t; }
static void io_fname(PFileVarProto fv, const char *t) { (void)fv; (void)t; }
static const TInfoOp INFO_OP = { io_init_prog, io_time, io_pkt, io_bytes, io_pct, io_text, io_fname };

#define XM_NAK 0x15
#define XM_ACK 0x06

void ttp_xmodem_send(size_t file_len, size_t fail_at, TtpResult *out)
{
	static unsigned char content[4096];
	size_t i;
	for (i = 0; i < sizeof(content); i++) {
		content[i] = (unsigned char)(i * 7 + 1);
	}

	memset(out, 0, sizeof(*out));

	FakeFile ff;
	memset(&ff, 0, sizeof(ff));
	ff.content = content;
	ff.len = file_len;
	ff.fail_at = fail_at;

	TFileIO file;
	memset(&file, 0, sizeof(file));
	file.OpenRead = ff_open_read;
	file.OpenWrite = ff_open_write;
	file.ReadFile = ff_read;
	file.WriteFile = ff_write;
	file.Close = ff_close;
	file.Seek = ff_seek;
	file.FileSysDestroy = ff_destroy;
	file.GetFSize = ff_fsize;
	file.GetSendFilename = ff_send_name;
	file.data = &ff;

	FakeComm fc;
	memset(&fc, 0, sizeof(fc));
	TComm comm;
	comm.op = &FC_OP;
	comm.private_data = &fc;

	TFileVarProto fv;
	memset(&fv, 0, sizeof(fv));
	fv.GetNextFname = fv_next_fname;
	fv.FTSetTimeOut = fv_set_timeout;
	fv.InfoOp = &INFO_OP;
	fv.Comm = &comm;
	fv.file_fv = &file;

	TTTSet ts;
	memset(&ts, 0, sizeof(ts));
	ts.XmodemTimeOutInit = 10;
	ts.XmodemTimeOutInitCRC = 3;
	ts.XmodemTimeOutShort = 10;
	ts.XmodemTimeOutLong = 20;
	ts.XmodemTimeOutVLong = 40;

	TComVar cv;
	memset(&cv, 0, sizeof(cv));
	cv.Ready = TRUE;

	TProto *proto = XCreate(&fv);
	fv.Proto = proto;
	proto->Op->SetOpt(proto, XMODEM_MODE, IdXSend);
	proto->Op->SetOpt(proto, XMODEM_OPT, XoptCheck);
	if (!proto->Op->Init(proto, &cv, &ts)) {
		out->rc = -1;
		proto->Op->Destroy(proto);
		return;
	}

	int guard = 0;
	while (proto->Op->Parse(proto)) {
		if (++guard > 64) {
			out->rc = -2;
			proto->Op->Destroy(proto);
			return;
		}
		if (fc.rx_head == fc.rx_tail) {
			/* NAK kicks off the send, then ACK each packet */
			fc.rx[fc.rx_tail++] = (unsigned char)(fc.tx_len == 0 ? XM_NAK : XM_ACK);
		}
	}

	out->rc = 0;
	memcpy(out->tx, fc.tx, fc.tx_len);
	out->tx_len = fc.tx_len;
	out->success = fv.Success;
	out->io_error = ff.io_error_flag;
	proto->Op->Destroy(proto);
}
