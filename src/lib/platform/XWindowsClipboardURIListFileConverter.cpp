/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier Contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "platform/XWindowsClipboardURIListFileConverter.h"

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

std::string trimLine(std::string line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                             line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }
    return line;
}

std::string encodeFileUriPath(const std::string& path)
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

std::string uriListToPathList(const std::string& uriList)
{
    std::istringstream lines(uriList);
    std::string line;
    std::string paths;
    while (std::getline(lines, line)) {
        line = trimLine(line);
        if (line.empty() || line[0] == '#') {
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

        if (!paths.empty()) {
            paths.push_back('\n');
        }
        paths += percentDecode(path);
    }
    return paths;
}

std::string gnomeCopiedFilesToPathList(const std::string& data)
{
    std::istringstream lines(data);
    std::string line;
    std::string uriList;
    bool firstLine = true;
    while (std::getline(lines, line)) {
        line = trimLine(line);
        if (firstLine && (line == "copy" || line == "cut")) {
            firstLine = false;
            continue;
        }
        firstLine = false;
        uriList += line;
        uriList += '\n';
    }
    return uriListToPathList(uriList);
}

std::string pathListToUriList(const std::string& pathList)
{
    std::istringstream lines(pathList);
    std::string line;
    std::string uris;
    while (std::getline(lines, line)) {
        line = trimLine(line);
        if (line.empty() || line[0] != '/') {
            continue;
        }
        uris += "file://" + encodeFileUriPath(line) + "\r\n";
    }
    return uris;
}

std::string pathListToGnomeCopiedFiles(const std::string& pathList)
{
    const std::string uris = pathListToUriList(pathList);
    if (uris.empty()) {
        return "";
    }
    return "copy\n" + uris;
}

}

XWindowsClipboardURIListFileConverter::XWindowsClipboardURIListFileConverter(
    Display* display,
    const char* atomName,
    bool gnomeSpecial) :
    m_atom(XInternAtom(display, atomName, False)),
    m_gnomeSpecial(gnomeSpecial)
{
}

XWindowsClipboardURIListFileConverter::~XWindowsClipboardURIListFileConverter()
{
}

IClipboard::EFormat
XWindowsClipboardURIListFileConverter::getFormat() const
{
    return IClipboard::kText;
}

Atom
XWindowsClipboardURIListFileConverter::getAtom() const
{
    return m_atom;
}

int
XWindowsClipboardURIListFileConverter::getDataSize() const
{
    return 8;
}

std::string
XWindowsClipboardURIListFileConverter::fromIClipboard(const std::string& pathList) const
{
    if (m_gnomeSpecial) {
        return pathListToGnomeCopiedFiles(pathList);
    }
    return pathListToUriList(pathList);
}

std::string
XWindowsClipboardURIListFileConverter::toIClipboard(const std::string& uriList) const
{
    if (m_gnomeSpecial) {
        return gnomeCopiedFilesToPathList(uriList);
    }
    return uriListToPathList(uriList);
}
