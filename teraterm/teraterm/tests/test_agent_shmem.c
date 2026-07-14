/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 *
 * Host unit test for agent_shmem (pure logic; no Win32).
 *   cc -I.. -o t test_agent_shmem.c ../agent_shmem.c && ./t
 */

#include "agent_shmem.h"

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

static void test_claim_find_release(void)
{
	AgentShmem *m = (AgentShmem *)calloc(1, sizeof(*m));

	int a = agent_shm_claim(m, 100, 0xAAAA);
	int b = agent_shm_claim(m, 101, 0xBBBB);
	CHECK(a == 0);
	CHECK(b == 1);
	CHECK(agent_shm_find(m, 0xAAAA) == 0);
	CHECK(agent_shm_find(m, 0xBBBB) == 1);
	CHECK(agent_shm_find(m, 0xCCCC) == -1);

	agent_shm_release(m, a);
	CHECK(agent_shm_find(m, 0xAAAA) == -1);
	/* freed slot is reused */
	int c = agent_shm_claim(m, 102, 0xCCCC);
	CHECK(c == 0);
	CHECK(agent_shm_find(m, 0xCCCC) == 0);

	free(m);
}

static void test_claim_full(void)
{
	AgentShmem *m = (AgentShmem *)calloc(1, sizeof(*m));
	for (int i = 0; i < AGENT_MAX_SESSIONS; i++)
		CHECK(agent_shm_claim(m, (uint32_t)i, 0x1000 + i) == i);
	CHECK(agent_shm_claim(m, 999, 0x9999) == -1);
	free(m);
}

static void test_ring(void)
{
	AgentShmem *m = (AgentShmem *)calloc(1, sizeof(*m));
	int s = agent_shm_claim(m, 1, 1);
	AgentSession *sess = &m->sessions[s];

	agent_shm_ring_append(sess, "hello", 5);
	CHECK(agent_shm_ring_total(sess) == 5);
	CHECK(agent_shm_ring_oldest(sess) == 0);

	char out[64];
	uint64_t from = 9;
	int gap = 9;
	size_t n = agent_shm_ring_read(sess, 0, out, sizeof(out), &from, &gap);
	CHECK(n == 5 && from == 0 && gap == 0 && memcmp(out, "hello", 5) == 0);

	agent_shm_ring_append(sess, "world", 5);
	n = agent_shm_ring_read(sess, 5, out, sizeof(out), &from, &gap);
	CHECK(n == 5 && from == 5 && memcmp(out, "world", 5) == 0);

	free(m);
}

static void test_ring_wrap_gap(void)
{
	AgentShmem *m = (AgentShmem *)calloc(1, sizeof(*m));
	AgentSession *sess = &m->sessions[agent_shm_claim(m, 1, 1)];

	/* Fill past capacity so the oldest bytes drop. */
	unsigned char *big = (unsigned char *)malloc(AGENT_SHM_RING + 100);
	for (size_t i = 0; i < AGENT_SHM_RING + 100; i++)
		big[i] = (unsigned char)(i & 0xff);
	agent_shm_ring_append(sess, big, AGENT_SHM_RING + 100);

	CHECK(agent_shm_ring_total(sess) == AGENT_SHM_RING + 100);
	CHECK(agent_shm_ring_oldest(sess) == 100);

	char out[64];
	uint64_t from = 0;
	int gap = 0;
	size_t n = agent_shm_ring_read(sess, 0, out, sizeof(out), &from, &gap);
	CHECK(gap == 1);
	CHECK(from == 100);            /* oldest surviving offset */
	CHECK(n == sizeof(out));
	CHECK((unsigned char)out[0] == (unsigned char)(100 & 0xff));

	free(big);
	free(m);
}

static void test_cmdq(void)
{
	AgentShmem *m = (AgentShmem *)calloc(1, sizeof(*m));
	AgentSession *sess = &m->sessions[agent_shm_claim(m, 1, 1)];

	CHECK(agent_shm_cmd_push(sess, AGENT_CMD_TEXT, "ls\r", 3) == 0);
	CHECK(agent_shm_cmd_push(sess, AGENT_CMD_BINARY, "\x03", 1) == 0);

	uint8_t op = 99;
	char buf[64];
	int n = agent_shm_cmd_pop(sess, &op, buf, sizeof(buf));
	CHECK(n == 3 && op == AGENT_CMD_TEXT && memcmp(buf, "ls\r", 3) == 0);
	n = agent_shm_cmd_pop(sess, &op, buf, sizeof(buf));
	CHECK(n == 1 && op == AGENT_CMD_BINARY && (unsigned char)buf[0] == 0x03);
	/* empty */
	CHECK(agent_shm_cmd_pop(sess, &op, buf, sizeof(buf)) == -1);

	free(m);
}

static void test_cmdq_wrap_and_full(void)
{
	AgentShmem *m = (AgentShmem *)calloc(1, sizeof(*m));
	AgentSession *sess = &m->sessions[agent_shm_claim(m, 1, 1)];

	/* Cycle many push/pop pairs to force wraparound of head/tail. */
	char payload[100];
	memset(payload, 'x', sizeof(payload));
	for (int round = 0; round < 1000; round++) {
		CHECK(agent_shm_cmd_push(sess, AGENT_CMD_BINARY, payload, sizeof(payload)) == 0);
		uint8_t op;
		char buf[128];
		int n = agent_shm_cmd_pop(sess, &op, buf, sizeof(buf));
		CHECK(n == (int)sizeof(payload));
		CHECK(memcmp(buf, payload, sizeof(payload)) == 0);
	}

	/* Fill until full; push must eventually reject without corrupting. */
	int pushed = 0;
	while (agent_shm_cmd_push(sess, AGENT_CMD_BINARY, payload, sizeof(payload)) == 0)
		pushed++;
	CHECK(pushed > 0);
	/* Everything pushed can be popped back intact. */
	int popped = 0;
	uint8_t op;
	char buf[128];
	while (agent_shm_cmd_pop(sess, &op, buf, sizeof(buf)) == (int)sizeof(payload))
		popped++;
	CHECK(popped == pushed);

	free(m);
}

static void test_cmd_payload_too_big(void)
{
	AgentShmem *m = (AgentShmem *)calloc(1, sizeof(*m));
	AgentSession *sess = &m->sessions[agent_shm_claim(m, 1, 1)];
	char payload[64];
	memset(payload, 'y', sizeof(payload));
	CHECK(agent_shm_cmd_push(sess, AGENT_CMD_BINARY, payload, sizeof(payload)) == 0);
	uint8_t op;
	char small[10];
	/* payload (64) exceeds bufcap (10): frame dropped, returns -1 */
	CHECK(agent_shm_cmd_pop(sess, &op, small, sizeof(small)) == -1);
	/* queue is now empty (frame consumed/dropped) */
	char big[128];
	CHECK(agent_shm_cmd_pop(sess, &op, big, sizeof(big)) == -1);
	free(m);
}

static void test_ring_overwrite_drop(void)
{
	const uint64_t cap = AGENT_SHM_RING;
	/* writer hasn't wrapped yet (total_after < cap): nothing was overwritten */
	CHECK(agent_shm_ring_overwrite_drop(1000, 2000, cap) == 0);
	CHECK(agent_shm_ring_overwrite_drop(0, cap, cap) == 0);
	/* writer advanced but our read start stays inside the retained window */
	CHECK(agent_shm_ring_overwrite_drop(20000, cap + 12000, cap) == 0);
	/* writer overtook the oldest bytes we copied -> drop that prefix */
	CHECK(agent_shm_ring_overwrite_drop(10000, cap + 12000, cap) == 2000);
	/* writer lapped the whole ring past our start -> everything dropped */
	CHECK(agent_shm_ring_overwrite_drop(100, 3 * cap, cap) == 2 * cap - 100);
}

int main(void)
{
	test_claim_find_release();
	test_claim_full();
	test_ring();
	test_ring_wrap_gap();
	test_cmdq();
	test_cmdq_wrap_and_full();
	test_cmd_payload_too_big();
	test_ring_overwrite_drop();

	if (failures == 0) {
		printf("all agent_shmem tests passed\n");
		return 0;
	}
	printf("%d agent_shmem assertion(s) failed\n", failures);
	return 1;
}
