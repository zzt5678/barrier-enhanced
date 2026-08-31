#include "test/global/gtest.h"

#include "platform/XWindowsEventQueueBuffer.h"

TEST(XWindowsEventQueueBufferTests, pollTimeoutForTest_capsWaitToRemainingDeadline)
{
	EXPECT_EQ(25, XWindowsEventQueueBuffer::pollTimeoutForTest(-1));
	EXPECT_EQ(25, XWindowsEventQueueBuffer::pollTimeoutForTest(100));
	EXPECT_EQ(25, XWindowsEventQueueBuffer::pollTimeoutForTest(25));
	EXPECT_EQ(4, XWindowsEventQueueBuffer::pollTimeoutForTest(4));
}
