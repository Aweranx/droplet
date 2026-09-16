#pragma once

#include <droplet/types.h>

#include <droplet/export.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <sys/uio.h>
#include <vector>

namespace droplet {

/**
 * @brief 面向网络协议的二进制序列化缓冲区。
 *
 * ByteArray 使用固定大小的链式内存块保存数据。当前位置既是写入位置，
 * 也是读取位置；写入后将位置设为 0 即可从头读取刚写入的数据。整数默认
 * 按大端序编码，也可以切换为小端序。链式块布局还能直接转换为 iovec，
 * 供 SocketStream 使用 scatter/gather I/O。
 */
class DROPLET_API ByteArray final {
 public:
  using Ptr = std::shared_ptr<ByteArray>;

  /** @brief ByteArray 的一块连续存储空间。 */
  struct Node {
    explicit Node(std::size_t size);
    Node() noexcept;
    ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    char* ptr{nullptr};
    Node* next{nullptr};
    std::size_t size{0};
  };

  explicit ByteArray(std::size_t base_size = 4096);
  ~ByteArray();

  ByteArray(const ByteArray&) = delete;
  ByteArray& operator=(const ByteArray&) = delete;
  ByteArray(ByteArray&&) = delete;
  ByteArray& operator=(ByteArray&&) = delete;

  void writeFint8(i8 value);
  void writeFuint8(u8 value);
  void writeFint16(i16 value);
  void writeFuint16(u16 value);
  void writeFint32(i32 value);
  void writeFuint32(u32 value);
  void writeFint64(i64 value);
  void writeFuint64(u64 value);

  void writeInt32(i32 value);
  void writeUint32(u32 value);
  void writeInt64(i64 value);
  void writeUint64(u64 value);

  void writeFloat(float value);
  void writeDouble(double value);

  void writeStringF16(const std::string& value);
  void writeStringF32(const std::string& value);
  void writeStringF64(const std::string& value);
  void writeStringVint(const std::string& value);
  void writeStringWithoutLength(const std::string& value);

  [[nodiscard]] i8 readFint8();
  [[nodiscard]] u8 readFuint8();
  [[nodiscard]] i16 readFint16();
  [[nodiscard]] u16 readFuint16();
  [[nodiscard]] i32 readFint32();
  [[nodiscard]] u32 readFuint32();
  [[nodiscard]] i64 readFint64();
  [[nodiscard]] u64 readFuint64();

  [[nodiscard]] i32 readInt32();
  [[nodiscard]] u32 readUint32();
  [[nodiscard]] i64 readInt64();
  [[nodiscard]] u64 readUint64();

  [[nodiscard]] float readFloat();
  [[nodiscard]] double readDouble();

  [[nodiscard]] std::string readStringF16();
  [[nodiscard]] std::string readStringF32();
  [[nodiscard]] std::string readStringF64();
  [[nodiscard]] std::string readStringVint();

  /** @brief 清空有效数据，但保留第一块内存。 */
  void clear() noexcept;

  /** @brief 从当前位置写入原始字节。 */
  void write(const void* buffer, std::size_t size);
  /** @brief 从当前位置读取原始字节；数据不足时抛出 std::out_of_range。 */
  void read(void* buffer, std::size_t size);
  /** @brief 从指定位置读取原始字节，不改变当前位置。 */
  void read(void* buffer, std::size_t size, std::size_t position) const;

  [[nodiscard]] std::size_t getPosition() const noexcept { return position_; }
  void setPosition(std::size_t position);

  [[nodiscard]] bool writeToFile(const std::string& name) const;
  [[nodiscard]] bool readFromFile(const std::string& name);

  [[nodiscard]] std::size_t getBaseSize() const noexcept { return base_size_; }
  [[nodiscard]] std::size_t getReadSize() const noexcept {
    return size_ - position_;
  }
  [[nodiscard]] std::size_t getSize() const noexcept { return size_; }

  [[nodiscard]] bool isLittleEndian() const noexcept { return little_endian_; }
  void setIsLittleEndian(bool value) noexcept { little_endian_ = value; }

  /** @brief 将 [position, size) 转换为字符串或十六进制字符串。 */
  [[nodiscard]] std::string toString() const;
  [[nodiscard]] std::string toHexString() const;

  /** @brief 获取当前可读区域的 scatter/gather 缓冲区。 */
  [[nodiscard]] u64 getReadBuffers(
      std::vector<iovec>& buffers,
      u64 length = UINT64_MAX) const;
  /** @brief 从指定位置获取可读区域的 scatter/gather 缓冲区。 */
  [[nodiscard]] u64 getReadBuffers(
      std::vector<iovec>& buffers, u64 length,
      u64 position) const;
  /** @brief 获取当前可写区域的 scatter/gather 缓冲区，并按需扩容。 */
  [[nodiscard]] u64 getWriteBuffers(
      std::vector<iovec>& buffers, u64 length);

 private:
  template <class T>
  void writeFixed(T value);

  template <class T>
  [[nodiscard]] T readFixed();

  void addCapacity(std::size_t size);
  [[nodiscard]] Node* nodeAt(std::size_t position,
                             std::size_t& offset) noexcept;
  [[nodiscard]] const Node* nodeAt(std::size_t position,
                                   std::size_t& offset) const noexcept;
  [[nodiscard]] std::size_t getCapacity() const noexcept {
    return capacity_ - position_;
  }

  std::size_t base_size_;
  std::size_t position_{0};
  std::size_t capacity_{0};
  std::size_t size_{0};
  bool little_endian_{false};
  Node* root_{nullptr};
  Node* tail_{nullptr};
  Node* current_{nullptr};
};

}  // namespace droplet
