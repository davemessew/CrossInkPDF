#pragma once

#include <cstddef>
#include <cstdint>

namespace QemuSemihost {
int openRead(const char* path);
int openReadWrite(const char* path);
bool close(int handle);
int64_t length(int handle);
size_t read(int handle, void* destination, size_t bytes);
size_t write(int handle, const void* source, size_t bytes);
bool seek(int handle, uint64_t offset);
}  // namespace QemuSemihost
