#pragma once
#include <algorithm>
#include <string_view>

namespace VitaStartup {
// Keep every byte offset and newline stable for upstream save compatibility.
inline bool omitProjectLogo(char *buffer, size_t length) {
    const std::string_view text(buffer, length);
    const auto boot = text.find("\n*boot_logo\n");
    if (boot == text.npos) return false;
    const auto end = text.find("\n*boot_logo_skip\n", boot);
    const auto first = text.find("lsp 100,project_logo,0,0", boot);
    const auto last = text.find(";Done with logos", first);
    if (end == text.npos || first == text.npos || last == text.npos || last >= end) return false;
    // Never erase an unexpected label or a different logo sequence.
    if (text.substr(first, last - first).find("\n*") != text.npos) return false;
    for (size_t i = first; i < last; ++i)
        if (buffer[i] != '\n' && buffer[i] != '\r') buffer[i] = ' ';
    return true;
}
}
