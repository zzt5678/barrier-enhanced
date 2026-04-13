/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2026 Barrier Contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "platform/XWindowsClipboardURIListPNGConverter.h"

#include "base/Log.h"
#include "io/filesystem.h"

#include <cctype>
#include <exception>
#include <fstream>
#include <sstream>

namespace {

const size_t kMaxUriImageBytes = 64 * 1024 * 1024;

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

std::string firstLocalFileFromUriList(const std::string& uriList)
{
    std::istringstream lines(uriList);
    std::string line;

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
            // Remote file URIs are not safe to dereference as local paths.
            continue;
        }

        return percentDecode(path);
    }

    return {};
}

bool looksLikePng(const std::string& data)
{
    // PNG signature (RFC 2083): bytes 0-3 are fixed (0x89, 'P', 'N', 'G').
    // Bytes 4-7 are a line-ending variant: either CRLF (0x0d 0x0a) or LF (0x0a),
    // followed by 0x1a (SUB) and 0x0a (LF). We only need to check the fixed prefix.
    return data.size() >= 4 &&
           static_cast<unsigned char>(data[0]) == 0x89 &&
           data[1] == 'P' &&
           data[2] == 'N' &&
           data[3] == 'G';
}

std::string readPngFile(const std::string& path)
{
    if (path.empty()) {
        return {};
    }

    try {
        const barrier::fs::path imagePath = barrier::fs::u8path(path);
        if (!barrier::fs::exists(imagePath) || !barrier::fs::is_regular_file(imagePath)) {
            LOG((CLOG_DEBUG "uri-list image path is not a regular file: %s", path.c_str()));
            return {};
        }

        const auto fileSize = barrier::fs::file_size(imagePath);
        if (fileSize == 0 || fileSize > kMaxUriImageBytes) {
            LOG((CLOG_WARN "uri-list image file skipped because size is %lu bytes: %s",
                 static_cast<unsigned long>(fileSize), path.c_str()));
            return {};
        }

        std::ifstream file;
        barrier::open_utf8_path(file, imagePath, std::ios::in | std::ios::binary);
        if (!file.is_open()) {
            LOG((CLOG_WARN "failed to open uri-list image file: %s", path.c_str()));
            return {};
        }

        std::string data;
        data.resize(static_cast<size_t>(fileSize));
        file.read(&data[0], static_cast<std::streamsize>(data.size()));
        if (!file || !looksLikePng(data)) {
            LOG((CLOG_WARN "uri-list image file is not a PNG or could not be read: %s", path.c_str()));
            return {};
        }

        return data;
    }
    catch (const std::exception& e) {
        LOG((CLOG_WARN "failed to read uri-list image file %s: %s", path.c_str(), e.what()));
        return {};
    }
}

}

XWindowsClipboardURIListPNGConverter::XWindowsClipboardURIListPNGConverter(Display* display) :
    m_atom(XInternAtom(display, "text/uri-list", False))
{
}

XWindowsClipboardURIListPNGConverter::~XWindowsClipboardURIListPNGConverter()
{
}

IClipboard::EFormat
XWindowsClipboardURIListPNGConverter::getFormat() const
{
    return IClipboard::kPNG;
}

Atom
XWindowsClipboardURIListPNGConverter::getAtom() const
{
    return m_atom;
}

int
XWindowsClipboardURIListPNGConverter::getDataSize() const
{
    return 8;
}

std::string
XWindowsClipboardURIListPNGConverter::fromIClipboard(const std::string&) const
{
    return {};
}

std::string
XWindowsClipboardURIListPNGConverter::toIClipboard(const std::string& uriList) const
{
    const std::string path = firstLocalFileFromUriList(uriList);
    std::string png = readPngFile(path);
    if (!png.empty()) {
        LOG((CLOG_INFO "converted uri-list PNG file to image clipboard payload: %s", path.c_str()));
    }
    return png;
}
