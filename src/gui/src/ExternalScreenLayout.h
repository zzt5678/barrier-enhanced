#pragma once

#include <QString>
#include <QVector>

// Read-only visualization, not a replacement for the server's config parser.
namespace ExternalScreenLayout {
struct Device { QString name; int x = 0; int y = 0; };
struct Result {
    QVector<Device> devices;
    bool trustworthy = false;
};
constexpr int maximumCharacters = 256 * 1024;
constexpr int maximumScreens = 64;
Result parse(const QString& contents);
}
