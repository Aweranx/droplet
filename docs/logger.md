## 流程
创建一个Logger，然后给logger添加appender。
使用DROPLET_LOG_DEBUG(logger)，创建一个LogLine，这个LogLine以及他的impl会在构造时获取所有的日志信息，然后在析构时调用logger的submit提交构建的record。
logger通过拿到pattern来设置Impl的formatter，在logger的submit里，先声明一个InlineBuffer，然后把拿到的record通过formatter format到InlineBuffer里，然后依次调用appender的append
输出日志。
appender在遇到fatal日志时会调用flush把数据从程序的内存刷到操作系统缓冲区。


LogRecordView
    │ 被 Formatter 读取
    ▼
Logger::Impl::submit()
    │ 创建输出缓冲区
    ▼
Formatter::format(record, buffer)
    │ 把字段写入 buffer
    ▼
FormattedRecordBuffer::view()
    │ 把最终文本交给多个 Appender
    ├──> StdoutAppenderImpl
    └──> FileAppenderImpl

Logger::Impl::submit里把一条record进行format到buffer，然后把buffer里的内容写入到appender里进行记录。


# 优化

## PImpl 隐藏实现架构
Logger   导出api里可以看到的类
    ↓ unique_ptr<Logger::Impl>
Logger::Impl    仅声明不导出
    ↓
LoggerAccess 访问 Logger 的私有 impl_    是logger的友元
    ↓
GetLogger / GetRootLogger 创建并返回 LoggerPtr   使用LoggerAccess的static函数

PImpl 隐藏内部数据和实现；
Access 控制内部代码如何访问私有实现；
工厂函数隐藏具体实现类的创建过程；
用户只依赖稳定的公开类和智能指针接口。


## 自留空间缓冲区
SBO针对小对象进行优化，直接在栈上分配一块byte数组，当超过阈值时才在堆上分配。
每条日志的信息基本都会走缓冲区构建,测试与直接使用stringstream对比速度是他的两倍。
```cpp
alignas(std::max_align_t) std::byte implStorage_[LOG_LINE_IMPL_SIZE];
```

### alignas 对齐
要声明对齐大小，可以加快访存速度，在arm上不对齐可能导致程序崩溃。
### placement new 和 construct_at
```cpp
// 都是表示在给定的位置上调用指定对象的构造函数
Impl* impl = ::new (implStorage_)
    Impl(logger, level, line, file_name);

Impl* impl = std::construct_at(
    reinterpret_cast<Impl*>(implStorage_),
    logger,
    level,
    line,
    file_name
);

// 销毁时
impl->~Impl();
std::destroy_at(impl);
```

### launder
在同一块地址上销毁一个对象后重新构造一个对象后返回时用launder来修饰。
告诉编译器不要继续根据旧对象身份对这个指针做那些假设；这里现在有一个新的对象。


## logger的实现细节

### formatter中int转换为char
使用std::to_chars(...)配合array，比stringstream更轻量，而且使用array避免了堆分配。


### 日期时间按秒缓存
在服务量很大的情况下，不断地获取时间并调用localtime_r把时间戳转化为本地时间开销很大。
所以判断如果和上一条日志时同一秒发生的，就直接复用他的日期字符串。
DateTimeFormatItem::format()

### 线程id获取
使用thread_local的局部static变量，因为每个线程自己的id创建时就已确定，
所以只需要在第一次调用时初始化一次，后续就不再发起syscall，直接返回THREAD_ID。
```cpp
u64 GetThreadId() noexcept {
  static thread_local const u64 THREAD_ID =
      static_cast<u64>(::syscall(SYS_gettid));
  return THREAD_ID;
}
```

### logrecordview和appender都使用的是非用有视图
在logline、logrecordview和appender这些接口的使用中，只有一份创建的数据，其他使用时都是只带地址和size的view视图。


### 自定义 SmallStreamBuffer
可以使用这种流式接口，避免直接使用 std::stringstream 作为日志中间存储。
它不会再维护一份独立字符串，而是直接把写入内容追加到 InlineBuffer。
DROPLET_LOG_INFO(logger) << "id=" << id;


## 总结
我实现的是一个同步低分配日志系统。日志入口先做级别短路，过滤日志不会创建 LogLine 或进行格式化；LogLine 使用内联 PImpl，消息和最终记录使用 Small Buffer Optimization，短日志走栈内存，长日志才回退到堆。Formatter 的 pattern 只解析一次，整数使用 to_chars，时间按秒使用线程局部缓存，线程 ID 也按线程缓存。格式化结果通过 string_view 在多个 Appender 间复用，避免重复格式化和字符串复制。并发方面使用 atomic level、shared_mutex 和 Appender 独立锁，普通日志不主动 flush，从而降低锁和 I/O 开销。
当前是同步日志，不是异步日志；

## TODO
每条有效日志会创建一个局部 Buffer，尝试添加Buffer 池或复用机制。