#include "platform/XWindowsClipboardFileListConverter.h"

#include "barrier/RemoteFileClipboard.h"

#include <cctype>
#include <sstream>

namespace {

bool isHex(char c)
{
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 0;
}

std::string percentDecode(const std::string& value)
{
    std::string result;
    result.reserve(value.size());

    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size() &&
            isHex(value[i + 1]) && isHex(value[i + 2])) {
            result.push_back(static_cast<char>((hexValue(value[i + 1]) << 4) |
                                               hexValue(value[i + 2])));
            i += 2;
        }
        else {
            result.push_back(value[i]);
        }
    }

    return result;
}

std::string percentEncodePath(const std::string& path)
{
    static const char hex[] = "0123456789ABCDEF";

    std::string encoded;
    encoded.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(path[i]);
        const bool unreserved =
            (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~' || c == '/';
        if (unreserved) {
            encoded.push_back(static_cast<char>(c));
        }
        else {
            encoded.push_back('%');
            encoded.push_back(hex[c >> 4]);
            encoded.push_back(hex[c & 0x0f]);
        }
    }
    return encoded;
}

std::string trimLine(std::string line)
{
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' ||
            line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

bool parseUriList(const std::string& data,
                  bool gnomeCopiedFiles,
                  RemoteFileClipboard::Data& payload)
{
    payload = RemoteFileClipboard::Data();
    payload.mode = RemoteFileClipboard::Mode::SourcePaths;

    std::istringstream lines(data);
    std::string line;
    bool firstLine = true;
    while (std::getline(lines, line)) {
        line = trimLine(line);
        if (line.empty()) {
            continue;
        }

        if (gnomeCopiedFiles && firstLine) {
            payload.cut = (line == "cut");
            firstLine = false;
            continue;
        }
        firstLine = false;

        if (line[0] == '#') {
            continue;
        }

        const std::string filePrefix = "file://";
        if (line.compare(0, filePrefix.size(), filePrefix) != 0) {
            continue;
        }

        std::string path = line.substr(filePrefix.size());
        const std::string localhostPrefix = "localhost/";
        if (path.compare(0, localhostPrefix.size(), localhostPrefix) == 0) {
            path = path.substr(std::string("localhost").size());
        }
        else if (!path.empty() && path[0] != '/') {
            continue;
        }

        payload.paths.push_back(barrier::fs::u8path(percentDecode(path)));
    }

    return !payload.paths.empty();
}

std::string buildUriList(const RemoteFileClipboard::Data& payload, bool gnomeCopiedFiles)
{
    if (payload.mode != RemoteFileClipboard::Mode::MaterializedPaths ||
        payload.paths.empty()) {
        return std::string();
    }

    std::string output;
    if (gnomeCopiedFiles) {
        output += payload.cut ? "cut\n" : "copy\n";
    }

    for (size_t i = 0; i < payload.paths.size(); ++i) {
        output += "file://";
        output += percentEncodePath(payload.paths[i].generic_u8string());
        output += "\r\n";
    }
    return output;
}

} // namespace

XWindowsClipboardFileListConverter::XWindowsClipboardFileListConverter(Display* display,
                                                                       const char* name,
                                                                       bool gnomeCopiedFiles) :
    m_atom(XInternAtom(display, name, False)),
    m_gnomeCopiedFiles(gnomeCopiedFiles)
{
}

XWindowsClipboardFileListConverter::~XWindowsClipboardFileListConverter()
{
}

IClipboard::EFormat
XWindowsClipboardFileListConverter::getFormat() const
{
    return IClipboard::kFileList;
}

Atom
XWindowsClipboardFileListConverter::getAtom() const
{
    return m_atom;
}

int
XWindowsClipboardFileListConverter::getDataSize() const
{
    return 8;
}

std::string
XWindowsClipboardFileListConverter::fromIClipboard(const std::string& data) const
{
    RemoteFileClipboard::Data payload;
    return RemoteFileClipboard::parse(data, payload)
        ? buildUriList(payload, m_gnomeCopiedFiles)
        : std::string();
}

std::string
XWindowsClipboardFileListConverter::toIClipboard(const std::string& data) const
{
    RemoteFileClipboard::Data payload;
    return parseUriList(data, m_gnomeCopiedFiles, payload)
        ? RemoteFileClipboard::serialize(payload)
        : std::string();
}
