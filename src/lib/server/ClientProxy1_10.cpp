/*
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "server/ClientProxy1_10.h"

ClientProxy1_10::ClientProxy1_10(const std::string& name,
                                 barrier::IStream* stream,
                                 Server* server,
                                 IEventQueue* events) :
    ClientProxy1_9(name, stream, server, events)
{
}

ClientProxy1_10::~ClientProxy1_10()
{
}
