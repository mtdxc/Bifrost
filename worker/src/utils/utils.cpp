/*******************************************************
 * @author      : dog head
 * @date        : Created in 2023/6/7 4:44 下午
 * @mail        : qw225967@github.com
 * @project     : worker
 * @file        : utils.cpp
 * @description : TODO
 *******************************************************/

#include "utils.h"

#include <uv.h>

#include <iomanip>
#include <iostream>
#include <sstream>
#include <cstring>
#include <ctime>

namespace bifrost {
std::string Byte::bytes_to_hex(const uint8_t* buf, std::size_t len,
                               std::size_t num_per_line) {
  if (buf == NULL || len == 0 || num_per_line == 0) {
    return std::string();
  }
  std::ostringstream oss;
  for (std::size_t i = 0; i < len; i++) {
    oss << std::right << std::setw(3) << std::hex << static_cast<int>(buf[i]);
    if ((i + 1) % num_per_line == 0) {
      oss << '\n';
    }
  }
  if (len % num_per_line != 0) {
    oss << '\n';
  }
  return oss.str();
}

std::string String::get_now_str_s() {
  time_t t = time(nullptr);
  struct tm* now = localtime(&t);
  char temp_str[64] = {0};
  sprintf(temp_str, "%d-%02d-%02dT%02d:%02d:%02d", 
    now->tm_year + 1900, now->tm_mon + 1, now->tm_mday,
    now->tm_hour, now->tm_min, now->tm_sec);
  return temp_str;
}

int IP::get_family(const std::string& ip) {
  if (ip.size() >= INET6_ADDRSTRLEN) return AF_UNSPEC;

  auto ipPtr = ip.c_str();
  char ipBuffer[INET6_ADDRSTRLEN] = {0};

  if (uv_inet_pton(AF_INET, ipPtr, ipBuffer) == 0)
    return AF_INET;
  else if (uv_inet_pton(AF_INET6, ipPtr, ipBuffer) == 0)
    return AF_INET6;
  else
    return AF_UNSPEC;
}

void IP::get_address_info(const struct sockaddr* addr, int& family,
                          std::string& ip, uint16_t& port) {
  char ipBuffer[INET6_ADDRSTRLEN] = {0};
  int err;

  switch (addr->sa_family) {
    case AF_INET: {
      auto ipv4 = reinterpret_cast<const struct sockaddr_in*>(addr);
      err = uv_inet_ntop(AF_INET, &ipv4->sin_addr, ipBuffer, sizeof(ipBuffer));
      if (err)
        RTC_LOG(WARNING) << "uv_inet_ntop() failed: " << uv_strerror(err);

      port = ntohs(ipv4->sin_port);
      break;
    }

    case AF_INET6: {
      auto ipv6 = reinterpret_cast<const struct sockaddr_in6*>(addr);
      err = uv_inet_ntop(AF_INET6, &ipv6->sin6_addr, ipBuffer, sizeof(ipBuffer));
      if (err)
        RTC_LOG(WARNING) << "uv_inet_ntop() failed: " << uv_strerror(err);

      port = ntohs(ipv6->sin6_port);
      break;
    }

    default: {
      RTC_LOG(WARNING) << "unknown network family: " << addr->sa_family;
    }
  }

  family = addr->sa_family;
  ip.assign(ipBuffer);
}

void IP::NormalizeIp(std::string& ip) {
  static sockaddr_storage addrStorage;
  char ipBuffer[INET6_ADDRSTRLEN] = {0};
  int err;

  switch (IP::get_family(ip)) {
    case AF_INET: {
      auto addr = reinterpret_cast<struct sockaddr_in*>(&addrStorage);
      err = uv_ip4_addr(ip.c_str(), 0, addr);
      if (err != 0) {
        RTC_LOG(WARNING) << "uv_ip4_addr("<< ip << ") failed: " << uv_strerror(err);
      }

      err = uv_ip4_name(addr, ipBuffer, sizeof(ipBuffer));
      if (err != 0) {
        RTC_LOG(WARNING) << "uv_ipv4_name(" << ip <<") failed:" << uv_strerror(err);
      }

      ip.assign(ipBuffer);
      break;
    }

    case AF_INET6: {
      auto addr = reinterpret_cast<struct sockaddr_in6*>(&addrStorage);
      err = uv_ip6_addr(ip.c_str(), 0, addr);
      if (err != 0) {
        RTC_LOG(WARNING) << "uv_ip6_addr("<< ip << ") failed: " << uv_strerror(err);
      }

      err = uv_ip6_name(addr, ipBuffer, sizeof(ipBuffer));
      if (err != 0) {
        RTC_LOG(WARNING) << "uv_ipv6_name(" << ip <<") failed:" << uv_strerror(err);
      }

      ip.assign(ipBuffer);
      break;
    }

    default: {
      RTC_LOG(WARNING) << "invalid IP " << ip;
    }
  }
}
}  // namespace bifrost