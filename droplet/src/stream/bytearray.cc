#include "droplet/stream/bytearray.h"
#include <droplet/types.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <type_traits>

namespace droplet {

namespace {

u32 EncodeZigzag32(i32 value) noexcept {
  return (static_cast<u32>(value) << 1) ^
         static_cast<u32>(-(value < 0));
}

u64 EncodeZigzag64(i64 value) noexcept {
  return (static_cast<u64>(value) << 1) ^
         static_cast<u64>(-(value < 0));
}

i32 DecodeZigzag32(u32 value) noexcept {
  const auto bits = (value >> 1) ^ (0u - (value & 1u));
  return std::bit_cast<i32>(bits);
}

i64 DecodeZigzag64(u64 value) noexcept {
  const auto bits = (value >> 1) ^ (0ull - (value & 1ull));
  return std::bit_cast<i64>(bits);
}

std::size_t CheckedSize(u64 value) {
  if (value > std::numeric_limits<std::size_t>::max()) {
    throw std::length_error("ByteArray length does not fit in size_t");
  }
  return static_cast<std::size_t>(value);
}

}  // namespace

ByteArray::Node::Node(std::size_t size)
    : ptr(size == 0 ? nullptr : new char[size]), size(size) {}

ByteArray::Node::Node() noexcept = default;

ByteArray::Node::~Node() { delete[] ptr; }

ByteArray::ByteArray(std::size_t base_size)
    : base_size_(base_size), capacity_(base_size) {
  if (base_size_ == 0) {
    throw std::invalid_argument("ByteArray base size must be greater than zero");
  }
  root_ = new Node(base_size_);
  tail_ = root_;
  current_ = root_;
}

ByteArray::~ByteArray() {
  Node* node = root_;
  while (node) {
    Node* next = node->next;
    delete node;
    node = next;
  }
  root_ = nullptr;
  tail_ = nullptr;
  current_ = nullptr;
}

template <class T>
void ByteArray::writeFixed(T value) {
  static_assert(std::is_integral_v<T>);
  using Unsigned = std::make_unsigned_t<T>;

  Unsigned bits{};
  std::memcpy(&bits, &value, sizeof(value));
  std::array<u8, sizeof(T)> bytes{};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const std::size_t shift = little_endian_
                                  ? i * 8
                                  : (sizeof(T) - 1 - i) * 8;
    bytes[i] = static_cast<u8>(bits >> shift);
  }
  write(bytes.data(), bytes.size());
}

template <class T>
T ByteArray::readFixed() {
  static_assert(std::is_integral_v<T>);
  using Unsigned = std::make_unsigned_t<T>;

  std::array<u8, sizeof(T)> bytes{};
  read(bytes.data(), bytes.size());

  Unsigned bits{};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const std::size_t shift = little_endian_
                                  ? i * 8
                                  : (sizeof(T) - 1 - i) * 8;
    bits |= static_cast<Unsigned>(bytes[i]) << shift;
  }

  T value{};
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void ByteArray::writeFint8(i8 value) { writeFixed(value); }

void ByteArray::writeFuint8(u8 value) { writeFixed(value); }

void ByteArray::writeFint16(i16 value) { writeFixed(value); }

void ByteArray::writeFuint16(u16 value) { writeFixed(value); }

void ByteArray::writeFint32(i32 value) { writeFixed(value); }

void ByteArray::writeFuint32(u32 value) { writeFixed(value); }

void ByteArray::writeFint64(i64 value) { writeFixed(value); }

void ByteArray::writeFuint64(u64 value) { writeFixed(value); }

void ByteArray::writeInt32(i32 value) {
  writeUint32(EncodeZigzag32(value));
}

void ByteArray::writeUint32(u32 value) {
  std::array<u8, 5> bytes{};
  std::size_t count = 0;
  while (value >= 0x80u) {
    bytes[count++] = static_cast<u8>((value & 0x7fu) | 0x80u);
    value >>= 7;
  }
  bytes[count++] = static_cast<u8>(value);
  write(bytes.data(), count);
}

void ByteArray::writeInt64(i64 value) {
  writeUint64(EncodeZigzag64(value));
}

void ByteArray::writeUint64(u64 value) {
  std::array<u8, 10> bytes{};
  std::size_t count = 0;
  while (value >= 0x80u) {
    bytes[count++] = static_cast<u8>((value & 0x7fu) | 0x80u);
    value >>= 7;
  }
  bytes[count++] = static_cast<u8>(value);
  write(bytes.data(), count);
}

void ByteArray::writeFloat(float value) {
  writeFuint32(std::bit_cast<u32>(value));
}

void ByteArray::writeDouble(double value) {
  writeFuint64(std::bit_cast<u64>(value));
}

void ByteArray::writeStringF16(const std::string& value) {
  if (value.size() > std::numeric_limits<u16>::max()) {
    throw std::length_error("string is too long for a uint16 length");
  }
  writeFuint16(static_cast<u16>(value.size()));
  write(value.data(), value.size());
}

void ByteArray::writeStringF32(const std::string& value) {
  if (value.size() > std::numeric_limits<u32>::max()) {
    throw std::length_error("string is too long for a uint32 length");
  }
  writeFuint32(static_cast<u32>(value.size()));
  write(value.data(), value.size());
}

void ByteArray::writeStringF64(const std::string& value) {
  writeFuint64(static_cast<u64>(value.size()));
  write(value.data(), value.size());
}

void ByteArray::writeStringVint(const std::string& value) {
  writeUint64(static_cast<u64>(value.size()));
  write(value.data(), value.size());
}

void ByteArray::writeStringWithoutLength(const std::string& value) {
  write(value.data(), value.size());
}

i8 ByteArray::readFint8() { return readFixed<i8>(); }

u8 ByteArray::readFuint8() { return readFixed<u8>(); }

i16 ByteArray::readFint16() { return readFixed<i16>(); }

u16 ByteArray::readFuint16() { return readFixed<u16>(); }

i32 ByteArray::readFint32() { return readFixed<i32>(); }

u32 ByteArray::readFuint32() { return readFixed<u32>(); }

i64 ByteArray::readFint64() { return readFixed<i64>(); }

u64 ByteArray::readFuint64() { return readFixed<u64>(); }

i32 ByteArray::readInt32() {
  return DecodeZigzag32(readUint32());
}

u32 ByteArray::readUint32() {
  u32 result = 0;
  for (std::size_t index = 0; index < 5; ++index) {
    const u8 byte = readFuint8();
    if (index == 4 && byte > 0x0fu) {
      throw std::out_of_range("invalid uint32 varint");
    }
    result |= static_cast<u32>(byte & 0x7fu) << (index * 7);
    if ((byte & 0x80u) == 0) {
      return result;
    }
  }
  throw std::out_of_range("invalid uint32 varint");
}

i64 ByteArray::readInt64() {
  return DecodeZigzag64(readUint64());
}

u64 ByteArray::readUint64() {
  u64 result = 0;
  for (std::size_t index = 0; index < 10; ++index) {
    const u8 byte = readFuint8();
    if (index == 9 && byte > 0x01u) {
      throw std::out_of_range("invalid uint64 varint");
    }
    result |= static_cast<u64>(byte & 0x7fu) << (index * 7);
    if ((byte & 0x80u) == 0) {
      return result;
    }
  }
  throw std::out_of_range("invalid uint64 varint");
}

float ByteArray::readFloat() {
  return std::bit_cast<float>(readFuint32());
}

double ByteArray::readDouble() {
  return std::bit_cast<double>(readFuint64());
}

std::string ByteArray::readStringF16() {
  const auto length = static_cast<std::size_t>(readFuint16());
  if (length > getReadSize()) {
    throw std::out_of_range("not enough bytes for uint16 string");
  }
  std::string result(length, '\0');
  read(result.data(), length);
  return result;
}

std::string ByteArray::readStringF32() {
  const auto length = static_cast<std::size_t>(readFuint32());
  if (length > getReadSize()) {
    throw std::out_of_range("not enough bytes for uint32 string");
  }
  std::string result(length, '\0');
  read(result.data(), length);
  return result;
}

std::string ByteArray::readStringF64() {
  const auto length = CheckedSize(readFuint64());
  if (length > getReadSize()) {
    throw std::out_of_range("not enough bytes for uint64 string");
  }
  std::string result(length, '\0');
  read(result.data(), length);
  return result;
}

std::string ByteArray::readStringVint() {
  const auto length = CheckedSize(readUint64());
  if (length > getReadSize()) {
    throw std::out_of_range("not enough bytes for varint string");
  }
  std::string result(length, '\0');
  read(result.data(), length);
  return result;
}

void ByteArray::clear() noexcept {
  Node* node = root_->next;
  while (node) {
    Node* next = node->next;
    delete node;
    node = next;
  }
  root_->next = nullptr;
  tail_ = root_;
  current_ = root_;
  position_ = 0;
  size_ = 0;
  capacity_ = base_size_;
}

ByteArray::Node* ByteArray::nodeAt(std::size_t position,
                                   std::size_t& offset) noexcept {
  Node* node = root_;
  while (node && position >= node->size) {
    position -= node->size;
    node = node->next;
  }
  offset = position;
  return node;
}

const ByteArray::Node* ByteArray::nodeAt(
    std::size_t position, std::size_t& offset) const noexcept {
  const Node* node = root_;
  while (node && position >= node->size) {
    position -= node->size;
    node = node->next;
  }
  offset = position;
  return node;
}

void ByteArray::addCapacity(std::size_t size) {
  if (size <= getCapacity()) {
    return;
  }

  const std::size_t missing = size - getCapacity();
  const std::size_t count = missing / base_size_ +
                            static_cast<std::size_t>(missing % base_size_ != 0);
  if (count > (std::numeric_limits<std::size_t>::max() - capacity_) /
                  base_size_) {
    throw std::length_error("ByteArray capacity overflow");
  }

  for (std::size_t index = 0; index < count; ++index) {
    tail_->next = new Node(base_size_);
    tail_ = tail_->next;
    capacity_ += base_size_;
  }
}

void ByteArray::write(const void* buffer, std::size_t size) {
  if (size == 0) {
    return;
  }
  if (!buffer) {
    throw std::invalid_argument("ByteArray::write received a null buffer");
  }
  if (size > std::numeric_limits<std::size_t>::max() - position_) {
    throw std::length_error("ByteArray position overflow");
  }

  addCapacity(size);
  std::size_t offset = 0;
  Node* node = nodeAt(position_, offset);
  const auto* source = static_cast<const char*>(buffer);
  std::size_t remaining = size;

  while (remaining != 0 && node) {
    const std::size_t count = std::min(remaining, node->size - offset);
    std::memcpy(node->ptr + offset, source, count);
    source += count;
    remaining -= count;
    position_ += count;
    node = node->next;
    offset = 0;
  }

  if (remaining != 0) {
    throw std::logic_error("ByteArray internal capacity error");
  }
  size_ = std::max(size_, position_);
  std::size_t current_offset = 0;
  current_ = nodeAt(position_, current_offset);
}

void ByteArray::read(void* buffer, std::size_t size) {
  if (size == 0) {
    return;
  }
  if (!buffer) {
    throw std::invalid_argument("ByteArray::read received a null buffer");
  }
  if (size > getReadSize()) {
    throw std::out_of_range("ByteArray does not contain enough readable bytes");
  }

  std::size_t offset = 0;
  Node* node = nodeAt(position_, offset);
  auto* destination = static_cast<char*>(buffer);
  std::size_t remaining = size;

  while (remaining != 0 && node) {
    const std::size_t count = std::min(remaining, node->size - offset);
    std::memcpy(destination, node->ptr + offset, count);
    destination += count;
    remaining -= count;
    position_ += count;
    node = node->next;
    offset = 0;
  }

  if (remaining != 0) {
    throw std::logic_error("ByteArray internal readable range error");
  }
  std::size_t current_offset = 0;
  current_ = nodeAt(position_, current_offset);
}

void ByteArray::read(void* buffer, std::size_t size,
                     std::size_t position) const {
  if (size == 0) {
    return;
  }
  if (!buffer) {
    throw std::invalid_argument("ByteArray::read received a null buffer");
  }
  if (position > size_ || size > size_ - position) {
    throw std::out_of_range("ByteArray does not contain enough readable bytes");
  }

  std::size_t offset = 0;
  const Node* node = nodeAt(position, offset);
  auto* destination = static_cast<char*>(buffer);
  std::size_t remaining = size;

  while (remaining != 0 && node) {
    const std::size_t count = std::min(remaining, node->size - offset);
    std::memcpy(destination, node->ptr + offset, count);
    destination += count;
    remaining -= count;
    node = node->next;
    offset = 0;
  }

  if (remaining != 0) {
    throw std::logic_error("ByteArray internal readable range error");
  }
}

void ByteArray::setPosition(std::size_t position) {
  if (position > capacity_) {
    throw std::out_of_range("ByteArray position exceeds capacity");
  }
  position_ = position;
  size_ = std::max(size_, position_);
  std::size_t offset = 0;
  current_ = nodeAt(position_, offset);
}

bool ByteArray::writeToFile(const std::string& name) const {
  std::ofstream output(name, std::ios::binary | std::ios::trunc);
  if (!output) {
    return false;
  }

  std::vector<iovec> buffers;
  (void)getReadBuffers(buffers);
  for (const auto& buffer : buffers) {
    const auto* data = static_cast<const char*>(buffer.iov_base);
    std::size_t remaining = buffer.iov_len;
    while (remaining != 0) {
      const auto count = std::min<std::size_t>(
          remaining, static_cast<std::size_t>(
                         std::numeric_limits<std::streamsize>::max()));
      output.write(data, static_cast<std::streamsize>(count));
      if (!output) {
        return false;
      }
      data += count;
      remaining -= count;
    }
  }
  return true;
}

bool ByteArray::readFromFile(const std::string& name) {
  std::ifstream input(name, std::ios::binary);
  if (!input) {
    return false;
  }

  std::array<char, 16 * 1024> buffer{};
  while (input.read(buffer.data(), static_cast<std::streamsize>(buffer.size())) ||
         input.gcount() != 0) {
    const auto count = input.gcount();
    write(buffer.data(), static_cast<std::size_t>(count));
  }
  return !input.bad();
}

std::string ByteArray::toString() const {
  std::string result(getReadSize(), '\0');
  if (!result.empty()) {
    read(result.data(), result.size(), position_);
  }
  return result;
}

std::string ByteArray::toHexString() const {
  const std::string value = toString();
  std::ostringstream output;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index != 0 && index % 32 == 0) {
      output << '\n';
    }
    output << std::setw(2) << std::setfill('0') << std::hex
           << static_cast<unsigned>(static_cast<u8>(value[index]))
           << ' ';
  }
  return output.str();
}

u64 ByteArray::getReadBuffers(std::vector<iovec>& buffers,
                                        u64 length) const {
  const auto readable = static_cast<u64>(getReadSize());
  length = std::min(length, readable);
  if (length == 0) {
    return 0;
  }

  std::size_t offset = 0;
  const Node* node = nodeAt(position_, offset);
  u64 remaining = length;
  while (remaining != 0 && node) {
    const auto count = std::min<u64>(
        remaining, static_cast<u64>(node->size - offset));
    iovec buffer{};
    buffer.iov_base = const_cast<char*>(node->ptr + offset);
    buffer.iov_len = static_cast<std::size_t>(count);
    buffers.push_back(buffer);
    remaining -= count;
    node = node->next;
    offset = 0;
  }
  return length - remaining;
}

u64 ByteArray::getReadBuffers(std::vector<iovec>& buffers,
                                        u64 length,
                                        u64 position) const {
  if (position > size_) {
    return 0;
  }
  const auto readable = static_cast<u64>(size_ - position);
  length = std::min(length, readable);
  if (length == 0) {
    return 0;
  }

  std::size_t offset = 0;
  const Node* node = nodeAt(static_cast<std::size_t>(position), offset);
  u64 remaining = length;
  while (remaining != 0 && node) {
    const auto count = std::min<u64>(
        remaining, static_cast<u64>(node->size - offset));
    iovec buffer{};
    buffer.iov_base = const_cast<char*>(node->ptr + offset);
    buffer.iov_len = static_cast<std::size_t>(count);
    buffers.push_back(buffer);
    remaining -= count;
    node = node->next;
    offset = 0;
  }
  return length - remaining;
}

u64 ByteArray::getWriteBuffers(std::vector<iovec>& buffers,
                                         u64 length) {
  if (length == 0) {
    return 0;
  }
  const auto requested = CheckedSize(length);
  addCapacity(requested);

  std::size_t offset = 0;
  Node* node = nodeAt(position_, offset);
  u64 remaining = length;
  while (remaining != 0 && node) {
    const auto count = std::min<u64>(
        remaining, static_cast<u64>(node->size - offset));
    iovec buffer{};
    buffer.iov_base = node->ptr + offset;
    buffer.iov_len = static_cast<std::size_t>(count);
    buffers.push_back(buffer);
    remaining -= count;
    node = node->next;
    offset = 0;
  }
  return length - remaining;
}

}  // namespace droplet
