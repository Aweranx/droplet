#include <droplet/stream/bytearray.h>
#include <droplet/types.h>

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

namespace {

using droplet::ByteArray;

TEST(ByteArrayTest, FixedTypesRoundTripAcrossNodes) {
  ByteArray bytes(3);
  bytes.writeFint8(-7);
  bytes.writeFuint16(0x1234);
  bytes.writeFint32(-1234567);
  bytes.writeFuint64(0x0123456789abcdefull);
  bytes.writeFloat(1.25f);
  bytes.writeDouble(-9.5);

  EXPECT_EQ(bytes.getSize(), sizeof(i8) + sizeof(u16) +
                                 sizeof(i32) + sizeof(u64) +
                                 sizeof(float) + sizeof(double));

  bytes.setPosition(0);
  EXPECT_EQ(bytes.readFint8(), -7);
  EXPECT_EQ(bytes.readFuint16(), 0x1234u);
  EXPECT_EQ(bytes.readFint32(), -1234567);
  EXPECT_EQ(bytes.readFuint64(), 0x0123456789abcdefull);
  EXPECT_FLOAT_EQ(bytes.readFloat(), 1.25f);
  EXPECT_DOUBLE_EQ(bytes.readDouble(), -9.5);
  EXPECT_EQ(bytes.getReadSize(), 0u);
}

TEST(ByteArrayTest, FixedIntegersRespectConfiguredEndian) {
  ByteArray big_endian(2);
  big_endian.writeFuint32(0x12345678u);
  big_endian.setPosition(0);
  EXPECT_EQ(big_endian.toHexString(), "12 34 56 78 ");

  ByteArray little_endian(2);
  little_endian.setIsLittleEndian(true);
  little_endian.writeFuint32(0x12345678u);
  EXPECT_TRUE(little_endian.isLittleEndian());
  little_endian.setPosition(0);
  EXPECT_EQ(little_endian.toHexString(), "78 56 34 12 ");

  little_endian.setPosition(0);
  EXPECT_EQ(little_endian.readFuint32(), 0x12345678u);
}

TEST(ByteArrayTest, VarintsAndLengthPrefixedStringsRoundTrip) {
  ByteArray bytes(1);
  const std::vector<u32> unsigned_values = {
      0u, 1u, 127u, 128u, 16384u, std::numeric_limits<u32>::max()};
  for (const auto value : unsigned_values) {
    bytes.writeUint32(value);
  }
  const std::vector<i32> signed_values = {
      std::numeric_limits<i32>::min(), -1, 0, 1,
      std::numeric_limits<i32>::max()};
  for (const auto value : signed_values) {
    bytes.writeInt32(value);
  }
  const std::vector<u64> wide_values = {
      0ull, 127ull, 128ull, 1ull << 32,
      std::numeric_limits<u64>::max()};
  for (const auto value : wide_values) {
    bytes.writeUint64(value);
  }
  const std::vector<i64> wide_signed_values = {
      std::numeric_limits<i64>::min(), -1, 0, 1,
      std::numeric_limits<i64>::max()};
  for (const auto value : wide_signed_values) {
    bytes.writeInt64(value);
  }

  bytes.writeStringF16("f16");
  bytes.writeStringF32("f32");
  bytes.writeStringF64("f64");
  bytes.writeStringVint("varint");

  bytes.setPosition(0);
  for (const auto value : unsigned_values) {
    EXPECT_EQ(bytes.readUint32(), value);
  }
  for (const auto value : signed_values) {
    EXPECT_EQ(bytes.readInt32(), value);
  }
  for (const auto value : wide_values) {
    EXPECT_EQ(bytes.readUint64(), value);
  }
  for (const auto value : wide_signed_values) {
    EXPECT_EQ(bytes.readInt64(), value);
  }
  EXPECT_EQ(bytes.readStringF16(), "f16");
  EXPECT_EQ(bytes.readStringF32(), "f32");
  EXPECT_EQ(bytes.readStringF64(), "f64");
  EXPECT_EQ(bytes.readStringVint(), "varint");
}

TEST(ByteArrayTest, ReadWriteBuffersExposeChainedStorage) {
  ByteArray bytes(4);
  std::vector<iovec> write_buffers;
  ASSERT_EQ(bytes.getWriteBuffers(write_buffers, 10), 10u);
  ASSERT_GE(write_buffers.size(), 3u);

  std::string expected = "0123456789";
  std::size_t offset = 0;
  for (const auto& buffer : write_buffers) {
    std::memcpy(buffer.iov_base, expected.data() + offset, buffer.iov_len);
    offset += buffer.iov_len;
  }
  bytes.setPosition(expected.size());
  bytes.setPosition(0);

  std::vector<iovec> read_buffers;
  ASSERT_EQ(bytes.getReadBuffers(read_buffers), expected.size());
  std::string actual;
  for (const auto& buffer : read_buffers) {
    actual.append(static_cast<const char*>(buffer.iov_base), buffer.iov_len);
  }
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(bytes.toString(), expected);
}

TEST(ByteArrayTest, FileRoundTripAndBounds) {
  const auto path = std::filesystem::temp_directory_path() /
                    "droplet_bytearray_test.bin";

  ByteArray source(5);
  source.writeStringWithoutLength("file payload");
  source.setPosition(0);
  ASSERT_TRUE(source.writeToFile(path.string()));

  ByteArray loaded(2);
  ASSERT_TRUE(loaded.readFromFile(path.string()));
  loaded.setPosition(0);
  EXPECT_EQ(loaded.toString(), "file payload");

  loaded.setPosition(0);
  std::array<char, 13> too_large{};
  EXPECT_THROW(loaded.read(too_large.data(), too_large.size()),
               std::out_of_range);
  std::filesystem::remove(path);
}

TEST(ByteArrayTest, InvalidVarintIsRejected) {
  ByteArray bytes(2);
  const u8 malformed[] = {0x80u, 0x80u, 0x80u, 0x80u, 0x10u};
  bytes.write(malformed, sizeof(malformed));
  bytes.setPosition(0);
  EXPECT_THROW((void)bytes.readUint32(), std::out_of_range);
}

}  // namespace
