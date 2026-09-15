#pragma once

#include <QHash>
#include <QString>

// Display-only inventory. It never determines routing, identity or online state.
class DevicePlatformCatalog {
public:
    // Strict version-1 JSON; any failed reload clears previously loaded entries.
    // Reads once, with a 64 KiB bound and no following of the final symlink.
    bool load(const QString& path);
    // Returns only ubuntu, windows, macos or unknown. Local always uses QSysInfo.
    QString resolve(const QString& canonicalName, bool local) const;
    static QString normalizeProductType(const QString& productType);
    static QString localPlatform();
private:
    QHash<QString, QString> m_devices;
};
