/*
 * The C runtime calls whose spelling differs between Windows' CRT and the
 * POSIX systems the host also builds for (macOS first): the host calls these,
 * never a platform's own name for them. Windows keeps exactly the calls it made
 * before; the others are their POSIX equivalents.
 */

#ifndef PS2HOST_CRT_H
#define PS2HOST_CRT_H

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace ps2host {

// A time as the host's local calendar (localtime_s; localtime_r).
inline void LocalTime(std::time_t time, std::tm* out) {
#ifdef _WIN32
    localtime_s(out, &time);
#else
    localtime_r(&time, out);
#endif
}

// A time as UTC's calendar (gmtime_s; gmtime_r).
inline void UtcTime(std::time_t time, std::tm* out) {
#ifdef _WIN32
    gmtime_s(out, &time);
#else
    gmtime_r(&time, out);
#endif
}

// Seeking and telling past 2 GB: a DVD image is 4.7 GB and more, beyond what
// fseek's long reaches on Windows.
inline int FileSeek64(std::FILE* file, std::int64_t offset, int origin) {
#ifdef _WIN32
    return _fseeki64(file, offset, origin);
#else
    return fseeko(file, off_t(offset), origin);
#endif
}

inline std::int64_t FileTell64(std::FILE* file) {
#ifdef _WIN32
    return _ftelli64(file);
#else
    return std::int64_t(ftello(file));
#endif
}

// Opens a file for reading by a path that may hold any character: Windows'
// fopen takes the ANSI code page, so the wide call there; elsewhere paths are
// UTF-8 bytes already.
inline std::FILE* OpenForReading(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.wstring().c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

}  // namespace ps2host

#endif  // PS2HOST_CRT_H
