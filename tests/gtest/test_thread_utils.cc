#include <gtest/gtest.h>

#include <iostream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <pthread.h>
#endif

#include <droplet/utils/thread_utils.h>

TEST(thread_utils, id_name) {
  std::cout << "1: main thread id: " << droplet::GetThreadId() << std::endl;
  std::cout << "2: main thread name: " << droplet::GetThreadName() << std::endl;
  sleep(30);  // 输入命令：top -H -p <pid>
  droplet::SetThreadName("main_thread");
  std::cout << "3: main thread name: " << droplet::GetThreadName() << std::endl;
  sleep(10);
  droplet::SetThreadName("ranx");  
  std::cout << "4: main thread name: " << droplet::GetThreadName() << std::endl;
  sleep(10);
  droplet::SetThreadName("水滴攻击舰队"); // 一个汉字三个占字节
  std::cout << "5: main thread name: " << droplet::GetThreadName() << std::endl;
  sleep(10);
  std::thread worker([]() {
    std::cout << "6: main thread id: " << droplet::GetThreadId() << std::endl;
    std::cout << "7. son thread name: " << droplet::GetThreadName() << std::endl;
    sleep(10);
    droplet::SetThreadName("son-thread-long-name");
    std::cout << "8. son thread name: " << droplet::GetThreadName() << std::endl;
    sleep(10);
    droplet::SetThreadName("");
    std::cout << "9. son thread name: " << droplet::GetThreadName() << std::endl;
  });
  worker.join();
}