/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 */

#include "agent_shmem.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
/* Claim in_use 0 -> 1 atomically. Returns non-zero if this call won it. */
static int claim_flag(volatile int32_t *p)
{
	return InterlockedCompareExchange((volatile LONG *)p, 1, 0) == 0;
}
/* Atomic 64-bit load/store so a cross-process reader never tears ring_total
 * (matters on 32-bit builds, where a naked uint64 access is two words). */
static uint64_t load64(volatile uint64_t *p)
{
	return (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)p, 0, 0);
}
static void store64(volatile uint64_t *p, uint64_t v)
{
	InterlockedExchange64((volatile LONG64 *)p, (LONG64)v);
}
/* Same fenced load/store for the 32-bit queue cursors, so cmd[] writes are
 * visible before cmd_head/cmd_tail on any arch (not just x86 store order). */
static uint32_t load32(volatile uint32_t *p)
{
	return (uint32_t)InterlockedCompareExchange((volatile LONG *)p, 0, 0);
}
static void store32(volatile uint32_t *p, uint32_t v)
{
	InterlockedExchange((volatile LONG *)p, (LONG)v);
}
#else
/* Host tests are single-threaded. */
static int claim_flag(volatile int32_t *p)
{
	if (*p == 0) {
		*p = 1;
		return 1;
	}
	return 0;
}
static uint64_t load64(volatile uint64_t *p) { return *p; }
static void store64(volatile uint64_t *p, uint64_t v) { *p = v; }
static uint32_t load32(volatile uint32_t *p) { return *p; }
static void store32(volatile uint32_t *p, uint32_t v) { *p = v; }
#endif

int agent_shm_claim(AgentShmem *m, uint32_t pid, uint64_t hwnd)
{
	for (int i = 0; i < AGENT_MAX_SESSIONS; i++) {
		AgentSession *s = &m->sessions[i];
		if (claim_flag(&s->in_use)) {
			s->pid = pid;
			s->hwnd = hwnd;
			s->ready = 0;
			s->send_armed = 0;
			s->port_type = 0;
			s->host[0] = 0;
			s->title[0] = 0;
			s->ring_total = 0;
			s->cmd_head = 0;
			s->cmd_tail = 0;
			return i;
		}
	}
	return -1;
}

void agent_shm_release(AgentShmem *m, int slot)
{
	if (slot < 0 || slot >= AGENT_MAX_SESSIONS)
		return;
	AgentSession *s = &m->sessions[slot];
	s->hwnd = 0;
	s->ready = 0;
	s->in_use = 0;
}

int agent_shm_find(const AgentShmem *m, uint64_t hwnd)
{
	for (int i = 0; i < AGENT_MAX_SESSIONS; i++) {
		if (m->sessions[i].in_use && m->sessions[i].hwnd == hwnd)
			return i;
	}
	return -1;
}

/* ---- ring (head == ring_total % cap) ---- */

void agent_shm_ring_append(AgentSession *s, const void *data, size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	const size_t cap = AGENT_SHM_RING;
	uint64_t total = load64(&s->ring_total);

	if (len >= cap) {
		p += len - cap; /* keep only the last cap bytes */
		/* Arrange so offset O lands at ring[O % cap]; the surviving span
		 * [total-cap, total) begins at the new head = total % cap. */
		size_t newhead = (size_t)((total + len) % cap);
		size_t first = cap - newhead;
		memcpy(s->ring + newhead, p, first);
		memcpy(s->ring, p + first, cap - first);
		store64(&s->ring_total, total + len); /* publish after the bytes */
		return;
	}

	size_t head = (size_t)(total % cap);
	size_t first = cap - head;
	if (first >= len) {
		memcpy(s->ring + head, p, len);
	}
	else {
		memcpy(s->ring + head, p, first);
		memcpy(s->ring, p + first, len - first);
	}
	store64(&s->ring_total, total + len); /* publish after the bytes */
}

uint64_t agent_shm_ring_total(const AgentSession *s)
{
	return load64((volatile uint64_t *)&s->ring_total);
}

uint64_t agent_shm_ring_oldest(const AgentSession *s)
{
	uint64_t total = load64((volatile uint64_t *)&s->ring_total);
	uint64_t retained = (total < AGENT_SHM_RING) ? total : (uint64_t)AGENT_SHM_RING;
	return total - retained;
}

uint64_t agent_shm_ring_overwrite_drop(uint64_t since, uint64_t total_after, size_t cap)
{
	uint64_t new_oldest = (total_after < (uint64_t)cap) ? 0 : total_after - (uint64_t)cap;
	return (new_oldest > since) ? (new_oldest - since) : 0;
}

size_t agent_shm_ring_read(const AgentSession *s, uint64_t since, void *out, size_t max, uint64_t *from,
						   int *gap)
{
	const size_t cap = AGENT_SHM_RING;
	uint64_t total = load64((volatile uint64_t *)&s->ring_total); /* single snapshot */
	uint64_t retained = (total < cap) ? total : (uint64_t)cap;
	uint64_t oldest = total - retained;
	int had_gap = 0;

	if (since < oldest) {
		since = oldest;
		had_gap = 1;
	}
	if (since > total)
		since = total;

	uint64_t available = total - since;
	size_t n = (available < (uint64_t)max) ? (size_t)available : max;

	if (n > 0) {
		size_t dist = (size_t)(total - since);
		size_t idx = (size_t)((total % cap) + cap - dist) % cap;
		unsigned char *dst = (unsigned char *)out;
		size_t first = cap - idx;
		if (first >= n) {
			memcpy(dst, s->ring + idx, n);
		}
		else {
			memcpy(dst, s->ring + idx, first);
			memcpy(dst + first, s->ring, n - first);
		}
	}

	/* SPSC: a fast cross-process writer can overtake the region we just copied.
	 * Re-read ring_total and drop any prefix that was overwritten during the
	 * copy, flagging a gap -- so the caller never gets torn bytes silently. */
	uint64_t total_after = load64((volatile uint64_t *)&s->ring_total);
	uint64_t drop = agent_shm_ring_overwrite_drop(since, total_after, cap);
	if (drop > 0) {
		had_gap = 1;
		if (drop >= n) {
			n = 0;
		}
		else {
			memmove(out, (unsigned char *)out + drop, n - (size_t)drop);
			n -= (size_t)drop;
		}
		since += drop;
	}

	if (from != NULL)
		*from = since;
	if (gap != NULL)
		*gap = had_gap;
	return n;
}

/* ---- command queue (SPSC byte ring, framed [op][len:4][payload]) ---- */

static uint32_t cmdq_used(const AgentSession *s)
{
	uint32_t head = load32((volatile uint32_t *)&s->cmd_head);
	uint32_t tail = load32((volatile uint32_t *)&s->cmd_tail);
	return (head - tail) & (AGENT_SHM_CMDQ - 1);
}

static void cmdq_write(AgentSession *s, uint32_t *pos, const void *data, size_t len)
{
	const unsigned char *p = (const unsigned char *)data;
	uint32_t at = *pos;
	for (size_t i = 0; i < len; i++) {
		s->cmd[at] = p[i];
		at = (at + 1) & (AGENT_SHM_CMDQ - 1);
	}
	*pos = at;
}

static void cmdq_read(const AgentSession *s, uint32_t *pos, void *out, size_t len)
{
	unsigned char *o = (unsigned char *)out;
	uint32_t at = *pos;
	for (size_t i = 0; i < len; i++) {
		o[i] = s->cmd[at];
		at = (at + 1) & (AGENT_SHM_CMDQ - 1);
	}
	*pos = at;
}

int agent_shm_cmd_push(AgentSession *s, uint8_t op, const void *data, size_t len)
{
	size_t frame = 1 + 4 + len;
	/* one slot is kept free to distinguish full from empty */
	uint32_t free_bytes = (AGENT_SHM_CMDQ - 1) - cmdq_used(s);
	if (frame > free_bytes)
		return -1;

	uint32_t head = s->cmd_head;
	unsigned char hdr[5];
	hdr[0] = op;
	uint32_t l = (uint32_t)len;
	hdr[1] = (unsigned char)(l & 0xff);
	hdr[2] = (unsigned char)((l >> 8) & 0xff);
	hdr[3] = (unsigned char)((l >> 16) & 0xff);
	hdr[4] = (unsigned char)((l >> 24) & 0xff);
	cmdq_write(s, &head, hdr, 5);
	cmdq_write(s, &head, data, len);
	store32(&s->cmd_head, head); /* publish after the frame is written */
	return 0;
}

int agent_shm_cmd_pop(AgentSession *s, uint8_t *op, void *buf, size_t bufcap)
{
	if (cmdq_used(s) < 5)
		return -1;

	uint32_t tail = s->cmd_tail;
	unsigned char hdr[5];
	cmdq_read(s, &tail, hdr, 5);
	uint32_t len = (uint32_t)hdr[1] | ((uint32_t)hdr[2] << 8) | ((uint32_t)hdr[3] << 16) |
				   ((uint32_t)hdr[4] << 24);

	/* consume the frame regardless of whether it fits, so the queue advances */
	if (op != NULL)
		*op = hdr[0];
	if (len > bufcap) {
		/* skip payload */
		for (uint32_t i = 0; i < len; i++)
			tail = (tail + 1) & (AGENT_SHM_CMDQ - 1);
		store32(&s->cmd_tail, tail);
		return -1;
	}
	cmdq_read(s, &tail, buf, len);
	store32(&s->cmd_tail, tail); /* publish */
	return (int)len;
}
