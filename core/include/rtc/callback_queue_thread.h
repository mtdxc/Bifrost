/*******************************************************
 * @author      : dog head
 * @date        : Created in 2024/11/8 11:28
 * @mail        : fangruiqian.frq@alibaba-inc.com
 * @project     : TEMP-PROJECT-FLAG
 * @file        : callback_queue_thread.h
 * @description : TODO
 *******************************************************/

#ifndef DEMO_TEST_CALLBACK_QUEUE_THREAD_H
#define DEMO_TEST_CALLBACK_QUEUE_THREAD_H

#include "spdlog/spdlog.h"
#include "utils/copy_on_write_buffer.h"
#include "utils/time_handler.h"
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
namespace RTC {
		void sleep(int ms);
		bool IsLargeSeq(uint16_t small1, uint16_t big, bool& has_routing);
		class ThreadRtpPacketQueue {
		private:
				std::map<uint32_t, RTCUtils::CopyOnWriteBuffer> packet_map_;
				mutable std::mutex mutex_;
				std::condition_variable is_empty_;
				std::condition_variable is_disorder_;
				bool is_start_{ true };

				uint32_t cycle_{ 0 };
				uint16_t max_seq_{ 0 };
				uint32_t waite_pop_seq_{ 0 };

		public:
				ThreadRtpPacketQueue() = default;

				// 禁止复制和赋值
				ThreadRtpPacketQueue(const ThreadRtpPacketQueue&)            = delete;
				ThreadRtpPacketQueue& operator=(const ThreadRtpPacketQueue&) = delete;

				void ResetPopSeq() {
						this->waite_pop_seq_ = 0;
				}

				// 入队操作
				void Push(uint16_t seq, const RTCUtils::CopyOnWriteBuffer& packet_buffer);

				// 出队操作
				bool Pop(std::vector<RTCUtils::CopyOnWriteBuffer>& packet_buffers);
				bool FindContinuousPacket();

				// 检查队列是否为空
				bool Empty() const {
						std::lock_guard<std::mutex> lock(mutex_);
						return packet_map_.empty();
				}
		};

		class CallBackQueueThread : public RTCUtils::TimerHandle::Listener {
		public:
				class Listener {
				public:
						virtual void HasContinuousPacketCall(RTCUtils::CopyOnWriteBuffer buffer)
						    = 0;
						virtual void ReceiveDataFinishCall() = 0;
				};

		public:
				CallBackQueueThread(
				    Listener* listener, std::shared_ptr<CoreIO::NetworkThread> thread);
				~CallBackQueueThread();

				void ResetFrameSequence() {
						this->thread_queue_->ResetPopSeq();
				}

				void ReceiveRtpPacket(uint16_t seq,
				                      const RTCUtils::CopyOnWriteBuffer& buffer);

				void OnTimer(RTCUtils::TimerHandle* timer) override;

		private:
				void TryCallData();

		private:
				volatile uint16_t wait_pop_frames_{ 0 };
				std::unique_ptr<std::thread> thread_;
				std::unique_ptr<ThreadRtpPacketQueue> thread_queue_;
				Listener* listener_{ nullptr };
				volatile bool running_{ true };

				RTCUtils::TimerHandle* check_frame_timer_{ nullptr };
		};
} // namespace RTC

#endif // DEMO_TEST_CALLBACK_QUEUE_THREAD_H
