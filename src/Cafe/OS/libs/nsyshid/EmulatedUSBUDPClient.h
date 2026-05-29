#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

#include "EmulatedUSBMessages.h"
#include "util/helpers/ConcurrentQueue.h"

namespace nsyshid::emulated_usb_udp
{
	class EmulatedUSBUDPClient
	{
	  public:
		struct Settings
		{
			std::string host;
			uint16_t port;
		};

		struct ReadResult
		{
			Status status;
			uint32_t packet_index;
			std::vector<uint8_t> data;
		};

		struct WriteResult
		{
			Status status;
			uint32_t bytes_written;
		};

		struct DescriptorResult
		{
			Status status;
			std::vector<uint8_t> data;
		};

		explicit EmulatedUSBUDPClient(Settings settings);
		~EmulatedUSBUDPClient();

		bool Start();
		void Stop();
		bool IsRunning() const;

		std::optional<DeviceInfoPayload> RequestDeviceInfo(std::chrono::milliseconds timeout);
		bool RequestOpen(std::chrono::milliseconds timeout);
		bool RequestClose(std::chrono::milliseconds timeout);
		std::optional<ReadResult> RequestRead(uint32_t length, std::chrono::milliseconds timeout);
		std::optional<WriteResult> RequestWrite(const uint8_t* data, uint32_t length, std::chrono::milliseconds timeout);
		std::optional<DescriptorResult> RequestGetDescriptor(uint8_t desc_type, uint8_t desc_index, uint16_t lang,
															 uint32_t max_length, std::chrono::milliseconds timeout);
		bool RequestSetIdle(uint8_t if_index, uint8_t report_id, uint8_t duration, std::chrono::milliseconds timeout);
		bool RequestSetProtocol(uint8_t if_index, uint8_t protocol, std::chrono::milliseconds timeout);
		bool RequestSetReport(uint8_t report_type, uint8_t report_id, const uint8_t* data, uint32_t length,
							  std::chrono::milliseconds timeout);

	  private:
		struct PendingResponse
		{
			explicit PendingResponse(MessageType expected_type);
			MessageType expected_type;
			std::vector<uint8_t> payload;
			bool ready = false;
			std::mutex mutex;
			std::condition_variable condition;
		};

		std::optional<std::vector<uint8_t>> SendRequestAndWait(std::vector<uint8_t> packet, uint32_t request_id,
															   MessageType expected_type, std::chrono::milliseconds timeout);
		uint32_t NextRequestId();

		void ReaderThread();
		void WriterThread();
		void WakeReader();

		Settings m_settings;
		std::atomic_bool m_running{false};
		std::thread m_reader_thread;
		std::thread m_writer_thread;

		uint32_t m_uid;
		std::atomic<uint32_t> m_request_id{0};
		boost::asio::io_context m_io_service;
		boost::asio::ip::udp::endpoint m_receiver_endpoint;
		boost::asio::ip::udp::socket m_socket;
		boost::asio::ip::udp::endpoint m_socket_wakeup_endpoint;

		ConcurrentQueue<std::shared_ptr<std::vector<uint8_t>>> m_writer_jobs;

		mutable std::mutex m_pending_mutex;
		std::unordered_map<uint32_t, std::shared_ptr<PendingResponse>> m_pending;

		mutable std::mutex m_latest_read_mutex;
		std::vector<uint8_t> m_latest_read;
		uint32_t m_latest_read_packet_index = 0;
	};
} // namespace nsyshid::emulated_usb_udp