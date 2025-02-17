/*******************************************************
 * @author      : dog head
 * @date        : Created in 2023/8/16 8:23 下午
 * @mail        : qw225967@github.com
 * @project     : .
 * @file        : bifrost_pacer.cpp
 * @description : TODO
 *******************************************************/

#include "bifrost_send_algorithm/bifrost_pacer.h"

#include <modules/rtp_rtcp/source/rtp_header_extension_size.h>
#include <modules/rtp_rtcp/source/rtp_packet_to_send.h>

#include "bifrost_send_algorithm/webrtc_clock_adapter.h"
#include "experiment_manager/fake_data_producer.h"
#include "experiment_manager/h264_file_data_producer.h"
using namespace webrtc::media_optimization;
namespace bifrost {

constexpr uint16_t DefaultCreatePacketTimeInterval = 10u;  // 每10ms创建10个包
constexpr uint16_t DefaultStatisticsTimerInterval = 1000u;  // 每1s统计一次
constexpr uint16_t DefaultPacingTimeInterval = 5u;
const uint32_t InitialPacingGccBitrate = 200000u;  // 配合当前测试的码率一半左右开始探测 780

uint32_t BifrostPacer::MaxPacingDataLimit = 4096000;

BifrostPacer::BifrostPacer(uint32_t ssrc, uint32_t flexfec_ssrc,
                           UvLoop* uv_loop, Observer* observer)
    : uv_loop_(uv_loop),
      observer_(observer),
      pacer_timer_interval_(DefaultPacingTimeInterval),
      pacing_rate_(InitialPacingGccBitrate),
      flexfec_ssrc_(flexfec_ssrc) {
  // 1.数据生产者
#ifdef USE_FAKE_DATA_PRODUCER
  data_producer_ = std::make_shared<FakeDataProducer>(ssrc);
#else
  data_producer_ = std::make_shared<H264FileDataProducer>(ssrc, uv_loop);
#endif

  // 2.发送定时器
  pacer_timer_ = new UvTimer(this, uv_loop->get_loop());
  pacer_timer_->Start(pacer_timer_interval_);

  create_timer_ = new UvTimer(this, uv_loop->get_loop());
  create_timer_->Start(DefaultCreatePacketTimeInterval,
                       DefaultCreatePacketTimeInterval);

  statistics_timer_ = new UvTimer(this, uv_loop->get_loop());
  statistics_timer_->Start(DefaultStatisticsTimerInterval,
                           DefaultStatisticsTimerInterval);

#ifdef USE_FLEX_FEC_PROTECT
  // flexfec sender new
  std::vector<webrtc::RtpExtension> vec_ext;
  vec_ext.emplace_back(webrtc::RtpExtension(
      webrtc::TransportSequenceNumber::kUri,
      webrtc::RTPExtensionType::kRtpExtensionTransportSequenceNumber));
  rtc::ArrayView<const webrtc::RtpExtensionSize> size;
  clock_ = new WebRTCClockAdapter(uv_loop);
  flexfec_sender_ = std::make_unique<webrtc::FlexfecSender>(
      110, flexfec_ssrc, ssrc, "", vec_ext, size, nullptr, clock_);

  loss_prot_logic_ = std::make_unique<VCMLossProtectionLogic>(clock_->TimeInMilliseconds());

  // 开启保护模式，nack+fec
  this->SetProtectionMethod(true, true);
#endif
}

BifrostPacer::~BifrostPacer() {
  delete pacer_timer_;
  delete create_timer_;
  delete statistics_timer_;
  delete clock_;
}

void BifrostPacer::SetProtectionMethod(bool enable_fec, bool enable_nack) {
  VCMProtectionMethodEnum method(kNone);
  if (enable_fec && enable_nack) {
    method = kNackFec;
  } else if (enable_nack) {
    method = kNack;
  } else if (enable_fec) {
    method = kFec;
  }

  loss_prot_logic_->SetMethod(method);
}

void BifrostPacer::UpdateFecRates(uint8_t fraction_lost,
                                  int64_t round_trip_time_ms) {
#ifdef USE_FLEX_FEC_PROTECT
  float target_bitrate_kbps = static_cast<float>(this->target_bitrate_) / 1000.f;

  // Sanity check.
  if (this->last_pacing_frame_rate_ < 1.0) {
    this->last_pacing_frame_rate_ = 1.0;
  }

  {
    this->loss_prot_logic_->UpdateBitRate(target_bitrate_kbps);
    this->loss_prot_logic_->UpdateRtt(round_trip_time_ms);
    // Update frame rate for the loss protection logic class: frame rate should
    // be the actual/sent rate.
    loss_prot_logic_->UpdateFrameRate(last_pacing_frame_rate_);
    // Returns the filtered packet loss, used for the protection setting.
    // The filtered loss may be the received loss (no filter), or some
    // filtered value (average or max window filter).
    // Use max window filter for now.
    FilterPacketLossMode filter_mode = kMaxFilter;
    uint8_t packet_loss_enc = loss_prot_logic_->FilteredLoss(
        clock_->TimeInMilliseconds(), filter_mode, fraction_lost);
    // For now use the filtered loss for computing the robustness settings.
    loss_prot_logic_->UpdateFilteredLossPr(packet_loss_enc);
    if (loss_prot_logic_->SelectedType() == kNone) {
      return;
    }
    // Update method will compute the robustness settings for the given
    // protection method and the overhead cost
    // the protection method is set by the user via SetVideoProtection.
    loss_prot_logic_->UpdateMethod();
    // Get the bit cost of protection method, based on the amount of
    // overhead data actually transmitted (including headers) the last
    // second.
    // Get the FEC code rate for Key frames (set to 0 when NA).
    auto method = loss_prot_logic_->SelectedMethod();
    key_fec_params_.fec_rate = method->RequiredProtectionFactorK();
    // Get the FEC code rate for Delta frames (set to 0 when NA).
    delta_fec_params_.fec_rate = method->RequiredProtectionFactorD();
    // The RTP module currently requires the same |max_fec_frames| for both
    // key and delta frames.
    delta_fec_params_.max_fec_frames = method->MaxFramesFec();
    key_fec_params_.max_fec_frames = method->MaxFramesFec();
  }
  // Set the FEC packet mask type. |kFecMaskBursty| is more effective for
  // consecutive losses and little/no packet re-ordering. As we currently
  // do not have feedback data on the degree of correlated losses and packet
  // re-ordering, we keep default setting to |kFecMaskRandom| for now.
  delta_fec_params_.fec_mask_type = webrtc::kFecMaskRandom;
  key_fec_params_.fec_mask_type = webrtc::kFecMaskRandom;

  // 随便设置一个
  if (flexfec_sender_) {
    flexfec_sender_->SetFecParameters(delta_fec_params_);
    RTC_LOG(INFO) << "fec rate:" << delta_fec_params_.fec_rate
              << ", max frame rate:" << delta_fec_params_.max_fec_frames
              << ", fraction lost:" << uint32_t(fraction_lost);
  }
#endif
}

void BifrostPacer::TryFlexFecPacketSend(RtpPacketPtr& packet) {
  if (flexfec_sender_ && loss_prot_logic_ && clock_ &&
      packet->GetSsrc() == flexfec_sender_->protected_media_ssrc()) {
    webrtc::RtpPacketToSend webrtc_packet(nullptr);
    if (!webrtc_packet.Parse(packet->GetData(), packet->GetSize())) 
      return;

    flexfec_sender_->AddRtpPacketAndGenerateFec(webrtc_packet);
    auto vec_s = flexfec_sender_->GetFecPackets();
    if (vec_s.empty()) 
      return;
    for (auto iter = vec_s.begin(); iter != vec_s.end(); iter++) {
      (*iter)->SetExtension<webrtc::TransportSequenceNumber>(0);
      RtpPacketPtr rtp_packet = std::make_shared<RtpPacket>((*iter)->data(), (*iter)->size());
      this->ready_send_vec_.emplace_back(std::make_pair(rtp_packet, FEC));
    }
  }
}

void BifrostPacer::OnTimer(UvTimer* timer) {
  if (timer == pacer_timer_) {
    if (this->pacing_congestion_windows_ > 0 && this->bytes_in_flight_ > 0 &&
        this->pacing_congestion_windows_ < this->bytes_in_flight_) {
    } else {
      // bbr的需要完全贴合pacing。
      // 1.5的原因是在聚合ack的实现中，pacing_rate会因为确认之后偏小，这里拍脑袋给加大点
      
      // gcc 的pacing
      // 2.5则是根据码率浮动的，这里如果没有使用真实的h264文件发数据，可以改小
      double pacing_gain = 2.5;
      if (this->pacing_congestion_windows_ > 0 && this->bytes_in_flight_ > 0)
        pacing_gain = 1.5;

      int32_t interval_pacing_bytes = pre_remainder_bytes_ + /* 上个周期剩余bytes */
          int32_t((pacing_rate_ * pacing_gain / 1000) /* 转换ms */
                  * pacer_timer_interval_ /* 该间隔发送速率 */ /
                  8 /* 转换bytes */);

      auto ite = ready_send_vec_.begin();
      while (ite != ready_send_vec_.end() && interval_pacing_bytes > 0) {
        auto pair = (*ite);
        auto packet = pair.first;
        if (packet == nullptr) {
          RTC_LOG(WARNING) << "[pacer send] packet is nullptr";
          ite = ready_send_vec_.erase(ite);
          continue;
        }
        // 发送时更新tcc拓展序号，nack的rtp和普通rtp序号是连续的
        if (packet->UpdateTransportWideCc01(this->tcc_seq_)) {
          this->tcc_seq_++;

          if (packet->HasMarker()) count_pacing_frame_rate_++;

          // 区分类型发送
          switch (pair.second) {
            case RTP:
            case FEC: {
              observer_->OnPublisherSendPacket(packet);
              break;
            }
            case NACK: {
              observer_->OnPublisherSendReTransPacket(packet);
              break;
            }
            default:
              break;
          }
        }

        interval_pacing_bytes -= int32_t(packet->GetSize());

        // 统计相关
        pacing_bitrate_ += packet->GetSize() * 8;
        pacing_bytes_ += packet->GetSize();
        pacing_packet_count_++;

        ite = ready_send_vec_.erase(ite);
      }
      pre_remainder_bytes_ = interval_pacing_bytes;
    }
    pacer_timer_->Start(pacer_timer_interval_);
  }

  if (timer == create_timer_) {
    delta_fec_params_.fec_rate = 255;
    delta_fec_params_.max_fec_frames = 1;
#ifdef USE_FLEX_FEC_PROTECT
    flexfec_sender_->SetFecParameters(delta_fec_params_);                                                        
#endif
    // 每10ms产生3次
    for (int i = 0; i < 5; i++) {
      auto packet = this->data_producer_->CreateData();
      if (packet == nullptr) continue;
#ifdef USE_FLEX_FEC_PROTECT
      this->TryFlexFecPacketSend(packet);
#endif
      this->ready_send_vec_.emplace_back(std::make_pair(packet, RTP));
    }
  }

  if (timer == statistics_timer_) {
    pacing_bitrate_bps_ = pacing_bitrate_;
    pacing_bitrate_ = 0;
    last_pacing_frame_rate_ = count_pacing_frame_rate_;
    count_pacing_frame_rate_ = 0;
  }
}
}  // namespace bifrost
