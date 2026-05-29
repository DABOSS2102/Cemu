#include "EmulatedUSBUDPClient.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <utility>

#include <fmt/format.h>

#include "util/helpers/helpers.h"

namespace nsyshid::emulated_usb_udp
{
	namespace
	{
		constexpr auto kDefaultTimeout = std::chrono::milliseconds(1000);
		constexpr auto kReadTimeout = std::chrono::milliseconds(1500);
		constexpr auto kDeviceInfoTimeout = std::chrono::milliseconds(2000);
		constexpr size_t kMaxPacketSize = 2048;

		std::optional<Status> ParseStatus(const std::vector<uint8_t>& payload)
		{
			if (payload.size() < sizeof(StatusResponse))
				return std::nullopt;
			const auto* response = reinterpret_cast<const StatusResponse*>(payload.data());
			return response->status;
		}
	} // namespace

	EmulatedUSBUDPClient::PendingResponse::PendingResponse(MessageType expected_type)
		: expected_type(expected_type)
	{
	}

	EmulatedUSBUDPClient::EmulatedUSBUDPClient(Settings settings)
		: m_settings(std::move(settings)),
		  m_uid(rand()),
		  m_socket(m_io_service)
	{
	}

	EmulatedUSBUDPClient::~EmulatedUSBUDPClient()
	{
		Stop();
	}

	bool EmulatedUSBUDPClient::Start()
	{
		if (m_running.load(std::memory_order_relaxed))
			return true;

		try
		{
			using namespace boost::asio;

			ip::udp::resolver resolver(m_io_service);
			m_receiver_endpoint = *resolver.resolve(m_settings.host, fmt::format("{}", m_settings.port)).cbegin();

			if (m_socket.is_open())
				m_socket.close();

			m_socket.open(ip::udp::v4());
			m_socket.bind(ip::udp::endpoint(ip::udp::v4(), 0));
			m_socket_wakeup_endpoint = ip::udp::endpoint(ip::udp::address_v4::loopback(), m_socket.local_endpoint().port());
			m_running = true;
			m_reader_thread = std::thread(&EmulatedUSBUDPClient::ReaderThread, this);
			m_writer_thread = std::thread(&EmulatedUSBUDPClient::WriterThread, this);
			return true;
		} catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "EmulatedUSBUDPClient start failed: {}", ex.what());
			m_running = false;
			return false;
		}
	}

	void EmulatedUSBUDPClient::Stop()
	{
		if (!m_running.load(std::memory_order_relaxed))
			return;

		m_running = false;
		WakeReader();
		if (m_reader_thread.joinable())
			m_reader_thread.join();
		m_writer_jobs.push(nullptr);
		if (m_writer_thread.joinable())
			m_writer_thread.join();
		if (m_socket.is_open())
			m_socket.close();
	}

	bool EmulatedUSBUDPClient::IsRunning() const
	{
		return m_running.load(std::memory_order_relaxed);
	}

	std::optional<DeviceInfoPayload> EmulatedUSBUDPClient::RequestDeviceInfo(std::chrono::milliseconds timeout)
	{
		if (!m_running.load(std::memory_order_relaxed))
			return std::nullopt;

		const uint32_t request_id = NextRequestId();
		auto response = SendRequestAndWait(BuildDeviceInfoRequest(m_uid, request_id), request_id,
										   MessageType::DeviceInfoResponse,
										   timeout.count() > 0 ? timeout : kDeviceInfoTimeout);
		if (!response || response->size() < sizeof(DeviceInfoResponse))
			return std::nullopt;

		const auto* payload = reinterpret_cast<const DeviceInfoResponse*>(response->data());
		if (payload->status != Status::Success)
			return std::nullopt;
		return payload->info;
	}

	bool EmulatedUSBUDPClient::RequestOpen(std::chrono::milliseconds timeout)
	{
		if (!m_running.load(std::memory_order_relaxed))
			return false;

		const uint32_t request_id = NextRequestId();
		auto response = SendRequestAndWait(BuildOpenRequest(m_uid, request_id), request_id,
										   MessageType::OpenResponse,
										   timeout.count() > 0 ? timeout : kDefaultTimeout);
		if (!response)
			return false;
		const auto status = ParseStatus(*response);
		return status && *status == Status::Success;
	}

	bool EmulatedUSBUDPClient::RequestClose(std::chrono::milliseconds timeout)
	{
		if (!m_running.load(std::memory_order_relaxed))
			return false;

		const uint32_t request_id = NextRequestId();
		auto response = SendRequestAndWait(BuildCloseRequest(m_uid, request_id), request_id,
										   MessageType::CloseResponse,
										   timeout.count() > 0 ? timeout : kDefaultTimeout);
		if (!response)
			return false;
		const auto status = ParseStatus(*response);
		return status && *status == Status::Success;
	}

	std::optional<EmulatedUSBUDPClient::ReadResult> EmulatedUSBUDPClient::RequestRead(uint32_t length, std::chrono::milliseconds timeout)
	{
		if (!m_running.load(std::memory_order_relaxed))
			return std::nullopt;

		const uint32_t request_id = NextRequestId();
		auto response = SendRequestAndWait(BuildReadRequest(m_uid, request_id, length, static_cast<uint32_t>(timeout.count())),
										   request_id, MessageType::ReadResponse,
										   timeout.count() > 0 ? timeout : kReadTimeout);
		if (!response || response->size() < sizeof(ReadResponseHeader))
			return std::nullopt;

		const auto* payload = reinterpret_cast<const ReadResponseHeader*>(response->data());
		const size_t expected = sizeof(ReadResponseHeader) + payload->data_length;
		if (response->size() < expected)
			return std::nullopt;

		ReadResult result{payload->status, payload->packet_index, {}};
		if (payload->data_length > 0)
		{
			result.data.assign(response->data() + sizeof(ReadResponseHeader),
							   response->data() + sizeof(ReadResponseHeader) + payload->data_length);
		}
		return result;
	}

	std::optional<EmulatedUSBUDPClient::WriteResult> EmulatedUSBUDPClient::RequestWrite(const uint8_t* data, uint32_t length,
																						std::chrono::milliseconds timeout)
	{
		if (!m_running.load(std::memory_order_relaxed))
			return std::nullopt;

		const uint32_t request_id = NextRequestId();
		auto response = SendRequestAndWait(BuildWriteRequest(m_uid, request_id, data, length), request_id,
										   MessageType::WriteResponse,
										   timeout.count() > 0 ? timeout : kDefaultTimeout);
		if (!response || response->size() < sizeof(WriteResponse))
			return std::nullopt;

		const auto* payload = reinterpret_cast<const WriteResponse*>(response->data());
		return WriteResult{payload->status, payload->bytes_written};
	}

	std::optional<EmulatedUSBUDPClient::DescriptorResult> EmulatedUSBUDPClient::RequestGetDescriptor(
		uint8_t desc_type, uint8_t desc_index, uint16_t lang, uint32_t max_length, std::chrono::milliseconds timeout)
	{
		if (!m_running.load(std::memory_order_relaxed))
			return std::nullopt;

		const uint32_t request_id = NextRequestId();
		auto response = SendRequestAndWait(BuildGetDescriptorRequest(m_uid, request_id, desc_type, desc_index, lang, max_length),
										   request_id, MessageType::GetDescriptorResponse,
										   timeout.count() > 0 ? timeout : kDefaultTimeout);
		if (!response || response->size() < sizeof(GetDescriptorResponseHeader))
			return std::nullopt;

		const auto* payload = reinterpret_cast<const GetDescriptorResponseHeader*>(response->data());
		const size_t expected = sizeof(GetDescriptorResponseHeader) + payload->data_length;
		if (response->size() < expected)
			return std::nullopt;

		DescriptorResult result{payload->status, {}};
		if (payload->data_length > 0)
		{
			result.data.assign(response->data() + sizeof(GetDescriptorResponseHeader),
							   response->data() + sizeof(GetDescriptorResponseHeader) + payload->data_length);
		}
		return result;
	}

	bool EmulatedUSBUDPClient::RequestSetIdle(uint8_t if_index, uint8_t report_id, uint8_t duration, std::chrono::milliseconds timeout)
	{
		if (!m_running.load(std::memory_order_relaxed))
			return false;

		const uint32_t request_id = NextRequestId();
		auto response = SendRequestAndWait(BuildSetIdleRequest(m_uid, request_id, if_index, report_id, duration), request_id,
										   MessageType::SetIdleResponse,
										   timeout.count() > 0 ? timeout : kDefaultTimeout);
		if (!response)
			return false;
		const auto status = ParseStatus(*response);
		return status && *status == Status::Success;
	}

	bool EmulatedUSBUDPClient::RequestSetProtocol(uint8_t if_index, uint8_t protocol, std::chrono::milliseconds timeout)
	{
		if (!m_running.load(std::memory_order_relaxed))
			return false;

		const uint32_t request_id = NextRequestId();
		auto response = SendRequestAndWait(BuildSetProtocolRequest(m_uid, request_id, if_index, protocol), request_id,
										   MessageType::SetProtocolResponse,
										   timeout.count() > 0 ? timeout : kDefaultTimeout);
		if (!response)
			return false;
		const auto status = ParseStatus(*response);
		return status && *status == Status::Success;
	}

	bool EmulatedUSBUDPClient::RequestSetReport(uint8_t report_type, uint8_t report_id, const uint8_t* data, uint32_t length,
												std::chrono::milliseconds timeout)
	{
		if (!m_running.load(std::memory_order_relaxed))
			return false;

		const uint32_t request_id = NextRequestId();
		auto response = SendRequestAndWait(BuildSetReportRequest(m_uid, request_id, report_type, report_id, data, length),
										   request_id, MessageType::SetReportResponse,
										   timeout.count() > 0 ? timeout : kDefaultTimeout);
		if (!response)
			return false;
		const auto status = ParseStatus(*response);
		return status && *status == Status::Success;
	}

	std::optional<std::vector<uint8_t>> EmulatedUSBUDPClient::SendRequestAndWait(std::vector<uint8_t> packet, uint32_t request_id,
																				 MessageType expected_type, std::chrono::milliseconds timeout)
	{
		if (!m_running.load(std::memory_order_relaxed))
			return std::nullopt;

		auto pending = std::make_shared<PendingResponse>(expected_type);
		{
			std::lock_guard lock(m_pending_mutex);
			m_pending.emplace(request_id, pending);
		}

		m_writer_jobs.push(std::make_shared<std::vector<uint8_t>>(std::move(packet)));

		std::unique_lock lock(pending->mutex);
		const bool received = pending->condition.wait_for(lock, timeout, [&pending]() { return pending->ready; });

		std::optional<std::vector<uint8_t>> result;
		if (received)
		{
			if (pending->payload.size() >= sizeof(ServerMessage))
			{
				const auto* header = reinterpret_cast<const ServerMessage*>(pending->payload.data());
				if (header->GetMessageType() == pending->expected_type)
					result = pending->payload;
			}
		}

		{
			std::lock_guard lock_pending(m_pending_mutex);
			m_pending.erase(request_id);
		}

		return result;
	}

	uint32_t EmulatedUSBUDPClient::NextRequestId()
	{
		return ++m_request_id;
	}

	void EmulatedUSBUDPClient::ReaderThread()
	{
		SetThreadName("EmulatedUSBUDP-reader");
		while (m_running.load(std::memory_order_relaxed))
		{
			std::array<uint8_t, kMaxPacketSize> recv_buf{};
			boost::asio::ip::udp::endpoint sender_endpoint;
			boost::system::error_code ec{};
			const size_t len = m_socket.receive_from(boost::asio::buffer(recv_buf), sender_endpoint, 0, ec);
			if (!m_running.load(std::memory_order_relaxed))
				break;
			if (ec)
				continue;

			if (len < sizeof(ResponseHeader))
				continue;

			const auto* msg = reinterpret_cast<const ServerMessage*>(recv_buf.data());
			if (!msg->IsServerMessage() || !msg->ValidateCRC32(len))
				continue;

			const auto type = msg->GetMessageType();
			const auto* response_header = reinterpret_cast<const ResponseHeader*>(recv_buf.data());
			const uint32_t request_id = response_header->request_id;

			if (type == MessageType::ReadResponse && len >= sizeof(ReadResponseHeader))
			{
				const auto* read_header = reinterpret_cast<const ReadResponseHeader*>(recv_buf.data());
				const size_t expected = sizeof(ReadResponseHeader) + read_header->data_length;
				if (read_header->status == Status::Success && len >= expected)
				{
					std::lock_guard lock(m_latest_read_mutex);
					m_latest_read_packet_index = read_header->packet_index;
					m_latest_read.assign(recv_buf.data() + sizeof(ReadResponseHeader),
										 recv_buf.data() + sizeof(ReadResponseHeader) + read_header->data_length);
				}
			}

			std::shared_ptr<PendingResponse> pending;
			{
				std::lock_guard lock(m_pending_mutex);
				auto it = m_pending.find(request_id);
				if (it != m_pending.end())
					pending = it->second;
			}

			if (pending)
			{
				std::lock_guard lock(pending->mutex);
				pending->payload.assign(recv_buf.begin(), recv_buf.begin() + len);
				pending->ready = true;
				pending->condition.notify_all();
			}
		}
	}

	void EmulatedUSBUDPClient::WriterThread()
	{
		SetThreadName("EmulatedUSBUDP-writer");
		while (m_running.load(std::memory_order_relaxed))
		{
			auto packet = m_writer_jobs.pop();
			if (!m_running.load(std::memory_order_relaxed))
				return;
			if (!packet)
				return;
			try
			{
				m_socket.send_to(boost::asio::buffer(*packet), m_receiver_endpoint);
			} catch (const std::exception& ex)
			{
				cemuLog_log(LogType::Force, "EmulatedUSBUDPClient send failed: {}", ex.what());
				std::this_thread::sleep_for(std::chrono::milliseconds(250));
			}
		}
	}

	void EmulatedUSBUDPClient::WakeReader()
	{
		if (m_socket_wakeup_endpoint.port() == 0)
			return;

		boost::asio::io_context io_context;
		boost::asio::ip::udp::socket socket(io_context);
		boost::system::error_code ec;
		socket.open(m_socket_wakeup_endpoint.protocol(), ec);
		if (!ec)
		{
			std::array<uint8_t, 1> data{};
			socket.send_to(boost::asio::buffer(data), m_socket_wakeup_endpoint, 0, ec);
		}
	}
} // namespace nsyshid::emulated_usb_udp