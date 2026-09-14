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

#include "barrier/win32/DaemonApp.h"

#include <iostream>

#ifdef SYSAPI_UNIX

int
main(int argc, char** argv)
{
    DaemonApp app;
    return app.run(argc, argv);
}

#elif SYSAPI_WIN32

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "common/Version.h"
#include "barrier/protocol_types.h"
#include <cstdio>
#include <cstring>

int WINAPI
WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    // Diagnostics must not initialize service state or display an error dialog.
    for (int index = 1; index < __argc; ++index) {
        if (std::strcmp(__argv[index], "--version") == 0) {
            std::printf("Weave %s\n", kBuildId);
            std::printf("Protocol version %d.%d\n",
                        static_cast<int>(kProtocolMajorVersion),
                        static_cast<int>(kProtocolMinorVersion));
            return 0;
        }
    }
    DaemonApp app;
    return app.run(__argc, __argv);
}

#endif
