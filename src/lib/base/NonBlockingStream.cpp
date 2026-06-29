/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2008 Debauchee Open Source Group
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

#if !defined(_WIN32)

#include "base/NonBlockingStream.h"

#include <unistd.h> // tcgetattr/tcsetattr, read
#include <termios.h> // tcgetattr/tcsetattr
#include <fcntl.h>
#include <errno.h>

NonBlockingStream::NonBlockingStream(int fd) :
    _fd(fd),
    _restore_terminal(false),
    _p_ta_previous(nullptr),
    _cntl_previous(-1)
{
    if (isatty(fd)) {
        // Disable ICANON & ECHO so terminal input can be polled one byte at a time.
        termios ta;
        if (tcgetattr(fd, &ta) == 0) {
            _p_ta_previous = new termios(ta);
            ta.c_lflag &= ~(ICANON | ECHO);
            _restore_terminal = (tcsetattr(fd, TCSANOW, &ta) == 0);
        }
    }

    // prevent IO from blocking so we can poll (read())
    _cntl_previous = fcntl(fd, F_GETFL);
    if (_cntl_previous != -1) {
        fcntl(fd, F_SETFL, _cntl_previous | O_NONBLOCK);
    }
}

NonBlockingStream::~NonBlockingStream()
{
    if (_restore_terminal && _p_ta_previous != nullptr) {
        tcsetattr(_fd, TCSANOW, _p_ta_previous);
    }
    if (_cntl_previous != -1) {
        fcntl(_fd, F_SETFL, _cntl_previous);
    }
    delete _p_ta_previous;
}

bool NonBlockingStream::try_read_char(char &ch) const
{
    const ssize_t result = read(_fd, &ch, 1);
    if (result == 1) {
        return true;
    }
    if (result == 0) {
        return false;
    }
    if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
        return false;
    }
    return false;
}

#endif // !defined(_WIN32)
