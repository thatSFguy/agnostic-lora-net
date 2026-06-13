// fs_compat.h — persistent key/value storage, abstracted across MCU families.
//
// The firmware keeps its config (radio PHY, BLE PIN, routing table, …) in a handful
// of tiny files. On nRF52 those live in the Adafruit LittleFS on internal flash via
// the `InternalFS` object and its `File` type. ESP32 has no such API, so this header
// provides a drop-in shim with the SAME surface (`InternalFS`, `File`,
// `FILE_O_READ`/`FILE_O_WRITE`) backed by the ESP32 core's LittleFS — letting the
// persistence call sites in main.cpp stay byte-for-byte identical on both platforms.
//
// Only the minimal surface main.cpp actually uses is shimmed:
//   InternalFS.begin()            -> mount (format-on-fail), idempotent
//   InternalFS.remove(path)
//   File f(InternalFS); f.open(path, FILE_O_READ|FILE_O_WRITE) -> bool
//   f.read(buf, n) / f.write(buf, n) / f.close() / (bool)f
#pragma once

#if defined(ESP32)

#include <LittleFS.h>

enum { FILE_O_READ = 0, FILE_O_WRITE = 1 };

// Mirrors the bits of Adafruit_LittleFS we depend on, over the ESP32 LittleFS.
class AgnFS {
public:
    // Mount once; LittleFS is formatted on first use if the partition is blank.
    bool begin() {
        if (mounted_) return true;
        mounted_ = LittleFS.begin(/*formatOnFail=*/true);
        return mounted_;
    }
    bool remove(const char* path) { return LittleFS.remove(path); }
private:
    bool mounted_ = false;
};

static AgnFS InternalFS;

// Mirrors Adafruit_LittleFS_Namespace::File for the read/write/close calls we make.
// Constructed from the FS like `File f(InternalFS);`, then open()ed.
class AgnFile {
public:
    AgnFile() {}
    explicit AgnFile(AgnFS&) {}
    bool open(const char* path, int mode) {
        f_ = LittleFS.open(path, mode == FILE_O_WRITE ? "w" : "r");
        return (bool)f_;
    }
    int    read(void* buf, size_t n)          { return f_ ? f_.read((uint8_t*)buf, n) : -1; }
    size_t write(const uint8_t* buf, size_t n){ return f_ ? f_.write(buf, n) : 0; }
    void   close()                            { if (f_) f_.close(); }
    operator bool() const                     { return (bool)f_; }
private:
    fs::File f_;
};

typedef AgnFile File;

#else  // nRF52 (Adafruit nRF5 core) — original behaviour, unchanged.

#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
using namespace Adafruit_LittleFS_Namespace;

#endif
