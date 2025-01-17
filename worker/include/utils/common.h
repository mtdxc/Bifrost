/*******************************************************
 * @author      : dog head
 * @date        : Created in 2023/6/7 4:15 下午
 * @mail        : qw225967@github.com
 * @project     : worker
 * @file        : common.h
 * @description : TODO
 *******************************************************/

#ifndef WORKER_COMMON_H
#define WORKER_COMMON_H

#include <algorithm>   // std::transform(), std::find(), std::min(), std::max()
#include <cinttypes>   // PRIu64, etc
#include <cstddef>     // size_t
#include <cstdint>     // uint8_t, etc
#include <functional>  // std::function
#include <iostream>
#include <memory>  // std::addressof()
#ifdef _WIN32
#include <winsock2.h>
// https://stackoverflow.com/a/27443191/2085408
#undef max
#undef min
// avoid uv/win.h: error C2628 'intptr_t' followed by 'int' is illegal.
#if !defined(_SSIZE_T_) && !defined(_SSIZE_T_DEFINED)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#define SSIZE_MAX INTPTR_MAX
#define _SSIZE_T_
#define _SSIZE_T_DEFINED
#endif
#else
#include <arpa/inet.h>   // htonl(), htons(), ntohl(), ntohs()
#include <netinet/in.h>  // sockaddr_in, sockaddr_in6
#include <sys/socket.h>  // struct sockaddr, struct sockaddr_storage, AF_INET, AF_INET6
#endif
#include "rtc_base/logging.h"
// 使用默认端口模式
//#define USING_DEFAULT_AF_CONFIG 1

// 使用真实数据做测试
//#define USING_LOCAL_FILE_DATA 1

// 使用小端模式
#define MS_LITTLE_ENDIAN 1

#define PUBLISHER_CONFIG_FILE_PATH_STRING "../conf/config.json"
#define LOCAL_DATA_FILE_PATH_STRING "../source/test.h264"

typedef struct uv_loop_s uv_loop_t;
typedef struct uv_handle_s uv_handle_t;
typedef struct uv_timer_s uv_timer_t;
typedef struct uv_tcp_s uv_tcp_t;
typedef struct uv_udp_s uv_udp_t;
typedef struct uv_buf_t uv_buf_t;

namespace bifrost {
class UvLoop;
class RtpPacket;
typedef std::shared_ptr<RtpPacket> RtpPacketPtr;
class RtcpPacket;
typedef std::shared_ptr<RtcpPacket> RtcpPacketPtr;
class CompoundPacket;
typedef std::shared_ptr<CompoundPacket> CompoundPacketPtr;

struct SendPacketInfo {
  uint16_t sequence;
  uint32_t send_time;
  uint32_t send_bytes;
  bool is_retrans = false;
};
}  // namespace bifrost

#endif  // WORKER_COMMON_H
