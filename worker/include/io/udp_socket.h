/*******************************************************
 * @author      : dog head
 * @date        : Created in 2023/6/7 1:35 下午
 * @mail        : qw225967@github.com
 * @project     : worker
 * @file        : udp_socket.h
 * @description : TODO
 *******************************************************/

#ifndef WORKER_UDP_SOCKET_H
#define WORKER_UDP_SOCKET_H

#include <functional>
#include <string>
#include "common.h"
namespace bifrost {
/* Static. */
static constexpr size_t ReadBufferSize{65536};

class UdpSocket {
 public:
  using onSendCallback = const std::function<void(bool sent)>;
  /**
   * uv_handle_ must be an already initialized and binded uv_udp_t pointer.
   */
  explicit UdpSocket(uv_udp_t* uv_handle_);
  UdpSocket& operator=(const UdpSocket&) = delete;
  UdpSocket(const UdpSocket&) = delete;
  virtual ~UdpSocket();

 public:
  void Close();
  virtual void Dump() const;
  void Send(const uint8_t* data, size_t len, const struct sockaddr* addr,
            UdpSocket::onSendCallback* cb = nullptr);

  const struct sockaddr* GetLocalAddress() const {
    return reinterpret_cast<const struct sockaddr*>(&this->local_addr_);
  }
  int GetLocalFamily() const {
    return this->local_addr_.ss_family;
  }
  const std::string& GetLocalIp() const { return this->local_ip_; }
  uint16_t GetLocalPort() const { return this->local_port_; }
  size_t GetRecvBytes() const { return this->recv_bytes_; }
  size_t GetSentBytes() const { return this->sent_bytes_; }

 private:
  bool SetLocalAddress();

  /* Callbacks fired by UV events. */
 public:
  void OnUvRecvAlloc(size_t suggested_size, uv_buf_t* buf);
  void OnUvRecv(ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr,
                unsigned int flags);
  void OnUvSend(int status, UdpSocket::onSendCallback* cb);

  /* Pure virtual methods that must be implemented by the subclass. */
 protected:
  virtual void UserOnUdpDatagramReceived(const uint8_t* data, size_t len,
                                         const struct sockaddr* addr) = 0;

 protected:
  struct sockaddr_storage local_addr_;
  std::string local_ip_;
  uint16_t local_port_{0u};

 private:
  // Allocated by this (may be passed by argument).
  uv_udp_t* uv_handle_{nullptr};
  // Others.
  bool closed_{false};
  size_t recv_bytes_{0u};
  size_t sent_bytes_{0u};

  uint8_t ReadBuffer[ReadBufferSize];
};

}  // namespace bifrost
#endif  // WORKER_UDP_SOCKET_H
