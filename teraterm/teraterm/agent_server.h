/*
 * (C) 2026- TeraTerm Project
 * All rights reserved.
 */

/* Agent control server: local TCP listener that exposes the received stream
 * and lets an agent inject data into the connection, speaking the
 * agent_jsonrpc protocol. Runs entirely on the main message-pump thread
 * (WSAAsyncSelect on a hidden window, mirroring ttxssh fwd.c), so its handlers
 * touch the global cv without cross-thread marshaling.
 *
 * Phase 1: single session (this window). Config in [Agent] of teraterm.ini;
 * default OFF, loopback-only, optional bearer token, send disarmed. */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Read config and, if [Agent] Enable=on and Port>0, start the listener and
 * install the receive tap. Safe to call once at startup. */
void AgentServerInit(void);

/* Stop the listener, close clients, remove the tap. */
void AgentServerEnd(void);

/* Periodic tick from the main idle loop: publishes this window's status, drains
 * sends other windows queued for it, and retries broker election. Cheap. */
void AgentServerIdle(void);

/* Runtime send arming (menu toggle). Sending is allowed only when both the
 * ini AllowSend is on AND this runtime arm is set. */
void AgentServerArmSend(int arm);

int AgentServerIsEnabled(void);
int AgentServerIsListening(void);
int AgentServerIsSendArmed(void);
int AgentServerCanArm(void);

/* Title-bar indicator suffix ("" when disabled). */
const wchar_t *AgentServerTitleTagW(void);

#ifdef __cplusplus
}
#endif
