// Hawk OS - string, path and console plumbing.
#include "hawk.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <wincrypt.h>

#include <vector>

namespace hawk {

std::wstring wide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& utf16) {
    if (utf16.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), (int)utf16.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), (int)utf16.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::string protect_secret(const std::string& plaintext) {
    if (plaintext.empty()) return "";
    DATA_BLOB input{};
    input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"Hawk OS API key", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) return "";
    static const char* hex = "0123456789abcdef";
    std::string encoded = "dpapi:v1:";
    encoded.reserve(9 + output.cbData * 2);
    for (DWORD i = 0; i < output.cbData; ++i) {
        encoded.push_back(hex[output.pbData[i] >> 4]);
        encoded.push_back(hex[output.pbData[i] & 15]);
    }
    LocalFree(output.pbData);
    return encoded;
}

std::string unprotect_secret(const std::string& protected_hex) {
    const std::string prefix = "dpapi:v1:";
    if (protected_hex.compare(0, prefix.size(), prefix) != 0) return "";
    std::string hex = protected_hex.substr(prefix.size());
    if (hex.empty() || (hex.size() & 1) || hex.size() > 16384) return "";
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<BYTE> bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return "";
        bytes.push_back(static_cast<BYTE>((hi << 4) | lo));
    }
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), bytes.data()}, output{};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) return "";
    std::string plaintext(reinterpret_cast<char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return plaintext;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring format_wide(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 2047, fmt, ap);
    va_end(ap);
    buf[2047] = L'\0';
    return std::wstring(buf);
}

// Tokenizer that honours double quotes, used by the agent's command language.
std::vector<std::string> split_words(const std::string& s, int max_parts) {
    std::vector<std::string> out;
    size_t i = 0, n = s.size();
    while (i < n) {
        if (max_parts > 0 && (int)out.size() == max_parts - 1) {
            // keep everything that is left as a single argument
            std::string rest = trim(s.substr(i));
            if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"')
                rest = rest.substr(1, rest.size() - 2);
            out.push_back(rest);
            break;
        }
        while (i < n && isspace((unsigned char)s[i])) i++;
        if (i >= n) break;
        std::string tok;
        if (s[i] == '"') {
            i++;
            while (i < n && s[i] != '"') tok.push_back(s[i++]);
            if (i < n) i++;  // closing quote
        } else {
            while (i < n && !isspace((unsigned char)s[i])) tok.push_back(s[i++]);
        }
        out.push_back(tok);
    }
    return out;
}

std::string exe_dir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring path(buf, n);
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return ".";
    return narrow(path.substr(0, slash));
}

std::string app_dir() {
    std::string dir = exe_dir();
    // if the binary lives in a build/bin folder, treat the parent as the app root
    std::string low = to_lower(dir);
    for (const char* sub : {"\\bin", "\\build", "/bin", "/build"}) {
        size_t len = strlen(sub);
        if (low.size() > len && low.compare(low.size() - len, len, sub) == 0)
            return dir.substr(0, dir.size() - len);
    }
    return dir;
}

void attach_console() {
    // A GUI-subsystem binary has no console of its own. When it is launched
    // from a terminal we borrow the parent's, so CLI output still shows up;
    // if stdout was already redirected we leave it alone.
    if (GetStdHandle(STD_OUTPUT_HANDLE) != nullptr &&
        GetStdHandle(STD_OUTPUT_HANDLE) != INVALID_HANDLE_VALUE)
        return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
}

}  // namespace hawk
