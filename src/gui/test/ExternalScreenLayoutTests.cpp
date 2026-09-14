#include "../src/ExternalScreenLayout.h"
#include <gtest/gtest.h>
#include <QStringList>

namespace {
const QString screens = "section: screens\n ubuntu:\n windows:\n macbook:\nend\n";
}
TEST(ExternalScreenLayoutTests, ResolvesActualLeftCenterRightWithoutNameInference) {
    const auto r = ExternalScreenLayout::parse(screens +
        "section: links\n ubuntu:\n left = windows\n right = macbook\n"
        " windows:\n right = ubuntu\n macbook:\n left = ubuntu\nend\n");
    ASSERT_TRUE(r.trustworthy);
    ASSERT_EQ(3, r.devices.size());
    EXPECT_EQ(QString("ubuntu"), r.devices[0].name);
    EXPECT_EQ(0, r.devices[0].x); EXPECT_EQ(0, r.devices[0].y);
    EXPECT_EQ(-1, r.devices[1].x); EXPECT_EQ(0, r.devices[1].y);
    EXPECT_EQ(1, r.devices[2].x); EXPECT_EQ(0, r.devices[2].y);
}
TEST(ExternalScreenLayoutTests, RejectsAmbiguousGraphsButKeepsNames) {
    const QStringList links = {
        "ubuntu:\n left(0,50) = windows\n right = macbook",
        "ubuntu:\n left = windows\n left = macbook",
        "ubuntu:\n left = missing\n right = macbook",
        "ubuntu:\n right = windows\n windows:\n right = ubuntu",
        "ubuntu:\n right = windows\n macbook:\n right = windows",
        "ubuntu:\n right = windows",
        "ubuntu:\n sideways = windows",
        "missing:\n right = ubuntu"
    };
    for (const auto& link : links) {
        const auto r = ExternalScreenLayout::parse(screens + "section: links\n" + link + "\nend\n");
        EXPECT_FALSE(r.trustworthy) << link.toStdString();
        ASSERT_EQ(3, r.devices.size());
        EXPECT_EQ(QString("macbook"), r.devices[2].name);
    }
}
TEST(ExternalScreenLayoutTests, ReadsCommentsAndVerticalOneWayLinks) {
    const auto r = ExternalScreenLayout::parse(screens +
        "section: options\n heartbeat = 5000\nend\nsection: links # comment\r\n"
        "ubuntu:\n up = windows # above\n down = macbook\nend\n");
    ASSERT_TRUE(r.trustworthy);
    EXPECT_EQ(-1, r.devices[1].y); EXPECT_EQ(1, r.devices[2].y);
}
TEST(ExternalScreenLayoutTests, RefusesTruncatedDuplicateAndOversizedInput) {
    EXPECT_FALSE(ExternalScreenLayout::parse(screens + "section: links\nubuntu:\nright = windows").trustworthy);
    EXPECT_FALSE(ExternalScreenLayout::parse("section: screens\na:\na:\nend\n").trustworthy);
    EXPECT_FALSE(ExternalScreenLayout::parse(QString(ExternalScreenLayout::maximumCharacters + 1, 'x')).trustworthy);
    QString many = "section: screens\n";
    for (int i = 0; i <= ExternalScreenLayout::maximumScreens; ++i) many += QString("host%1:\n").arg(i);
    const auto r = ExternalScreenLayout::parse(many + "end\n");
    EXPECT_FALSE(r.trustworthy);
    EXPECT_LE(r.devices.size(), ExternalScreenLayout::maximumScreens);
    EXPECT_FALSE(ExternalScreenLayout::parse(QString()).trustworthy);
}
