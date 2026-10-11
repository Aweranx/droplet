#pragma once

#include <droplet/config/convert.h>
#include <droplet/types.h>
#include <droplet/utils/noncopyable.h>

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>

namespace droplet {
using ListenerId = u64;

namespace detail {

DROPLET_API void ReportConfigError(std::string_view operation,
                                   std::string_view name,
                                   std::string_view type_name,
                                   std::string_view reason);

[[nodiscard]] DROPLET_API ListenerId NextListenerId() noexcept;

[[nodiscard]] DROPLET_API std::string NormalizeConfigName(std::string name);

[[nodiscard]] DROPLET_API bool IsValidConfigName(
    std::string_view name) noexcept;

}  // namespace detail

class DROPLET_API ConfigVarBase : public Noncopyable {
 public:
  using ptr = std::shared_ptr<ConfigVarBase>;

  explicit ConfigVarBase(std::string name, std::string description = {});
  virtual ~ConfigVarBase();

  [[nodiscard]] const std::string& getName() const noexcept { return name_; }
  [[nodiscard]] const std::string& getDescription() const noexcept {
    return description_;
  }

  [[nodiscard]] virtual YAML::Node toNode() const = 0;
  virtual bool fromNode(const YAML::Node& node) = 0;

  [[nodiscard]] std::string toString() const;
  bool fromString(std::string_view value);

  [[nodiscard]] virtual std::string_view getTypeName() const noexcept = 0;

  virtual void removeListener(ListenerId id) = 0;
  virtual void clearListeners() = 0;
  [[nodiscard]] virtual std::size_t listenerCount() const = 0;

 protected:
  std::string name_;
  std::string description_;
};

// 存储一个具体的yaml kv对配置项
// ConfigConvert负责把不同的值转为ConfigVar的value
template <typename T, typename Convert = ConfigConvert<T>>
class ConfigVar final : public ConfigVarBase {
 public:
  using ptr = std::shared_ptr<ConfigVar>;
  using OnChange = std::function<void(const T& old_value, const T& new_value)>;

  ConfigVar(std::string name, T default_value, std::string description = {})
      : ConfigVarBase(std::move(name), std::move(description)),
        value_(std::make_shared<const T>(std::move(default_value))) {}

  ~ConfigVar() override { clearListeners(); }

  [[nodiscard]] T getValue() const {
    return *value_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::shared_ptr<const T> getSnapshot() const noexcept {
    return value_.load(std::memory_order_acquire);
  }

  // 修改的时候会重新构建一个对象及指针
  // 其他线程正在读的旧值依然可用，比如toNode
  bool setValue(T value) {
    auto new_value = std::make_shared<const T>(std::move(value));
    std::shared_ptr<const T> old_value;
    {
      std::lock_guard<std::mutex> lock(writeMutex_);
      old_value = value_.load(std::memory_order_relaxed);
      if constexpr (std::equality_comparable<T>) {
        if (*old_value == *new_value) {
          return false;
        }
      }
      value_.store(new_value, std::memory_order_release);
    }
    notify(*old_value, *new_value);
    return true;
  }

  [[nodiscard]] YAML::Node toNode() const override {
    return Convert::ToNode(*getSnapshot());
  }

  bool fromNode(const YAML::Node& node) override {
    try {
      setValue(Convert::FromNode(node));
      return true;
    } catch (const std::exception& e) {
      detail::ReportConfigError("ConfigVar::fromNode", name_, getTypeName(),
                                e.what());
    } catch (...) {
      detail::ReportConfigError("ConfigVar::fromNode", name_, getTypeName(),
                                "未知异常");
    }
    return false;
  }

  [[nodiscard]] std::string_view getTypeName() const noexcept override {
    return TypeName<T>();
  }

  ListenerId addListener(OnChange callback) {
    if (!callback) {
      return 0;
    }

    const ListenerId id = detail::NextListenerId();
    auto shared_callback =
        std::make_shared<const OnChange>(std::move(callback));
    {
      std::lock_guard<std::mutex> lock(listenersMutex_);
      listeners_.emplace_back(id, std::move(shared_callback));
    }
    return id;
  }

  void removeListener(ListenerId id) override {
    std::lock_guard<std::mutex> lock(listenersMutex_);
    for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
      if (it->m_id == id) {
        listeners_.erase(it);
        return;
      }
    }
  }

  void clearListeners() override {
    std::lock_guard<std::mutex> lock(listenersMutex_);
    listeners_.clear();
  }

  [[nodiscard]] std::size_t listenerCount() const override {
    std::lock_guard<std::mutex> lock(listenersMutex_);
    return listeners_.size();
  }

 private:
  struct ListenerEntry {
    ListenerId m_id{0};
    std::shared_ptr<const OnChange> m_callback;
  };

  void notify(const T& old_value, const T& new_value) const {
    std::vector<ListenerEntry> snapshot;
    {
      std::lock_guard<std::mutex> lock(listenersMutex_);
      snapshot = listeners_;
    }
    for (const ListenerEntry& entry : snapshot) {
      try {
        (*entry.m_callback)(old_value, new_value);
      } catch (const std::exception& e) {
        detail::ReportConfigError("ConfigVar::listener", name_, getTypeName(),
                                  e.what());
      } catch (...) {
        detail::ReportConfigError("ConfigVar::listener", name_, getTypeName(),
                                  "未知异常");
      }
    }
  }

 private:
  std::atomic<std::shared_ptr<const T>> value_;
  std::mutex writeMutex_;
  mutable std::mutex listenersMutex_;
  std::vector<ListenerEntry> listeners_;
};

}  // namespace droplet