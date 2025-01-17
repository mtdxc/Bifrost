/*******************************************************
 * @author      : dog head
 * @date        : Created in 2023/8/16 4:13 下午
 * @mail        : qw225967@github.com
 * @project     : .
 * @file        : quic_send_algorithm_adapter.cpp
 * @description : TODO
 *******************************************************/

#include "bifrost_send_algorithm/quic_send_algorithm_adapter.h"

#include "quiche/quic/core/congestion_control/rtt_stats.h"

namespace bifrost {
const uint32_t DefaultInitRtt = 10u;
const uint32_t MaxIntervalSendPacketRemove = 400u;  // 两个 feedback 来回的时间

QuicSendAlgorithmAdapter::QuicSendAlgorithmAdapter(
    UvLoop* uv_loop, quic::CongestionControlType congestion_type)
    : uv_loop_(uv_loop) {
  clock_ = new QuicClockAdapter(uv_loop);

  rtt_stats_ = new quic::RttStats();
  rtt_stats_->set_initial_rtt(quic::QuicTimeDelta::FromMilliseconds(DefaultInitRtt));

  unacked_packet_map_ = new quic::QuicUnackedPacketMap(quic::Perspective::IS_CLIENT);
  random_ = quiche::QuicheRandom::GetInstance();
  connection_stats_ = new quic::QuicConnectionStats();

  // bbr算法需要使用 拥塞窗口计算发送bytes，使用pacing_rate计算发送间隔
  send_algorithm_interface_ = quic::SendAlgorithmInterface::Create(
      clock_, rtt_stats_, unacked_packet_map_,
      congestion_type, random_, connection_stats_, 0, nullptr);
}

QuicSendAlgorithmAdapter::~QuicSendAlgorithmAdapter() {
  delete clock_;
  delete rtt_stats_;
  delete unacked_packet_map_;
  delete random_;
  delete connection_stats_;
  delete send_algorithm_interface_;
}

void QuicSendAlgorithmAdapter::UpdateRtt(float rtt) {
  quic::QuicTime now = quic::QuicTime::Zero() +
                       quic::QuicTimeDelta::FromMilliseconds(
                           (int64_t)uv_loop_->get_time_ms_int64());
  if (rtt_stats_) {
    rtt_stats_->UpdateRtt(
        quic::QuicTimeDelta::FromMilliseconds(0),
        quic::QuicTimeDelta::FromMilliseconds((int64_t)rtt), now);
  }
}

void QuicSendAlgorithmAdapter::OnRtpPacketSend(RtpPacketPtr& rtp_packet,
                                               int64_t now) {
  quic::QuicTime ts = quic::QuicTime::Zero() + quic::QuicTimeDelta::FromMilliseconds(now);

  uint16_t wide_seq_number;
  rtp_packet->ReadTransportWideCc01(wide_seq_number);

  // 1 quic 记录发送信息
  SendPacketInfo temp;
  temp.sequence = wide_seq_number;
  temp.send_time = now;
  temp.send_bytes = rtp_packet->GetSize();
  temp.is_retrans = rtp_packet->IsReTrans();
  has_send_map_[temp.sequence] = temp;

  // 序号反转检验
  // 2.放入unack，此处不统计重复数据，使用无重传模式
  unacked_packet_map_->AddSentPacket(
      rtp_packet, wide_seq_number, largest_acked_seq_,
      quic::NOT_RETRANSMISSION, ts, true, true, quic::ECN_NOT_ECT);
  bytes_in_flight_ += temp.send_bytes;

  // 3 算法记录发送
  send_algorithm_interface_->OnPacketSent(
      ts, unacked_packet_map_->bytes_in_flight(),
      quic::QuicPacketNumber(wide_seq_number), rtp_packet->GetSize(),
      quic::HAS_RETRANSMITTABLE_DATA);

  if (now - last_remove_old_time_ > MaxIntervalSendPacketRemove)
    RemoveOldSendPacket();
}

void QuicSendAlgorithmAdapter::OnReceiveQuicAckFeedback(
    QuicAckFeedbackPacket* feedback) {
  auto now_ms = uv_loop_->get_time_ms_int64();
  int64_t transport_rtt_sum = 0;
  int64_t transport_rtt_count = 0;
  uint32_t ack_bytes = 0;

  for (auto it = feedback->Begin(); it != feedback->End(); ++it) {
    QuicAckFeedbackItem* item = *it;

    quic::QuicTime recv_time = quic::QuicTime::Zero() +
        quic::QuicTimeDelta::FromMilliseconds(now_ms - item->GetDelta());

    acked_packets_.push_back(
        quic::AckedPacket(quic::QuicPacketNumber(item->GetSequence()),
                          item->GetRecvBytes(), recv_time));

    auto ack_it = has_send_map_.find(item->GetSequence());
    if (ack_it != has_send_map_.end()) {
      ack_bytes += ack_it->second.send_bytes;
      bytes_in_flight_ -= ack_it->second.send_bytes;

      // 2.移除确认的数据
      unacked_packet_map_->RemoveFromInFlight(quic::QuicPacketNumber(ack_it->second.sequence));

      transport_rtt_sum += now_ms - ack_it->second.send_time - item->GetDelta();
      transport_rtt_count++;

      has_send_map_.erase(ack_it);
    }

    largest_acked_seq_ = ack_it->second.sequence;
  }

  // 3.确认当前最新seq
  unacked_packet_map_->IncreaseLargestAcked(
      quic::QuicPacketNumber(largest_acked_seq_));

  // 4.更新rtt
  quic::QuicTime quic_now = quic::QuicTime::Zero() + quic::QuicTimeDelta::FromMilliseconds(now_ms);
  auto transport_rtt = transport_rtt_sum / transport_rtt_count;
  transport_rtt_ = transport_rtt == 0
                       ? transport_rtt
                       : (transport_rtt_ * 8 + transport_rtt) / 9;
  rtt_stats_->UpdateRtt(quic::QuicTimeDelta::FromMilliseconds(5),
                        quic::QuicTimeDelta::FromMilliseconds(transport_rtt),
                        quic_now);

  // 5.进行拥塞事件触发
  // 5.1 删除旧数据,并更新当前周期丢包数据
  RemoveOldSendPacket();

  // bbr v1 无需最后两个数据，随意给个值
  send_algorithm_interface_->OnCongestionEvent(
      true, unacked_packet_map_->bytes_in_flight(), quic_now,
      acked_packets_, losted_packets_, quic::QuicPacketCount(0),
      quic::QuicPacketCount(0));

  acked_packets_.clear();
  losted_packets_.clear();

  send_algorithm_interface_->DebugShow();
}

void QuicSendAlgorithmAdapter::RemoveOldSendPacket() {
  auto now = uv_loop_->get_time_ms_int64();
  last_remove_old_time_ = now;
  int64_t remove_interval = MaxIntervalSendPacketRemove +
                            transport_rtt_ / 2;  // 1个feedback时间+传输的rtt/2

  auto it = has_send_map_.begin();
  while (it != has_send_map_.end()) {
    // 大于周重传未确认则判定丢失
    if (now - it->second.send_time > remove_interval) {
      losted_packets_.push_back(quic::LostPacket(
          quic::QuicPacketNumber(it->second.sequence), it->second.send_bytes));

      unacked_packet_map_->RemoveFromInFlight(
          quic::QuicPacketNumber(it->second.sequence));

      bytes_in_flight_ -= it->second.send_bytes;
      it = has_send_map_.erase(it);
    } else {
      it++;
    }
  }
  // 4.清除旧数据
  unacked_packet_map_->RemoveObsoletePackets();
}
}  // namespace bifrost