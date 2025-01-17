/*******************************************************
 * @author      : dog head
 * @date        : Created in 2023/6/7 4:07 下午
 * @mail        : qw225967@github.com
 * @project     : worker
 * @file        : port_manager.cpp
 * @description : TODO
 *******************************************************/

#include "port_manager.h"

#include <iostream>
#include <uv.h>
#include "setting.h"
#include "utils.h"
#include "uv_loop.h"

static inline void onClose(uv_handle_t* handle) { delete handle; }

namespace bifrost {
uv_handle_t* PortManager::BindPort(Settings::Configuration config,
                                   uv_loop_t* loop) {
  RTC_LOG(INFO) << "BindPort by config befor " << config.rtcIp;
  // First normalize the IP. This may throw if invalid IP.
  IP::NormalizeIp(config.rtcIp);
  RTC_LOG(INFO) << "BindPort by config after " << config.rtcIp;

  struct sockaddr_storage bind_addr;
  uv_handle_t* uvHandle{nullptr};
  int err;
  int flags{0};
  int family = IP::get_family(config.rtcIp);
  switch (family) {
    case AF_INET: {
      err = uv_ip4_addr(config.rtcIp.c_str(), 0,
                        reinterpret_cast<struct sockaddr_in*>(&bind_addr));
      if (err != 0)
        RTC_LOG(WARNING) << "uv_ip4_addr() failed: " << uv_strerror(err);

      break;
    }

    case AF_INET6: {
      err = uv_ip6_addr(config.rtcIp.c_str(), 0,
                        reinterpret_cast<struct sockaddr_in6*>(&bind_addr));
      if (err != 0)
        RTC_LOG(WARNING) << "uv_ip6_addr() failed: " << uv_strerror(err);

      // Don't also bind into IPv4 when listening in IPv6.
      // flags |= UV_UDP_IPV6ONLY;

      break;
    }

    // This cannot happen.
    default: {
      RTC_LOG(WARNING) << "unknown IP family";
      break;
    }
  }

  // Set the chosen port into the sockaddr struct.
  switch (family) {
    case AF_INET:
      (reinterpret_cast<struct sockaddr_in*>(&bind_addr))->sin_port = htons(config.rtcPort);
      break;

    case AF_INET6:
      (reinterpret_cast<struct sockaddr_in6*>(&bind_addr))->sin6_port = htons(config.rtcPort);
      break;
  }

  uvHandle = reinterpret_cast<uv_handle_t*>(new uv_udp_t());
  uv_udp_init_ex(loop, reinterpret_cast<uv_udp_t*>(uvHandle), UV_UDP_RECVMMSG);
  uv_udp_bind(reinterpret_cast<uv_udp_t*>(uvHandle),
              reinterpret_cast<const struct sockaddr*>(&bind_addr), flags);

  return static_cast<uv_handle_t*>(uvHandle);
}

/* Class methods. */
uv_handle_t* PortManager::BindPort(uv_loop_t* loop) {
  RTC_LOG(INFO) << "BindPort befor " << Settings::config_.local_receive_configuration_.rtcIp;
  // First normalize the IP. This may throw if invalid IP.
  IP::NormalizeIp(Settings::config_.local_receive_configuration_.rtcIp);
  RTC_LOG(INFO) << "BindPort after " << Settings::config_.local_receive_configuration_.rtcIp;

  struct sockaddr_storage bind_addr;
  uv_handle_t* uvHandle{nullptr};
  int err;
  int flags{0};
  int family = IP::get_family(Settings::config_.local_receive_configuration_.rtcIp);
  switch (family) {
    case AF_INET: {
      err = uv_ip4_addr(Settings::config_.local_receive_configuration_.rtcIp.c_str(),
                        0, reinterpret_cast<struct sockaddr_in*>(&bind_addr));
      if (err != 0)
        RTC_LOG(WARNING) << "uv_ip4_addr failed: " << uv_strerror(err);

      break;
    }

    case AF_INET6: {
      err = uv_ip6_addr(Settings::config_.local_receive_configuration_.rtcIp.c_str(),
                        0, reinterpret_cast<struct sockaddr_in6*>(&bind_addr));
      if (err != 0)
        RTC_LOG(WARNING) << "uv_ip6_addr failed: " << uv_strerror(err);

      // Don't also bind into IPv4 when listening in IPv6.
      // flags |= UV_UDP_IPV6ONLY;

      break;
    }

    // This cannot happen.
    default: {
      RTC_LOG(WARNING) << "unknown IP family";
      break;
    }
  }

  // Set the chosen port into the sockaddr struct.
  switch (family) {
    case AF_INET:
      (reinterpret_cast<struct sockaddr_in*>(&bind_addr))->sin_port = htons(
          Settings::config_.local_receive_configuration_.rtcPort);
      break;

    case AF_INET6:
      (reinterpret_cast<struct sockaddr_in6*>(&bind_addr))->sin6_port = htons(
          Settings::config_.local_receive_configuration_.rtcPort);
      break;
  }

  uvHandle = reinterpret_cast<uv_handle_t*>(new uv_udp_t());
  uv_udp_init_ex(loop, reinterpret_cast<uv_udp_t*>(uvHandle), UV_UDP_RECVMMSG);
  uv_udp_bind(reinterpret_cast<uv_udp_t*>(uvHandle),
              reinterpret_cast<const struct sockaddr*>(&bind_addr), flags);

  return static_cast<uv_handle_t*>(uvHandle);
}

uv_handle_t* PortManager::Bind(Transport transport, std::string& ip) {
  // Do nothing
  return nullptr;
}

void PortManager::Unbind(Transport transport, std::string& ip, uint16_t port) {
  // Do nothing
}
}  // namespace bifrost