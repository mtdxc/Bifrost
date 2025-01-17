#include "bifrost/bifrost_send_algorithm/tcc_client.h"
#include <api/transport/network_types.h>  // webrtc::TargetRateConstraints
#include <limits>
#include "uv_loop.h"

namespace bifrost {
/* Static. */
static constexpr uint32_t MinBitrate{30000u};
static constexpr float MaxBitrateMarginFactor{0.1f};
static constexpr float MaxBitrateIncrementFactor{1.35f};
static constexpr float MaxPaddingBitrateFactor{0.85f};
static constexpr uint64_t AvailableBitrateEventInterval{1000u};  // In ms.
static constexpr size_t PacketLossHistogramLength{24};
static constexpr uint32_t MaxAvailableBitrate{2048000u};

static uint8_t ProbationPacketHeader[] = {
    0b10010000, 0b01111111, 0, 0,  // PayloadType: 127, Sequence Number: 0
    0,          0,          0, 0,  // Timestamp: 0
    0,          0,          0, 0,  // SSRC: 0
    0xBE,       0xDE,       0, 4,  // Header Extension (One-Byte Extensions)
    0,          0,          0, 0,  // Space for MID extension
    0,          0,          0, 0, 0,
    0,          0,          0, 0,  // Space for abs-send-time extension
    0,          0,          0      // Space for transport-wide-cc-01 extension
};

/* Instance methods. */
TransportCongestionControlClient::TransportCongestionControlClient(
    TransportCongestionControlClient::Observer* observer,
    uint32_t initial_available_bitrate, UvLoop* uv_loop)
    : observer_(observer),
      initial_available_bitrate_(std::max<uint32_t>(initial_available_bitrate, MinBitrate)),
      max_outgoing_bitrate_(MaxAvailableBitrate),
      uv_loop_(uv_loop) {

  webrtc::GoogCcFactoryConfig config;
  // Provide RTCP feedback as well as Receiver Reports.
  config.feedback_only = false;

  controller_factory_ = new webrtc::GoogCcNetworkControllerFactory(std::move(config));

  InitializeController();
}

TransportCongestionControlClient::~TransportCongestionControlClient() {
  delete controller_factory_;
  controller_factory_ = nullptr;

  DestroyController();
}

void TransportCongestionControlClient::OnRtpPacketSend(RtpPacketPtr& rtp_packet,
                                                       int64_t now) {
  uint16_t wideSeqNumber;
  rtp_packet->ReadTransportWideCc01(wideSeqNumber);

  webrtc::RtpPacketSendInfo packetInfo;
  packetInfo.ssrc = rtp_packet->GetSsrc();
  packetInfo.transport_sequence_number = wideSeqNumber;
  packetInfo.has_rtp_sequence_number = true;
  packetInfo.rtp_sequence_number = rtp_packet->GetSequenceNumber();
  packetInfo.length = rtp_packet->GetSize();
  packetInfo.pacing_info = GetPacingInfo();

  // webrtc中发送和进入发送状态有一小段等待时间
  // 因此分开了两个函数 insert 和 sent 函数
  InsertPacket(packetInfo);
  PacketSent(packetInfo, now);
}

std::shared_ptr<RtpPacket> TransportCongestionControlClient::GeneratePadding(
    size_t target_size_bytes) {
  uint8_t data[1400u];
  std::memcpy(data, ProbationPacketHeader, sizeof(ProbationPacketHeader));
  auto probationPacket = std::make_shared<RtpPacket>(data, 1400u);
  probationPacket->SetPayloadLength(sizeof(ProbationPacketHeader));
  return probationPacket;
}

void TransportCongestionControlClient::InitializeController() {
  webrtc::BitrateConstraints bitrate_config;
  bitrate_config.min_bitrate_bps = MinBitrate;           // 最小设置30kbps;
  bitrate_config.max_bitrate_bps = MaxAvailableBitrate;  // 最大设置4mbps;
  bitrate_config.start_bitrate_bps = initial_available_bitrate_;

  rtp_transport_controller_send_ = new webrtc::RtpTransportControllerSend(
      this, nullptr, controller_factory_, bitrate_config, uv_loop_);

  rtp_transport_controller_send_->RegisterTargetTransferRateObserver(this);

  // This makes sure that periodic probing is used when the application is send
  // less bitrate than needed to measure the bandwidth estimation.  (f.e. when
  // videos are muted or using screensharing with still images)
  rtp_transport_controller_send_->EnablePeriodicAlrProbing(true);

  process_timer_ = new UvTimer(this, uv_loop_->get_loop());
  process_timer_->Start(std::min(
      // Depends on probation being done and WebRTC-Pacer-MinPacketLimitMs field trial.
      rtp_transport_controller_send_->packet_sender()->TimeUntilNextProcess(),
      // Fixed value (25ms), libwebrtc/api/transport/goog_cc_factory.cc.
      controller_factory_->GetProcessInterval().ms()));

  rtp_transport_controller_send_->OnNetworkAvailability(true);
}

void TransportCongestionControlClient::DestroyController() {
  delete rtp_transport_controller_send_;
  rtp_transport_controller_send_ = nullptr;

  delete process_timer_;
  process_timer_ = nullptr;

  rtp_transport_controller_send_->OnNetworkAvailability(false);
}

void TransportCongestionControlClient::InsertPacket(
    webrtc::RtpPacketSendInfo& packetInfo) {
  if (rtp_transport_controller_send_ == nullptr) {
    return;
  }

  rtp_transport_controller_send_->packet_sender()->InsertPacket(packetInfo.length);
  rtp_transport_controller_send_->OnAddPacket(packetInfo);
}

webrtc::PacedPacketInfo TransportCongestionControlClient::GetPacingInfo() {
  if (rtp_transport_controller_send_ == nullptr) {
    return {};
  }

  return rtp_transport_controller_send_->packet_sender()->GetPacingInfo();
}

void TransportCongestionControlClient::PacketSent(
    webrtc::RtpPacketSendInfo& packetInfo, int64_t nowMs) {
  if (rtp_transport_controller_send_ == nullptr) {
    return;
  }

  // Notify the transport feedback adapter about the sent packet.
  rtc::SentPacket sentPacket(packetInfo.transport_sequence_number, nowMs);
  rtp_transport_controller_send_->OnSentPacket(sentPacket, packetInfo.length);
}

void TransportCongestionControlClient::ReceiveEstimatedBitrate(uint32_t bitrate) {
  if (rtp_transport_controller_send_ == nullptr) {
    return;
  }

  rtp_transport_controller_send_->OnReceivedEstimatedBitrate(bitrate);
}

void TransportCongestionControlClient::ReceiveRtcpReceiverReport(
    const webrtc::RTCPReportBlock& report, float rtt, int64_t nowMs) {
  if (rtp_transport_controller_send_ == nullptr) {
    return;
  }

  rtp_transport_controller_send_->OnReceivedRtcpReceiverReport(
      {report}, static_cast<int64_t>(rtt), nowMs);
}

void TransportCongestionControlClient::ReceiveRtcpTransportFeedback(
    const FeedbackRtpTransportPacket* feedback) {
  // Update packet loss history.
  size_t expected_packets = feedback->GetPacketStatusCount();
  size_t lost_packets = 0;
  for (const auto& result : feedback->GetPacketResults()) {
    if (!result.received) lost_packets += 1;
  }
  UpdatePacketLoss(static_cast<double>(lost_packets) / expected_packets);

  if (rtp_transport_controller_send_ == nullptr) {
    return;
  }

  rtp_transport_controller_send_->OnTransportFeedback(*feedback);
}

void TransportCongestionControlClient::UpdatePacketLoss(double packetLoss) {
  // Add the score into the histogram.
  if (packet_loss_history_.size() == PacketLossHistogramLength)
    packet_loss_history_.pop_front();

  packet_loss_history_.push_back(packetLoss);

  /*
   * Scoring mechanism is a weighted average.
   *
   * The more recent the score is, the more weight it has.
   * The oldest score has a weight of 1 and subsequent scores weight is
   * increased by one sequentially.
   *
   * Ie:
   * - scores: [1,2,3,4]
   * - scores = ((1) + (2+2) + (3+3+3) + (4+4+4+4)) / 10 = 2.8 => 3
   */

  size_t weight{0};
  size_t samples{0};
  double totalPacketLoss{0};

  for (auto packetLossEntry : packet_loss_history_) {
    weight++;
    samples += weight;
    totalPacketLoss += weight * packetLossEntry;
  }

  // clang-tidy "thinks" that this can lead to division by zero but we are
  // smarter.
  // NOLINTNEXTLINE(clang-analyzer-core.DivideZero)
  packet_loss_ = totalPacketLoss / samples;
}

void TransportCongestionControlClient::RescheduleNextAvailableBitrateEvent() {
  last_available_bitrate_event_at_ms_ = uv_loop_->get_time_ms();
}

void TransportCongestionControlClient::MayEmitAvailableBitrateEvent(
    uint32_t previousAvailableBitrate) {
  uint64_t nowMs = uv_loop_->get_time_ms_int64();
  bool notify{false};

  // Ignore if first event.
  // NOTE: Otherwise it will make the Transport crash since this event also
  // happens during the constructor of this class.
  if (last_available_bitrate_event_at_ms_ == 0u) {
    last_available_bitrate_event_at_ms_ = nowMs;

    return;
  }

  // Emit if this is the first valid event.
  if (!available_bitrate_event_called_) {
    available_bitrate_event_called_ = true;
    notify = true;
  }
  // Emit event if AvailableBitrateEventInterval elapsed.
  else if (nowMs - last_available_bitrate_event_at_ms_ >= AvailableBitrateEventInterval) {
    notify = true;
  }
  // Also emit the event fast if we detect a high BWE value increase.
  else if (bitrates_.availableBitrate > previousAvailableBitrate * 1.50) { // * 0.75
    RTC_LOG(INFO) << "high BWE value increase detected, notifying the listener: "
      << "now=" << bitrates_.availableBitrate << ", before=" << previousAvailableBitrate;
    notify = true;
  }

  if (notify) {
    // RTC_LOG(INFO) << "notifying the listener with new available bitrate:" << bitrates_.availableBitrate;
    last_available_bitrate_event_at_ms_ = nowMs;
    // TODO: impl this func in class publisher
    observer_->OnTransportCongestionControlClientBitrates(this, bitrates_);
  }
}

void TransportCongestionControlClient::OnTargetTransferRate(
    webrtc::TargetTransferRate targetTransferRate) {
  // NOTE: The same value as 'initialAvailableBitrate' is received
  // periodically regardless of the real available bitrate. Skip such value
  // except for the first time this event is called.

  if (available_bitrate_event_called_ &&
      targetTransferRate.target_rate.bps() == initial_available_bitrate_) {
    return;
  }

  auto previousAvailableBitrate = bitrates_.availableBitrate;

  // Update availableBitrate.
  // NOTE: Just in case.
  if (targetTransferRate.target_rate.bps() > std::numeric_limits<uint32_t>::max())
    bitrates_.availableBitrate = std::numeric_limits<uint32_t>::max();
  else
    bitrates_.availableBitrate = targetTransferRate.target_rate.bps();

  // RTC_LOG(INFO) << "new available bitrate:" << bitrates_.availableBitrate;

  MayEmitAvailableBitrateEvent(previousAvailableBitrate);
}

// Called from PacedSender in order to send probation packets.
void TransportCongestionControlClient::SendPacket(
    RtpPacket* packet, const webrtc::PacedPacketInfo& pacingInfo) {
  // Send the packet.
  observer_->OnTransportCongestionControlClientSendRtpPacket(this, packet, pacingInfo);
}

void TransportCongestionControlClient::OnTimer(UvTimer* timer) {
  if (timer == process_timer_) {
    // Time to call RtpTransportControllerSend::Process().
    rtp_transport_controller_send_->Process();

    // Time to call PacedSender::Process().
    rtp_transport_controller_send_->packet_sender()->Process();
    process_timer_->Start(std::min<uint64_t>(
        // Depends on probation being done and
        // WebRTC-Pacer-MinPacketLimitMs
        // field trial.
        rtp_transport_controller_send_->packet_sender()
            ->TimeUntilNextProcess(),
        // Fixed value (25ms), libwebrtc/api/transport/goog_cc_factory.cc.
        controller_factory_->GetProcessInterval().ms()));

    MayEmitAvailableBitrateEvent(bitrates_.availableBitrate);
  }
}
}  // namespace bifrost
