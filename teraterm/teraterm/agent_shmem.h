/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 */

/* Agent control: cross-process shared segment for multi-session.
 *
 * Every Tera Term window (its own process) claims a slot, streams its received
 * bytes into that slot's ring, and drains its slot's command queue. The window
 * that owns the listener (the broker) reads any slot's ring to serve reads and
 * pushes send commands into a target slot's queue. Data structures and their
 * logic are pure C (host-testable); only the mapping + atomic claim need Win32.
 *
 * Ring/queue are single-producer/single-consumer and lock-free on x86:
 *   - ring: the slot owner is the only producer; readers snapshot ring_total.
 *   - cmd queue: the broker is the only producer; the slot owner the only consumer.
 * A slow reader can still be overtaken by ring wrap -> reported as a gap. */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_MAX_SESSIONS 16
#define AGENT_SHM_RING     (256 * 1024)
#define AGENT_SHM_CMDQ     (32 * 1024)
#define AGENT_SHM_MAGIC    0x54544131 /* 'TTA1' */

#define AGENT_CMD_BINARY 0
#define AGENT_CMD_TEXT   1 /* payload is UTF-8; owner does CommTextOutW */

typedef struct {
	volatile int32_t in_use;
	uint32_t pid;
	uint64_t hwnd; /* window handle == session id */
	int32_t ready;
	int32_t send_armed; /* window will honor injected sends */
	int32_t port_type;
	char host[256];
	char title[256];

	/* received-stream ring: head == ring_total % AGENT_SHM_RING (owner writes) */
	volatile uint64_t ring_total;
	unsigned char ring[AGENT_SHM_RING];

	/* framed command queue [op u8][len u32][payload]; broker writes, owner reads */
	volatile uint32_t cmd_head;
	volatile uint32_t cmd_tail;
	unsigned char cmd[AGENT_SHM_CMDQ];
} AgentSession;

typedef struct {
	int32_t magic;
	AgentSession sessions[AGENT_MAX_SESSIONS];
} AgentShmem;

/* Claim a free slot for (pid, hwnd); returns index [0, MAX) or -1 if full.
 * Atomic across processes on Windows. */
int agent_shm_claim(AgentShmem *m, uint32_t pid, uint64_t hwnd);
void agent_shm_release(AgentShmem *m, int slot);

/* Find an in-use slot by hwnd (session id); returns index or -1. */
int agent_shm_find(const AgentShmem *m, uint64_t hwnd);

/* ---- received-stream ring ---- */
/* How many leading bytes of a read that began at offset `since` a writer that
 * has since advanced ring_total to `total_after` would have overwritten (the
 * valid window now starts at total_after - cap). Pure; exposed for unit tests. */
uint64_t agent_shm_ring_overwrite_drop(uint64_t since, uint64_t total_after, size_t cap);
void agent_shm_ring_append(AgentSession *s, const void *data, size_t len);
uint64_t agent_shm_ring_total(const AgentSession *s);
uint64_t agent_shm_ring_oldest(const AgentSession *s);
size_t agent_shm_ring_read(const AgentSession *s, uint64_t since, void *out, size_t max, uint64_t *from,
						   int *gap);

/* ---- command queue ---- */
/* Enqueue one command; returns 0 on success, -1 if it would not fit. */
int agent_shm_cmd_push(AgentSession *s, uint8_t op, const void *data, size_t len);
/* Dequeue one command into buf; returns payload length and sets *op, or -1 if
 * the queue is empty (nothing removed). If the payload exceeds bufcap the frame
 * is dropped and -1 is returned. */
int agent_shm_cmd_pop(AgentSession *s, uint8_t *op, void *buf, size_t bufcap);

#ifdef __cplusplus
}
#endif
