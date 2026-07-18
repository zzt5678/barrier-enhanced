/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2012 Nick Bolton
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#define IPC_HOST "127.0.0.1"
#define IPC_PORT 24801

enum EIpcMessage {
    kIpcHello,
    kIpcLogLine,
    kIpcCommand,
    kIpcShutdown,
    kIpcReady,
    kIpcReadyV2,
    kIpcReadyQuery,
    kIpcActivate,
    kIpcActivated,
    kIpcStopRequest,
    kIpcStopAck,
};

enum EIpcClientType {
    kIpcClientUnknown,
    kIpcClientGui,
    kIpcClientNode,
};

// handshake: node/gui -> daemon
// $1 = type, the client identifies itself as gui or node (barrierc/s).
// $2 = client process id, used by the daemon for targeted node shutdown.
extern const char*        kIpcMsgHello;

// ready: node -> daemon
// Sent only after node startup has completed and its IPC event loop is active.
extern const char*        kIpcMsgReady;

// capability ready: node -> daemon
// $1 = process id; $2 = session id; $3/$4 = input generation high/low;
// $5 = input backend ready; $6 = desktop name; $7 = build id;
// $8/$9 = watchdog query nonce high/low, or zero for a periodic lease.
extern const char*        kIpcMsgReadyV2;

// readiness query: daemon -> node
// $1/$2 = nonce high/low. The node responds immediately with IRV2 carrying
// the same nonce so process adoption uses a post-query backend snapshot.
extern const char*        kIpcMsgReadyQuery;

// activate: daemon -> standby node
// $1/$2 = activation nonce high/low. Only the authenticated target process
// receives this after durable ownership commit and old-process fencing.
extern const char*        kIpcMsgActivate;

// activated: node -> daemon
// $1 = process id; $2/$3 = activation nonce high/low. The node sends this
// only after its data plane has started.
extern const char*        kIpcMsgActivated;

// log line: daemon -> gui
// $1 = aggregate log lines collected from barriers/c or the daemon itself.
extern const char*        kIpcMsgLogLine;

// command: gui -> daemon
// $1 = command; the command for the daemon to launch, typically the full
// path to barriers/c. $2 = Windows elevation mode: 0 as-needed, 1 always,
// 2 never.
extern const char*        kIpcMsgCommand;

// shutdown: daemon -> node
// the daemon tells barriers/c to shut down gracefully.
extern const char*        kIpcMsgShutdown;

// stop request: authenticated gui -> daemon
// $1/$2 = non-zero request id high/low.
extern const char*        kIpcMsgStopRequest;

// stop acknowledgement: daemon -> requesting gui
// $1/$2 = request id high/low; $3/$4 = confirmed command generation high/low.
extern const char*        kIpcMsgStopAck;
