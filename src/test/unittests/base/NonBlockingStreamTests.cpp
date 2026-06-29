/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "base/NonBlockingStream.h"

#include "test/global/gtest.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)

namespace {

class Pipe {
public:
    Pipe()
    {
        EXPECT_EQ(0, pipe(m_fds));
    }

    ~Pipe()
    {
        if (m_fds[0] >= 0) {
            close(m_fds[0]);
        }
        if (m_fds[1] >= 0) {
            close(m_fds[1]);
        }
    }

    int readFd() const { return m_fds[0]; }
    int writeFd() const { return m_fds[1]; }

    void closeWrite()
    {
        if (m_fds[1] >= 0) {
            close(m_fds[1]);
            m_fds[1] = -1;
        }
    }

private:
    int m_fds[2] = {-1, -1};
};

}

TEST(NonBlockingStreamTests, readsSingleCharacterFromPipe)
{
    Pipe pipe;
    NonBlockingStream stream(pipe.readFd());

    const char shutdown = 'S';
    ASSERT_EQ(1, write(pipe.writeFd(), &shutdown, 1));

    char ch = '\0';
    EXPECT_TRUE(stream.try_read_char(ch));
    EXPECT_EQ(shutdown, ch);
}

TEST(NonBlockingStreamTests, returnsFalseWhenPipeHasNoData)
{
    Pipe pipe;
    NonBlockingStream stream(pipe.readFd());

    char ch = '\0';
    EXPECT_FALSE(stream.try_read_char(ch));
}

TEST(NonBlockingStreamTests, returnsFalseOnClosedPipeWithoutAsserting)
{
    Pipe pipe;
    NonBlockingStream stream(pipe.readFd());
    pipe.closeWrite();

    char ch = '\0';
    EXPECT_FALSE(stream.try_read_char(ch));
}

TEST(NonBlockingStreamTests, restoresOriginalFileStatusFlags)
{
    Pipe pipe;
    const int initialFlags = fcntl(pipe.readFd(), F_GETFL);
    ASSERT_NE(-1, initialFlags);

    {
        NonBlockingStream stream(pipe.readFd());
        const int activeFlags = fcntl(pipe.readFd(), F_GETFL);
        ASSERT_NE(-1, activeFlags);
        EXPECT_NE(0, activeFlags & O_NONBLOCK);
    }

    const int restoredFlags = fcntl(pipe.readFd(), F_GETFL);
    ASSERT_NE(-1, restoredFlags);
    EXPECT_EQ(initialFlags, restoredFlags);
}

#endif
