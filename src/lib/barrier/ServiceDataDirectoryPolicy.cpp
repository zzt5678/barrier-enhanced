/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/ServiceDataDirectoryPolicy.h"

namespace ServiceDataDirectoryPolicy {

const char* protectedDirectorySddl()
{
    return "O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
}

bool acceptsDirectoryObject(bool exists, bool directory, bool reparsePoint)
{
    return exists && directory && !reparsePoint;
}

} // namespace ServiceDataDirectoryPolicy
