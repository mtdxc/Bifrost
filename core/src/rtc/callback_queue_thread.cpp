/*******************************************************
 * @author      : dog head
 * @date        : Created in 2024/11/8 14:30
 * @mail        : fangruiqian.frq@alibaba-inc.com
 * @project     : TEMP-PROJECT-FLAG
 * @file        : callback_queue_thread.cpp
 * @description : TODO
 *******************************************************/

#include "rtc/callback_queue_thread.h"

namespace RTC {
	void sleep(int ms) {
#ifdef _WIN32
		Sleep(ms);
#else	
		usleep(ms * 1000);
#endif
	}

		bool IsLargeSeq(uint16_t small1, uint16_t big, bool& has_routing) {
				if (big > small1) {
						if (big - small1 > 65535 / 2) {
								return false; // 异常情况，翻转跳跃了一个很大的数
						} else {
								return true; // 正常情况
						}
				} else {
						if (small1 - big > 65535 / 2) {
								has_routing = true;
								return true; // 正常情况 small > big, 数据翻转到从 0 开始，因此也是大数
						} else {
								return false; // 异常情况 small > big,
								              // 小于65535/2那么这个数可能跳跃了很大一个数
						}
				}
		}	

// 入队操作
		void ThreadRtpPacketQueue::Push(uint16_t seq, const RTCUtils::CopyOnWriteBuffer& packet_buffer) {
				std::lock_guard<std::mutex> lock(mutex_);
				if (this->is_start_) {
						this->max_seq_  = seq;
						this->is_start_ = false;
				}

				bool has_routing = false;
				if (IsLargeSeq(max_seq_, seq, has_routing)) {
						max_seq_ = seq;
				}
				if (has_routing) {
						this->cycle_++;
				}
				uint32_t large_seq = (cycle_ << 16) + seq;
				SPDLOG_INFO("push seq:{}", large_seq);
				packet_map_.emplace(large_seq, packet_buffer);
				is_empty_.notify_one(); // 通知一个等待的出队线程

				this->FindContinuousPacket();
		}

		// 出队操作
		bool ThreadRtpPacketQueue::Pop(std::vector<RTCUtils::CopyOnWriteBuffer>& packet_buffers) {
				std::unique_lock<std::mutex> lock(mutex_);
				is_empty_.wait(lock, [this] {
						return !packet_map_.empty();
				}); // 等待直到队列不为空
				if (packet_map_.empty()) {
						return false;
				}

				// 只有第一个弹出后，才需要确定有序情况
				is_disorder_.wait(lock);

				auto begin = this->packet_map_.begin();
				while (begin != this->packet_map_.end()
						&& waite_pop_seq_ == begin->first)
				{
						packet_buffers.push_back(std::move(begin->second));
						waite_pop_seq_ = waite_pop_seq_ + 1;
						SPDLOG_INFO("pop seq:{}", begin->first);
						begin = this->packet_map_.erase(begin);
						sleep(1);
				}

				return true;
		}

		bool ThreadRtpPacketQueue::FindContinuousPacket() {
				if (packet_map_.empty()) {
						return false;
				}

				auto begin = this->packet_map_.begin();
				if (begin->first == waite_pop_seq_) {
						is_disorder_.notify_one();
						return true;
				}
				return false;
		}

		static uint16_t kCheckFrameHasCall = 40u; // ms
		CallBackQueueThread::CallBackQueueThread(
		    Listener* listener, std::shared_ptr<CoreIO::NetworkThread> network_thread)
		    : listener_(listener),
		      check_frame_timer_(new RTCUtils::TimerHandle(this, network_thread)) {
				this->check_frame_timer_->InitInvoke();
				this->check_frame_timer_->StartInvoke(kCheckFrameHasCall, kCheckFrameHasCall);

				this->thread_queue_ = Cpp11Adaptor::make_unique<ThreadRtpPacketQueue>();

				thread_ = Cpp11Adaptor::make_unique<std::thread>(
				    [this]() { this->TryCallData(); });
		}

		CallBackQueueThread::~CallBackQueueThread() {
				this->check_frame_timer_->StopInvoke();
				this->check_frame_timer_->CloseInvoke();
				delete this->check_frame_timer_;

				this->running_ = false;

				if (thread_) {
						thread_->join();
				}
		}

		void CallBackQueueThread::ReceiveRtpPacket(
		    uint16_t seq, const RTCUtils::CopyOnWriteBuffer& buffer) {
				this->thread_queue_->Push(seq, buffer);
				wait_pop_frames_ = wait_pop_frames_ + 1;
		}

		void CallBackQueueThread::TryCallData() {
				while (this->running_) {
						std::vector<RTCUtils::CopyOnWriteBuffer> buffers;
						if (this->thread_queue_->Pop(buffers)) {
								auto buffer = buffers.begin();
								while (buffer != buffers.end()) {
										this->listener_->HasContinuousPacketCall(*buffer);
										sleep(40);
										wait_pop_frames_ = wait_pop_frames_ - 1;
										buffer++;
								}
						}
				}
		}


		void CallBackQueueThread::OnTimer(RTCUtils::TimerHandle* timer) {
				if (timer == this->check_frame_timer_) {
						if (this->thread_queue_->FindContinuousPacket()) {
								printf("[rtc] ontimer find continuous \n");
						}
				}
		}
} // namespace RTC
