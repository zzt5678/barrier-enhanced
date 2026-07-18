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

#include "ipc/Ipc.h"

const char*                kIpcMsgHello        = "IHEL%1i%4i";
const char*                kIpcMsgReady        = "IRDY";
const char*                kIpcMsgReadyV2      = "IRV2%4i%4i%4i%4i%1i%s%s%4i%4i";
const char*                kIpcMsgReadyQuery   = "IRQP%4i%4i";
const char*                kIpcMsgActivate     = "IACT%4i%4i";
const char*                kIpcMsgActivated    = "IACK%4i%4i%4i";
const char*                kIpcMsgLogLine        = "ILOG%s";
const char*                kIpcMsgCommand        = "ICMD%s%1i";
const char*                kIpcMsgShutdown        = "ISDN";
const char*                kIpcMsgStopRequest     = "ISRP%4i%4i";
const char*                kIpcMsgStopAck         = "ISAK%4i%4i%4i%4i";
