/*
    barrier -- mouse and keyboard sharing utility
    Copyright (C) Barrier contributors

    This package is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    found in the file LICENSE that should have accompanied this file.

    This package is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "encoding_utilities.h"
#include <stringapiset.h>

#include <limits>

std::string win_wchar_to_utf8(const WCHAR* utfStr)
{
    if (utfStr == NULL) {
        return std::string();
    }

    const int utfLength = lstrlenW(utfStr);
    if (utfLength == 0) {
        return std::string();
    }

    const int mbLength = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, utfStr, utfLength,
        NULL, 0, NULL, NULL);
    if (mbLength <= 0) {
        return std::string();
    }

    std::string mbStr(static_cast<std::size_t>(mbLength), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, utfStr, utfLength,
            &mbStr[0], mbLength, NULL, NULL) != mbLength) {
        return std::string();
    }
    return mbStr;
}

std::vector<WCHAR> utf8_to_win_char(const std::string& str)
{
    std::vector<WCHAR> result(1u, L'\0');
    if (str.empty()) {
        return result;
    }
    if (str.find('\0') != std::string::npos ||
        str.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return result;
    }

    const int inputLength = static_cast<int>(str.size());
    const int resultLength = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, str.data(), inputLength, NULL, 0);
    if (resultLength <= 0) {
        return result;
    }

    result.assign(static_cast<std::size_t>(resultLength) + 1u, L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, str.data(), inputLength,
            result.data(), resultLength) != resultLength) {
        result.assign(1u, L'\0');
    }
    return result;
}
