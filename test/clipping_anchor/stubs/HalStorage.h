#pragma once

#include <fcntl.h>

#include <cstddef>
#include <string>

// Captures appended clipping exports in memory.
class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::string* sink) : sink(sink) {}
  explicit operator bool() const { return sink != nullptr; }
  size_t write(const void* data, const size_t size) {
    if (!sink) return 0;
    sink->append(static_cast<const char*>(data), size);
    return size;
  }
  void flush() {}
  void close() { sink = nullptr; }

 private:
  std::string* sink = nullptr;
};

struct HalStorageStub {
  std::string lastPath;
  std::string contents;
  bool failOpen = false;
  HalFile open(const char* path, int /*flags*/) {
    lastPath = path;
    return failOpen ? HalFile() : HalFile(&contents);
  }
};

inline HalStorageStub Storage;
