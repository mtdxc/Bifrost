#define MS_CLASS "RTC::TransportCongestionControlServer"
// #define MS_LOG_DEV_LEVEL 3

#include "bifrost/bifrost_send_algorithm/tcc_server.h"

#include <iterator>  // std::ostream_iterator
#include <sstream>   // std::ostringstream

#include "uv_loop.h"

namespace bifrost {
/* Static. */

static constexpr uint64_t TransportCcFeedbackSendInterval{100u};  // In ms.
static constexpr uint64_t LimitationRembInterval{1500u};          // In ms.
static constexpr uint8_t UnlimitedRembNumPackets{4u};
static constexpr size_t PacketLossHistogramLength{24};

/* Instance methods. */

TransportCongestionControlServer::TransportCongestionControlServer(
    TransportCongestionControlServer::Observer* observer,
    size_t maxRtcpPacketLen, UvLoop* uv_loop)
    : observer_(observer),
      maxRtcpPacketLen(maxRtcpPacketLen),
      uv_loop_(uv_loop) {
  transportCcFeedbackPacket = std::make_shared<FeedbackRtpTransportPacket>(0u, 0u);
  quicFeedbackPacket = std::make_shared<QuicAckFeedbackPacket>(0u, 0u);

  // Set initial packet count.
  transportCcFeedbackPacket->SetFeedbackPacketCount(transportCcFeedbackPacketCount);

  // Create the feedback send periodic timer.
  transportCcFeedbackSendPeriodicTimer = new UvTimer(this, uv_loop_->get_loop());

  transportCcFeedbackSendPeriodicTimer->Start(
      TransportCcFeedbackSendInterval, TransportCcFeedbackSendInterval);
}

TransportCongestionControlServer::~TransportCongestionControlServer() {
  delete transportCcFeedbackSendPeriodicTimer;
  transportCcFeedbackSendPeriodicTimer = nullptr;

  // Delete REMB server.
  delete rembServer;
  rembServer = nullptr;

  packet_recv_time_map_.clear();
}

void TransportCongestionControlServer::QuicCountIncomingPacket(
    uint64_t nowMs, const RtpPacket* packet) {
  uint16_t wideSeqNumber;
  if (!packet->ReadTransportWideCc01(wideSeqNumber)) return;

  quicFeedbackPacket->SetSenderSsrc(0u);
  quicFeedbackPacket->SetMediaSsrc(transportCcFeedbackMediaSsrc);

  RecvPacketInfo temp;
  temp.recv_time = nowMs;
  temp.sequence = wideSeqNumber;
  temp.recv_bytes = packet->GetSize();
  packet_recv_time_map_[temp.sequence] = temp;
}

void TransportCongestionControlServer::SendQuicAckFeedback() {
  auto nowMs = uv_loop_->get_time_ms_int64();

  auto it = packet_recv_time_map_.begin();
  while (it != packet_recv_time_map_.end()) {
    auto feedbackItem = new QuicAckFeedbackItem(it->second.sequence,
                                                nowMs - it->second.recv_time,
                                                it->second.recv_bytes);
    quicFeedbackPacket->AddItem(feedbackItem);

    if (quicFeedbackPacket->GetSize() > BufferSize) {
      quicFeedbackPacket->Serialize(Buffer);

      observer_->OnTransportCongestionControlServerSendRtcpPacket(this, quicFeedbackPacket);

      quicFeedbackPacket.reset();
      quicFeedbackPacket = std::make_shared<QuicAckFeedbackPacket>(
          transportCcFeedbackSenderSsrc,
          transportCcFeedbackMediaSsrc);
    }

    it = packet_recv_time_map_.erase(it);
  }

  quicFeedbackPacket->Serialize(Buffer);

  observer_->OnTransportCongestionControlServerSendRtcpPacket(this, quicFeedbackPacket);

  quicFeedbackPacket.reset();

  quicFeedbackPacket = std::make_shared<QuicAckFeedbackPacket>(
      transportCcFeedbackSenderSsrc, transportCcFeedbackMediaSsrc);
}

void TransportCongestionControlServer::IncomingPacket(uint64_t nowMs,
                                                      const RtpPacket* packet) {
  uint16_t wideSeqNumber;

  if (!packet->ReadTransportWideCc01(wideSeqNumber)) return;

  // Update the RTCP media SSRC of the ongoing Transport-CC Feedback packet.
  transportCcFeedbackSenderSsrc = 0u;
  transportCcFeedbackMediaSsrc = packet->GetSsrc();

  transportCcFeedbackPacket->SetSenderSsrc(0u);
  transportCcFeedbackPacket->SetMediaSsrc(
      transportCcFeedbackMediaSsrc);

  // Provide the feedback packet with the RTP packet info. If it fails,
  // send current feedback and add the packet info to a new one.
  if (transportCcFeedbackPacket == nullptr) return;
  if (quicFeedbackPacket == nullptr) return;

  auto result = transportCcFeedbackPacket->AddPacket(
      wideSeqNumber, nowMs, maxRtcpPacketLen);

  switch (result) {
    case FeedbackRtpTransportPacket::AddPacketResult::SUCCESS: {
      // If the feedback packet is full, send it now.
      if (transportCcFeedbackPacket->IsFull()) {
        RTC_LOG(INFO) << "transport-cc feedback packet is full, sending feedback now";
        SendTransportCcFeedback();
      }

      break;
    }

    case FeedbackRtpTransportPacket::AddPacketResult::MAX_SIZE_EXCEEDED: {
      // Send ongoing feedback packet and add the new packet info to the
      // regenerated one.
      SendTransportCcFeedback();

      transportCcFeedbackPacket->AddPacket(wideSeqNumber, nowMs, maxRtcpPacketLen);

      break;
    }

    case FeedbackRtpTransportPacket::AddPacketResult::FATAL: {
      // Create a new feedback packet.
      transportCcFeedbackPacket =
          std::make_shared<FeedbackRtpTransportPacket>(
              transportCcFeedbackSenderSsrc,
              transportCcFeedbackMediaSsrc);

      // Use current packet count.
      // NOTE: Do not increment it since the previous ongoing feedback
      // packet was not sent.
      transportCcFeedbackPacket->SetFeedbackPacketCount(transportCcFeedbackPacketCount);

      break;
    }
  }

  MaySendLimitationRembFeedback();
}

void TransportCongestionControlServer::SetMaxIncomingBitrate(uint32_t bitrate) {
  auto previousMaxIncomingBitrate = maxIncomingBitrate;

  maxIncomingBitrate = bitrate;

  if (previousMaxIncomingBitrate != 0u && maxIncomingBitrate == 0u) {
    // This is to ensure that we send N REMB packets with bitrate 0 (unlimited).
    unlimitedRembCounter = UnlimitedRembNumPackets;

    MaySendLimitationRembFeedback();
  }
}

inline void TransportCongestionControlServer::SendTransportCcFeedback() {
  if (!transportCcFeedbackPacket->IsSerializable()) return;

  auto latestWideSeqNumber = transportCcFeedbackPacket->GetLatestSequenceNumber();
  auto latestTimestamp = transportCcFeedbackPacket->GetLatestTimestamp();

  //  RTC_LOG(INFO) << "SendTransportCcFeedback latestWideSeqNumber:"
  //            << latestWideSeqNumber << ", latestTimestamp:" << latestTimestamp
  //            << ", base seq:" << transportCcFeedbackPacket->GetBaseSequenceNumber()
  //            << ", feedback count:" << (uint32_t)transportCcFeedbackPacket->GetFeedbackPacketCount();

  // Notify the listener.
  observer_->OnTransportCongestionControlServerSendRtcpPacket(this, transportCcFeedbackPacket);

  // Update packet loss history.
  size_t expected_packets = transportCcFeedbackPacket->GetPacketStatusCount();

  size_t lost_packets = 0;
  for (const auto& result : transportCcFeedbackPacket->GetPacketResults()) {
    if (!result.received) lost_packets += 1;
  }

  UpdatePacketLoss(static_cast<double>(lost_packets) / expected_packets);

  // Create a new feedback packet.
  transportCcFeedbackPacket =
      std::make_shared<FeedbackRtpTransportPacket>(
          transportCcFeedbackSenderSsrc,
          transportCcFeedbackMediaSsrc);

  // Increment packet count.
  transportCcFeedbackPacket->SetFeedbackPacketCount(++transportCcFeedbackPacketCount);

  // Pass the latest packet info (if any) as pre base for the new feedback
  // packet.
  if (latestTimestamp > 0u) {
    transportCcFeedbackPacket->AddPacket(latestWideSeqNumber, latestTimestamp, maxRtcpPacketLen);
  }
}

inline void TransportCongestionControlServer::MaySendLimitationRembFeedback() {}

void TransportCongestionControlServer::UpdatePacketLoss(double packetLoss) {
  // Add the score into the histogram.
  if (packetLossHistory.size() == PacketLossHistogramLength)
    packetLossHistory.pop_front();

  packetLossHistory.push_back(packetLoss);

  // Calculate a weighted average
  size_t weight{0};
  size_t samples{0};
  double totalPacketLoss{0};

  for (auto packetLossEntry : packetLossHistory) {
    weight++;
    samples += weight;
    totalPacketLoss += weight * packetLossEntry;
  }

  // "thinks" that this can lead to division by zero but we are smarter.
  packetLoss = totalPacketLoss / samples;
}

inline void TransportCongestionControlServer::OnRembServerAvailableBitrate(
    const webrtc::RemoteBitrateEstimator* /*rembServer*/,
    const std::vector<uint32_t>& ssrcs, uint32_t availableBitrate) {}

inline void TransportCongestionControlServer::OnTimer(UvTimer* timer) {
  if (timer == transportCcFeedbackSendPeriodicTimer) {
    SendTransportCcFeedback();
    SendQuicAckFeedback();
  }
}
}  // namespace bifrost
