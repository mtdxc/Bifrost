/*******************************************************
 * @author      : dog head
 * @date        : Created in 2023/6/20 9:55 上午
 * @mail        : qw225967@github.com
 * @project     : worker
 * @file        : publisher.cpp
 * @description : TODO
 *******************************************************/

#include "publisher.h"

#include "rtcp_compound_packet.h"

namespace bifrost {

const uint32_t IntervalUpdatePacingInfo = 100u;
const uint32_t IntervalSendReport = 2000u;

Publisher::Publisher(Settings::Configuration& remote_config, UvLoop* uv_loop,
                     Observer* observer, uint8_t number,
                     ExperimentManagerPtr& experiment_manager,
                     quic::CongestionControlType congestion_type)
    : remote_addr_config_(remote_config),
      uv_loop_(uv_loop),
      observer_(observer),
      rtt_(20),
      experiment_manager_(experiment_manager),
      number_(number) {
  std::cout << "publish experiment manager:" << experiment_manager << std::endl;

  // 1.remote address set
  auto remote_addr = Settings::get_sockaddr_by_config(remote_config);
  this->udp_remote_address_ = std::make_shared<sockaddr>(remote_addr);

  // 3.timer start
  this->send_report_timer_ = new UvTimer(this, this->uv_loop_->get_loop());
  this->send_report_timer_->Start(IntervalSendReport, IntervalSendReport);

  this->update_pacing_info_timer_ = new UvTimer(this, this->uv_loop_->get_loop());
  this->update_pacing_info_timer_->Start(IntervalUpdatePacingInfo, IntervalUpdatePacingInfo);

  // 4.nack
  nack_ = std::make_shared<Nack>(remote_addr_config_.ssrc, uv_loop);
  fec_nack_ = std::make_shared<Nack>(remote_addr_config_.flexfec_ssrc, uv_loop);

  // 5.ssrc
  ssrc_ = remote_addr_config_.ssrc;
  fec_ssrc_ = remote_addr_config_.flexfec_ssrc;

  // 6.send algorithm
  bifrost_send_algorithm_manager_ =
      std::make_shared<BifrostSendAlgorithmManager>(congestion_type, uv_loop);

  // 7.pacer
  pacer_ = std::make_shared<BifrostPacer>(
      ssrc_, remote_addr_config_.flexfec_ssrc, uv_loop, this);
}

void Publisher::OnReceiveRtcpFeedback(FeedbackRtpPacket* fb) {
  if (bifrost_send_algorithm_manager_->OnReceiveRtcpFeedback(fb)) {
    auto pacing_rate = bifrost_send_algorithm_manager_->get_pacing_rate(
        this->bifrost_send_algorithm_manager_->get_bytes_in_flight());
    if (pacing_rate > 0) this->pacer_->set_pacing_rate(pacing_rate);

    this->pacer_->set_pacing_congestion_windows(
        this->bifrost_send_algorithm_manager_->get_congestion_windows());
    this->pacer_->set_bytes_in_flight(
        this->bifrost_send_algorithm_manager_->get_bytes_in_flight());
    this->pacer_->set_pacing_transfer_time(
        this->bifrost_send_algorithm_manager_->get_pacing_transfer_time(
            this->bifrost_send_algorithm_manager_->get_congestion_windows()));

    send_packet_bytes_ = pacer_->get_pacing_bytes();

    // 投递数据落地
    ExperimentDumpData data(
        bifrost_send_algorithm_manager_->get_pacing_rate(
            this->bifrost_send_algorithm_manager_->get_bytes_in_flight()),
        pacer_->get_pacing_bitrate_bps(),
        bifrost_send_algorithm_manager_->get_trends());
    experiment_manager_->PostGccDataToShow(this->number_, data);
  }
}

void Publisher::OnReceiveNack(FeedbackRtpNackPacket* packet) {
  std::vector<RtpPacketPtr> packets;
  if (ssrc_ == packet->GetMediaSsrc())
    this->nack_->ReceiveNack(packet, packets);
  else if (fec_ssrc_ == packet->GetMediaSsrc())
    this->fec_nack_->ReceiveNack(packet, packets);
  else
    std::cout << std::endl;

  auto ite = packets.begin();
  while (ite != packets.end()) {
    this->pacer_->NackReadyToSendPacket(*ite);
    ite = packets.erase(ite);
  }
}

void Publisher::OnReceiveReceiverReport(ReceiverReport* report) {
  // Get the NTP representation of the current timestamp.
  uint64_t nowMs = this->uv_loop_->get_time_ms();
  auto ntp = Time::TimeMs2Ntp(nowMs);

  // Get the compact NTP representation of the current timestamp.
  uint32_t compactNtp = (ntp.seconds & 0x0000FFFF) << 16;

  compactNtp |= (ntp.fractions & 0xFFFF0000) >> 16;

  uint32_t lastSr = report->GetLastSenderReport();
  uint32_t dlsr = report->GetDelaySinceLastSenderReport();

  // RTT in 1/2^16 second fractions.
  uint32_t rtt{0};

  // If no Sender Report was received by the remote endpoint yet, ignore lastSr
  // and dlsr values in the Receiver Report.
  if (lastSr && (compactNtp > dlsr + lastSr)) rtt = compactNtp - dlsr - lastSr;

  // RTT in milliseconds.
  this->rtt_ = static_cast<float>(rtt >> 16) * 1000;
  this->rtt_ += (static_cast<float>(rtt & 0x0000FFFF) / 65536) * 1000;

  webrtc::RTCPReportBlock webrtc_report;
  webrtc_report.last_sender_report_timestamp = report->GetLastSenderReport();
  webrtc_report.source_ssrc = report->GetSsrc();
  webrtc_report.jitter = report->GetDelaySinceLastSenderReport();
  webrtc_report.fraction_lost = report->GetFractionLost();
  webrtc_report.packets_lost = report->GetTotalLost();
  //
  //  std::cout << "receive rr "
  //            << "last_sender_report_timestamp:" <<
  //            report->GetLastSenderReport()
  //            << ", ssrc:" << report->GetSsrc()
  //            << ", jitter:" << report->GetDelaySinceLastSenderReport()
  //            << ", fraction_lost:" << uint32_t(report->GetFractionLost())
  //            << ", packets_lost:" << report->GetTotalLost()
  //            << ", rtt:" << this->rtt_ << std::endl;
  ExperimentDumpData data(webrtc_report.jitter, webrtc_report.fraction_lost,
                          webrtc_report.packets_lost, this->rtt_);

  experiment_manager_->PostRRDataToShow(this->number_, data);

  this->nack_->UpdateRtt(uint32_t(this->rtt_));
  this->fec_nack_->UpdateRtt(uint32_t(this->rtt_));

  this->bifrost_send_algorithm_manager_->UpdateRtt(this->rtt_);
  this->bifrost_send_algorithm_manager_->OnReceiveReceiverReport(
      webrtc_report, this->rtt_, nowMs);

  this->pacer_->UpdateFecRates(webrtc_report.fraction_lost, this->rtt_);
}

SenderReport* Publisher::GetRtcpSenderReport(uint64_t nowMs) const {
  auto ntp = Time::TimeMs2Ntp(nowMs);
  auto* report = new SenderReport();

  // Calculate TS difference between now and maxPacketMs.
  auto diffMs = nowMs - this->max_packet_ms_;
  auto diffTs = diffMs * 90000 / 1000;  // 现实中常用的采样换算此处写死 90000 - 视频

  report->SetSsrc(this->ssrc_);
  report->SetPacketCount(this->send_packet_count_);
  report->SetOctetCount(this->send_packet_bytes_);
  report->SetNtpSec(ntp.seconds);
  report->SetNtpFrac(ntp.fractions);
  report->SetRtpTs(this->max_packet_ts_ + diffTs);

  return report;
}

void Publisher::OnTimer(UvTimer* timer) {
  if (timer == this->send_report_timer_) {
    // 立刻回复sr
    uint64_t now = this->uv_loop_->get_time_ms_int64();
    std::shared_ptr<CompoundPacket> packet = std::make_shared<CompoundPacket>();
    auto* report = GetRtcpSenderReport(now);
    packet->AddSenderReport(report);
    packet->Serialize(Buffer);
    this->observer_->OnPublisherSendRtcpPacket(packet, this->udp_remote_address_.get());
  }

  if (timer == this->update_pacing_info_timer_) {
    auto pacing_rate = bifrost_send_algorithm_manager_->get_pacing_rate(
        this->bifrost_send_algorithm_manager_->get_bytes_in_flight());
    if (pacing_rate > 0) this->pacer_->set_pacing_rate(pacing_rate);

    this->pacer_->set_pacing_congestion_windows(
        this->bifrost_send_algorithm_manager_->get_congestion_windows());
    this->pacer_->set_bytes_in_flight(
        this->bifrost_send_algorithm_manager_->get_bytes_in_flight());
    this->pacer_->set_pacing_transfer_time(
        this->bifrost_send_algorithm_manager_->get_pacing_transfer_time(
            this->bifrost_send_algorithm_manager_->get_congestion_windows()));

    send_packet_bytes_ = pacer_->get_pacing_bytes();
  }
}
}  // namespace bifrost