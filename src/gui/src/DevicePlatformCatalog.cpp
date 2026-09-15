#include "DevicePlatformCatalog.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSysInfo>
#include <cstdint>
#include <utility>
#include <fcntl.h>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
constexpr qint64 MaxBytes=65536;

bool boundedRead(const QString& path,QByteArray& bytes) {
    if(path.isEmpty() || path.contains(QChar(0))) return false;
    qint64 size=0; int fd=-1;
#ifdef Q_OS_WIN
    HANDLE handle=CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),GENERIC_READ,
        FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(handle==INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(handle,&info) ||
        (info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nFileSizeHigh || info.nFileSizeLow>MaxBytes) {
        CloseHandle(handle); return false;
    }
    size=info.nFileSizeLow;
    fd=_open_osfhandle(reinterpret_cast<intptr_t>(handle),_O_RDONLY|_O_BINARY);
    if(fd<0) { CloseHandle(handle); return false; }
#else
    fd=open(QFile::encodeName(path).constData(),O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(fd<0) return false;
    struct stat info{};
    if(fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_size<0 || info.st_size>MaxBytes) {
        close(fd); return false;
    }
    size=info.st_size;
#endif
    QFile file;
    if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)) {
#ifdef Q_OS_WIN
        _close(fd);
#else
        close(fd);
#endif
        return false;
    }
    bytes=file.read(MaxBytes+1);
    return file.error()==QFileDevice::NoError && bytes.size()==size && file.atEnd();
}

bool validName(const QString& name) {
    if(name.isEmpty() || name.trimmed().isEmpty()) return false;
    for(int i=0;i<name.size();++i) {
        const auto c=name.at(i);
        if(c.unicode()<0x20 || c.unicode()==0x7f || c.isLowSurrogate()) return false;
        if(c.isHighSurrogate() && (++i==name.size() || !name.at(i).isLowSurrogate())) return false;
    }
    return name.toUtf8().size()<=256;
}

// QJsonObject discards repeated keys. Count members in the already validated,
// strictly two-level schema so duplicate IDs cannot silently replace metadata.
bool uniqueMembers(const QByteArray& bytes,int devices) {
    int depth=0,rootCount=0,deviceCount=0;
    bool quoted=false,escaped=false;
    for(char c:bytes) {
        if(quoted) {
            if(escaped) escaped=false;
            else if(c=='\\') escaped=true;
            else if(c=='"') quoted=false;
        } else if(c=='"') quoted=true;
        else if(c=='{') ++depth;
        else if(c=='}') --depth;
        else if(c==':') { if(depth==1) ++rootCount; if(depth==2) ++deviceCount; }
    }
    return rootCount==2 && deviceCount==devices;
}
}

bool DevicePlatformCatalog::load(const QString& path) {
    m_devices.clear();
    QByteArray bytes;
    if(!boundedRead(path,bytes) || QString::fromUtf8(bytes).toUtf8()!=bytes) return false;
    QJsonParseError error;
    const auto document=QJsonDocument::fromJson(bytes,&error);
    if(error.error!=QJsonParseError::NoError || !document.isObject()) return false;
    const auto root=document.object();
    if(root.size()!=2 || !root.value("version").isDouble() || root.value("version").toDouble()!=1 ||
        !root.value("devices").isObject()) return false;
    const auto devices=root.value("devices").toObject();
    if(devices.size()>64 || !uniqueMembers(bytes,devices.size())) return false;
    QHash<QString,QString> parsed;
    for(auto i=devices.constBegin();i!=devices.constEnd();++i) {
        if(!validName(i.key()) || !i.value().isString()) return false;
        const auto platform=i.value().toString();
        if(platform!="ubuntu" && platform!="windows" && platform!="macos") return false;
        parsed.insert(i.key(),platform);
    }
    m_devices=std::move(parsed);
    return true;
}

QString DevicePlatformCatalog::resolve(const QString& name,bool local) const {
    return local ? localPlatform() : m_devices.value(name,QStringLiteral("unknown"));
}

QString DevicePlatformCatalog::normalizeProductType(const QString& product) {
    if(product=="ubuntu" || product=="windows") return product;
    if(product=="osx" || product=="macos") return QStringLiteral("macos");
    return QStringLiteral("unknown");
}

QString DevicePlatformCatalog::localPlatform() {
    return normalizeProductType(QSysInfo::productType());
}
