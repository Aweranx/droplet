#include <absl/strings/string_view.h>
#include <google/protobuf/stubs/common.h>
#include <google/protobuf/wire_format_lite.h>
#include <gtest/gtest.h>
#include <ikcp.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <tinyxml2.h>
#include <yaml-cpp/yaml.h>
#include <zlib.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace {

TEST(TestThirdParty, yaml_cppParsesStructuredData) {
  // YAML 节点可以直接转换为项目需要的基础类型和序列容器。
  const YAML::Node document = YAML::Load(R"(
name: droplet
enabled: true
ports: [8080, 8443]
)");

  ASSERT_TRUE(document.IsMap());
  EXPECT_EQ(document["name"].as<std::string>(), "droplet");
  EXPECT_TRUE(document["enabled"].as<bool>());
  EXPECT_EQ(document["ports"].as<std::vector<int>>(),
            (std::vector<int>{8080, 8443}));
}

TEST(TestThirdParty, rapidjsonParsesAndSerializesJson) {
  // RapidJSON 解析后再序列化，内容应保持可读且可再次解析。
  rapidjson::Document document;
  document.Parse(R"({"name":"droplet","count":2})");
  ASSERT_FALSE(document.HasParseError());
  ASSERT_TRUE(document.IsObject());
  EXPECT_STREQ(document["name"].GetString(), "droplet");
  EXPECT_EQ(document["count"].GetInt(), 2);

  rapidjson::StringBuffer output;
  rapidjson::Writer<rapidjson::StringBuffer> writer(output);
  ASSERT_TRUE(document.Accept(writer));

  rapidjson::Document round_trip;
  round_trip.Parse(output.GetString());
  ASSERT_FALSE(round_trip.HasParseError());
  ASSERT_TRUE(round_trip.IsObject());
  EXPECT_STREQ(round_trip["name"].GetString(), "droplet");
  EXPECT_EQ(round_trip["count"].GetInt(), 2);
}

TEST(TestThirdParty, tinyxml2ReadsAttributesAndText) {
  // tinyxml2 适合读取 HTTP/XML 配置等小型文档。
  tinyxml2::XMLDocument document;
  ASSERT_EQ(document.Parse(
                R"(<config><item id="7">enabled</item></config>)"),
            tinyxml2::XML_SUCCESS);

  const auto* root = document.FirstChildElement("config");
  ASSERT_NE(root, nullptr);
  const auto* item = root->FirstChildElement("item");
  ASSERT_NE(item, nullptr);
  EXPECT_STREQ(item->Attribute("id"), "7");
  EXPECT_STREQ(item->GetText(), "enabled");
}

TEST(TestThirdParty, zlibRoundTripsCompressedData) {
  // 压缩后解压，验证 zlib 的静态库和头文件均可正常使用。
  const std::string input =
      "droplet third-party smoke test: compression must preserve bytes";
  const auto input_size = static_cast<uLong>(input.size());
  std::vector<Bytef> compressed(compressBound(input_size));
  uLongf compressed_size = static_cast<uLongf>(compressed.size());

  ASSERT_EQ(compress2(compressed.data(), &compressed_size,
                      reinterpret_cast<const Bytef*>(input.data()), input_size,
                      Z_BEST_SPEED),
            Z_OK);

  std::vector<Bytef> restored(input.size());
  uLongf restored_size = static_cast<uLongf>(restored.size());
  ASSERT_EQ(uncompress(restored.data(), &restored_size, compressed.data(),
                       compressed_size),
            Z_OK);
  ASSERT_EQ(restored_size, input.size());
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(restored.data()),
                        restored_size),
            input);
}

struct KcpPacketQueue {
  std::vector<std::string> packets;
};

int CaptureKcpPacket(const char* data, int length, ikcpcb*, void* user) {
  auto* queue = static_cast<KcpPacketQueue*>(user);
  queue->packets.emplace_back(data, static_cast<std::size_t>(length));
  return 0;
}

struct KcpDeleter {
  void operator()(ikcpcb* kcp) const noexcept {
    if (kcp != nullptr) {
      ikcp_release(kcp);
    }
  }
};

TEST(TestThirdParty, kcpTransfersOneMessage) {
  // 用内存队列模拟 UDP：KCP 负责可靠传输，上层只需转发数据包。
  KcpPacketQueue queue;
  std::unique_ptr<ikcpcb, KcpDeleter> sender(ikcp_create(0x12345678, &queue));
  std::unique_ptr<ikcpcb, KcpDeleter> receiver(ikcp_create(0x12345678, nullptr));
  ASSERT_NE(sender, nullptr);
  ASSERT_NE(receiver, nullptr);

  ikcp_setoutput(sender.get(), CaptureKcpPacket);
  ASSERT_EQ(ikcp_nodelay(sender.get(), 1, 10, 2, 1), 0);

  const std::string message = "hello from kcp";
  EXPECT_EQ(ikcp_send(sender.get(), message.data(),
                      static_cast<int>(message.size())),
            static_cast<int>(message.size()));
  ikcp_update(sender.get(), 0);
  ikcp_flush(sender.get());
  ASSERT_FALSE(queue.packets.empty());

  for (const auto& packet : queue.packets) {
    EXPECT_EQ(ikcp_input(receiver.get(), packet.data(),
                         static_cast<long>(packet.size())),
              0);
  }

  std::vector<char> received(message.size());
  ASSERT_EQ(ikcp_recv(receiver.get(), received.data(),
                      static_cast<int>(received.size())),
            static_cast<int>(message.size()));
  EXPECT_EQ(std::string(received.begin(), received.end()), message);
}

TEST(TestThirdParty, abseilStringViewProvidesNonOwningView) {
  // Abseil 的 string_view 不拥有字符串存储，只提供轻量视图。
  const std::string storage = "droplet";
  const absl::string_view view(storage);
  EXPECT_EQ(view.size(), storage.size());
  EXPECT_EQ(view.substr(0, 3), "dro");
  EXPECT_EQ(view.substr(3), "plet");
}

TEST(TestThirdParty, protobufWireFormatBuildsExpectedTag) {
  // 只使用 constexpr 编码接口即可验证 Protobuf 的公共头文件安装正确。
  constexpr std::uint32_t tag =
      google::protobuf::internal::WireFormatLite::MakeTag(
          1, google::protobuf::internal::WireFormatLite::WIRETYPE_VARINT);
  EXPECT_EQ(tag, 8U);
  EXPECT_GT(GOOGLE_PROTOBUF_VERSION, 0);
}

}  // namespace
