#include "droplet/stream/stream.h"

namespace droplet {

int Stream::readFixSize(void* buffer, std::size_t length) {
  if (!buffer && length != 0) {
    return -1;
  }
  std::size_t offset = 0;
  while (offset < length) {
    const int result = read(static_cast<char*>(buffer) + offset,
                            length - offset);
    if (result <= 0) {
      return result;
    }
    offset += static_cast<std::size_t>(result);
  }
  return static_cast<int>(length);
}

int Stream::readFixSize(ByteArray::Ptr buffer, std::size_t length) {
  if (!buffer && length != 0) {
    return -1;
  }
  std::size_t offset = 0;
  while (offset < length) {
    const int result = read(buffer, length - offset);
    if (result <= 0) {
      return result;
    }
    offset += static_cast<std::size_t>(result);
  }
  return static_cast<int>(length);
}

int Stream::writeFixSize(const void* buffer, std::size_t length) {
  if (!buffer && length != 0) {
    return -1;
  }
  std::size_t offset = 0;
  while (offset < length) {
    const int result = write(static_cast<const char*>(buffer) + offset,
                             length - offset);
    if (result <= 0) {
      return result;
    }
    offset += static_cast<std::size_t>(result);
  }
  return static_cast<int>(length);
}

int Stream::writeFixSize(ByteArray::Ptr buffer, std::size_t length) {
  if (!buffer && length != 0) {
    return -1;
  }
  std::size_t offset = 0;
  while (offset < length) {
    const int result = write(buffer, length - offset);
    if (result <= 0) {
      return result;
    }
    offset += static_cast<std::size_t>(result);
  }
  return static_cast<int>(length);
}

}  // namespace droplet
