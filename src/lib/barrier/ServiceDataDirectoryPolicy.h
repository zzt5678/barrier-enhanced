/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Weave contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

namespace ServiceDataDirectoryPolicy {

const char* protectedDirectorySddl();
bool acceptsDirectoryObject(bool exists, bool directory, bool reparsePoint);

} // namespace ServiceDataDirectoryPolicy
