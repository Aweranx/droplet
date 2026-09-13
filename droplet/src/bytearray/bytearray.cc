#include "droplet/bytearray/bytearray.h"

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

std::uint32_t EncodeZigzag32(std::int32_t value) noexcept {
  return (static_cast<std::uint32_t>(value) << 1) ^
         static_cast<std::uint32_t>(-(value < 0));
}

std::uint64_t EncodeZigzag64(std::int64_t value) noexcept {
  return (static_cast<std::uint64_t>(value) << 1) ^
         static_cast<std::uint64_t>(-(value < 0));
}

std::int32_t DecodeZigzag32(std::uint32_t value) noexcept {
  const auto bits = (value >> 1) ^ (0u - (value & 1u));
  return std::bit_cast<std::int32_t>(bits);
}

std::int64_t DecodeZigzag64(std::uint64_t value) noexcept {
  const auto bits = (value >> 1) ^ (0ull - (value & 1ull));
  return std::bit_cast<std::int64_t>(bits);
}

std::size_t CheckedSize(std::uint64_t value) {
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
  std::array<std::uint8_t, sizeof(T)> bytes{};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    const std::size_t shift = little_endian_
                                  ? i * 8
                                  : (sizeof(T) - 1 - i) * 8;
    bytes[i] = static_cast<std::uint8_t>(bits >> shift);
  }
  write(bytes.data(), bytes.size());
}

template <class T>
T ByteArray::readFixed() {
  static_assert(std::is_integral_v<T>);
  using Unsigned = std::make_unsigned_t<T>;

  std::array<std::uint8_t, sizeof(T)> bytes{};
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

void ByteArray::writeFint8(std::int8_t value) { writeFixed(value); }

void ByteArray::writeFuint8(std::uint8_t value) { writeFixed(value); }

void ByteArray::writeFint16(std::int16_t value) { writeFixed(value); }

void ByteArray::writeFuint16(std::uint16_t value) { writeFixed(value); }

void ByteArray::writeFint32(std::int32_t value) { writeFixed(value); }

void ByteArray::writeFuint32(std::uint32_t value) { writeFixed(value); }

void ByteArray::writeFint64(std::int64_t value) { writeFixed(value); }

void ByteArray::writeFuint64(std::uint64_t value) { writeFixed(value); }

void ByteArray::writeInt32(std::int32_t value) {
  writeUint32(EncodeZigzag32(value));
}

void ByteArray::writeUint32(std::uint32_t value) {
  std::array<std::uint8_t, 5> bytes{};
  std::size_t count = 0;
  while (value >= 0x80u) {
    bytes[count++] = static_cast<std::uint8_t>((value & 0x7fu) | 0x80u);
    value >>= 7;
  }
  bytes[count++] = static_cast<std::uint8_t>(value);
  write(bytes.data(), count);
}

void ByteArray::writeInt64(std::int64_t value) {
  writeUint64(EncodeZigzag64(value));
}

void ByteArray::writeUint64(std::uint64_t value) {
  std::array<std::uint8_t, 10> bytes{};
  std::size_t count = 0;
  while (value >= 0x80u) {
    bytes[count++] = static_cast<std::uint8_t>((value & 0x7fu) | 0x80u);
    value >>= 7;
  }
  bytes[count++] = static_cast<std::uint8_t>(value);
  write(bytes.data(), count);
}

void ByteArray::writeFloat(float value) {
  writeFuint32(std::bit_cast<std::uint32_t>(value));
}

void ByteArray::writeDouble(double value) {
  writeFuint64(std::bit_cast<std::uint64_t>(value));
}

void ByteArray::writeStringF16(const std::string& value) {
  if (value.size() > std::numeric_limits<std::uint16_t>::max()) {
    throw std::length_error("string is too long for a uint16 length");
  }
  writeFuint16(static_cast<std::uint16_t>(value.size()));
  write(value.data(), value.size());
}

void ByteArray::writeStringF32(const std::string& value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("string is too long for a uint32 length");
  }
  writeFuint32(static_cast<std::uint32_t>(value.size()));
  write(value.data(), value.size());
}

void ByteArray::writeStringF64(const std::string& value) {
  writeFuint64(static_cast<std::uint64_t>(value.size()));
  write(value.data(), value.size());
}

void ByteArray::writeStringVint(const std::string& value) {
  writeUint64(static_cast<std::uint64_t>(value.size()));
  write(value.data(), value.size());
}

void ByteArray::writeStringWithoutLength(const std::string& value) {
  write(value.data(), value.size());
}

std::int8_t ByteArray::readFint8() { return readFixed<std::int8_t>(); }

std::uint8_t ByteArray::readFuint8() { return readFixed<std::uint8_t>(); }

std::int16_t ByteArray::readFint16() { return readFixed<std::int16_t>(); }

std::uint16_t ByteArray::readFuint16() { return readFixed<std::uint16_t>(); }

std::int32_t ByteArray::readFint32() { return readFixed<std::int32_t>(); }

std::uint32_t ByteArray::readFuint32() { return readFixed<std::uint32_t>(); }

std::int64_t ByteArray::readFint64() { return readFixed<std::int64_t>(); }

std::uint64_t ByteArray::readFuint64() { return readFixed<std::uint64_t>(); }

std::int32_t ByteArray::readInt32() {
  return DecodeZigzag32(readUint32());
}

std::uint32_t ByteArray::readUint32() {
  std::uint32_t result = 0;
  for (std::size_t index = 0; index < 5; ++index) {
    const std::uint8_t byte = readFuint8();
    if (index == 4 && byte > 0x0fu) {
      throw std::out_of_range("invalid uint32 varint");
    }
    result |= static_cast<std::uint32_t>(byte & 0x7fu) << (index * 7);
    if ((byte & 0x80u) == 0) {
      return result;
    }
  }
  throw std::out_of_range("invalid uint32 varint");
}

std::int64_t ByteArray::readInt64() {
  return DecodeZigzag64(readUint64());
}

std::uint64_t ByteArray::readUint64() {
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < 10; ++index) {
    const std::uint8_t byte = readFuint8();
    if (index == 9 && byte > 0x01u) {
      throw std::out_of_range("invalid uint64 varint");
    }
    result |= static_cast<std::uint64_t>(byte & 0x7fu) << (index * 7);
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
           << static_cast<unsigned>(static_cast<std::uint8_t>(value[index]))
           << ' ';
  }
  return output.str();
}

std::uint64_t ByteArray::getReadBuffers(std::vector<iovec>& buffers,
                                        std::uint64_t length) const {
  const auto readable = static_cast<std::uint64_t>(getReadSize());
  length = std::min(length, readable);
  if (length == 0) {
    return 0;
  }

  std::size_t offset = 0;
  const Node* node = nodeAt(position_, offset);
  std::uint64_t remaining = length;
  while (remaining != 0 && node) {
    const auto count = std::min<std::uint64_t>(
        remaining, static_cast<std::uint64_t>(node->size - offset));
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

std::uint64_t ByteArray::getReadBuffers(std::vector<iovec>& buffers,
                                        std::uint64_t length,
                                        std::uint64_t position) const {
  if (position > size_) {
    return 0;
  }
  const auto readable = static_cast<std::uint64_t>(size_ - position);
  length = std::min(length, readable);
  if (length == 0) {
    return 0;
  }

  std::size_t offset = 0;
  const Node* node = nodeAt(static_cast<std::size_t>(position), offset);
  std::uint64_t remaining = length;
  while (remaining != 0 && node) {
    const auto count = std::min<std::uint64_t>(
        remaining, static_cast<std::uint64_t>(node->size - offset));
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

std::uint64_t ByteArray::getWriteBuffers(std::vector<iovec>& buffers,
                                         std::uint64_t length) {
  if (length == 0) {
    return 0;
  }
  const auto requested = CheckedSize(length);
  addCapacity(requested);

  std::size_t offset = 0;
  Node* node = nodeAt(position_, offset);
  std::uint64_t remaining = length;
  while (remaining != 0 && node) {
    const auto count = std::min<std::uint64_t>(
        remaining, static_cast<std::uint64_t>(node->size - offset));
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
