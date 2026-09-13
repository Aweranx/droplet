#pragma once

#include <droplet/bytearray/bytearray.h>
#include <droplet/export.h>

#include <cstddef>
#include <memory>

namespace droplet {

/** @brief 字节流的统一读写接口。 */
class DROPLET_API Stream {
 public:
  using Ptr = std::shared_ptr<Stream>;

  virtual ~Stream() = default;

  virtual int read(void* buffer, std::size_t length) = 0;
  virtual int read(ByteArray::Ptr buffer, std::size_t length) = 0;
  virtual int write(const void* buffer, std::size_t length) = 0;
  virtual int write(ByteArray::Ptr buffer, std::size_t length) = 0;

  /** @brief 循环读，直到读满 length 或流关闭/出错。 */
  virtual int readFixSize(void* buffer, std::size_t length);
  virtual int readFixSize(ByteArray::Ptr buffer, std::size_t length);
  /** @brief 循环写，直到写完 length 或流关闭/出错。 */
  virtual int writeFixSize(const void* buffer, std::size_t length);
  virtual int writeFixSize(ByteArray::Ptr buffer, std::size_t length);

  virtual void close() = 0;
};

}  // namespace droplet
