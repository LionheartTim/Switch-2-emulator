// Lichii native Linux compatibility definitions for AMD FidelityFX SDK 1.1.4.
// Applied ONLY to the copied AMD SDK's C++ compilation units, via sdk/CMakeLists.txt.
// This is a build adaptation, not a replacement for AMD FSR rendering.
#pragma once

#if !defined(_WIN32)
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <cwchar>

// MSVC's _countof works on arrays, not pointers. Preserve that guarantee.
template <class T, std::size_t N>
constexpr std::size_t ffxLichiiCountOf(const T (&)[N]) noexcept {
    return N;
}
#ifndef _countof
#define _countof(array) ffxLichiiCountOf(array)
#endif

// MSVC accepts wcscpy_s(destination_array, source). Preserve bounds checking.
inline int ffxLichiiCopyWide(wchar_t* destination, std::size_t capacity,
                             const wchar_t* source) noexcept {
    if (!destination || capacity == 0) return EINVAL;
    if (!source) {
        destination[0] = L'\0';
        return EINVAL;
    }
    const std::size_t length = std::wcslen(source);
    if (length >= capacity) {
        destination[0] = L'\0';
        return ERANGE;
    }
    std::wmemcpy(destination, source, length + 1);
    return 0;
}

template <std::size_t N>
inline int wcscpy_s(wchar_t (&destination)[N], const wchar_t* source) noexcept {
    return ffxLichiiCopyWide(destination, N, source);
}
inline int wcscpy_s(wchar_t* destination, std::size_t capacity,
                    const wchar_t* source) noexcept {
    return ffxLichiiCopyWide(destination, capacity, source);
}

inline int strcpy_s(char* destination, std::size_t capacity,
                    const char* source) noexcept {
    if (!destination || capacity == 0) return EINVAL;
    if (!source) {
        destination[0] = '\0';
        return EINVAL;
    }
    const std::size_t length = std::strlen(source);
    if (length >= capacity) {
        destination[0] = '\0';
        return ERANGE;
    }
    std::memcpy(destination, source, length + 1);
    return 0;
}

template <std::size_t N>
inline int strcpy_s(char (&destination)[N], const char* source) noexcept {
    return strcpy_s(destination, N, source);
}
#endif // !_WIN32
