/*******************************************************
 * @author      : dog head
 * @date        : Created in 2022/10/19 2:42 下午
 * @mail        : qw225967@github.com
 * @project     : Bifrost
 * @file        : io_udp.h
 * @description : TODO
 *******************************************************/

#ifndef BIFROST_IO_UDP_H
#define BIFROST_IO_UDP_H

typedef struct uv_loop_s uv_loop_t;
#include <memory>

namespace bifrost {
class UvLoop {
 public:
  void ClassInit();
  void ClassDestroy();
  void PrintVersion();
  void RunLoop();

 public:
  uv_loop_t* get_loop() { return this->loop_.get(); }
  uint32_t get_time_s() {
    return get_time_ns() / 1000000000u;
  }
  uint64_t get_time_ms() {
    return get_time_ns() / 1000000u;
  }
  uint64_t get_time_us() { return get_time_ns() / 1000u; }
  uint64_t get_time_ns();
  // Used within libwebrtc dependency which uses int64_t values for time
  // representation.
  int64_t get_time_ms_int64() {
    return static_cast<int64_t>(UvLoop::get_time_ms());
  }
  // Used within libwebrtc dependency which uses int64_t values for time
  // representation.
  int64_t get_time_us_int64() {
    return static_cast<int64_t>(UvLoop::get_time_us());
  }

 private:
  std::shared_ptr<uv_loop_t> loop_;
};
}  // namespace bifrost

#endif  // BIFROST_IO_UDP_H
