#include "EmulatedUSBDevice.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace nsyshid
{
	namespace
	{
		constexpr auto kOpenTimeout = std::chrono::milliseconds(1500);
		constexpr auto kReadTimeout = std::chrono::milliseconds(1500);
		constexpr auto kRequestTimeout = std::chrono::milliseconds(1500);
		constexpr auto kDeviceInfoTimeout = std::chrono::milliseconds(1500);
	} // namespace nsyshid

	std::shared_ptr<EmulatedUSBDevice> EmulatedUSBDevice::Create(
		const emulated_usb_udp::EmulatedUSBUDPClient::Settings& settings)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBDevice: Create");
		auto client = std::make_unique<emulated_usb_udp::EmulatedUSBUDPClient>(settings);
		if (!client->Start())
			return nullptr;

		auto info = client->RequestDeviceInfo(kDeviceInfoTimeout);
		if (!info)
		{
			client->Stop();
			return nullptr;
		}

		return std::shared_ptr<EmulatedUSBDevice>(new EmulatedUSBDevice(*info, std::move(client)));
	}

	EmulatedUSBDevice::EmulatedUSBDevice(const emulated_usb_udp::DeviceInfoPayload& info,
										 std::unique_ptr<emulated_usb_udp::EmulatedUSBUDPClient> client)
		: Device(info.vendor_id, info.product_id, info.interface_index, info.interface_sub_class, info.protocol),
		  m_client(std::move(client))
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBDevice: Emulated USB Device");
		m_maxPacketSizeRX = info.max_packet_size_rx;
		m_maxPacketSizeTX = info.max_packet_size_tx;
	}

	EmulatedUSBDevice::~EmulatedUSBDevice()
	{
		Close();
		if (m_client)
			m_client->Stop();
	}

	bool EmulatedUSBDevice::Open()
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBDevice: Open");
		if (m_is_opened)
			return true;
		if (!m_client || !m_client->IsRunning())
			return false;

		if (!m_client->RequestOpen(kOpenTimeout))
			return false;
		m_is_opened = true;
		return true;
	}

	void EmulatedUSBDevice::Close()
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBDevice: Close");
		if (!m_is_opened)
			return;
		if (m_client)
			m_client->RequestClose(kRequestTimeout);
		m_is_opened = false;
	}

	bool EmulatedUSBDevice::IsOpened()
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBDevice: Is Opened");
		return m_is_opened;
	}

	Device::ReadResult EmulatedUSBDevice::Read(ReadMessage* message)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBDevice: Read");
		if (!m_client || !m_is_opened)
			return Device::ReadResult::Error;

		auto response = m_client->RequestRead(message->length, kReadTimeout);
		if (!response)
			return Device::ReadResult::ErrorTimeout;

		switch (response->status)
		{
		case emulated_usb_udp::Status::Success:
			break;
		case emulated_usb_udp::Status::Timeout:
			return Device::ReadResult::ErrorTimeout;
		default:
			return Device::ReadResult::Error;
		}

		const uint32_t bytes_to_copy = std::min<uint32_t>(message->length, response->data.size());
		if (bytes_to_copy > 0)
			memcpy(message->data, response->data.data(), bytes_to_copy);
		message->bytesRead = bytes_to_copy;
		return Device::ReadResult::Success;
	}

	Device::WriteResult EmulatedUSBDevice::Write(WriteMessage* message)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBDevice: Write");
		if (!m_client || !m_is_opened)
			return Device::WriteResult::Error;

		auto response = m_client->RequestWrite(message->data, message->length, kRequestTimeout);
		if (!response)
			return Device::WriteResult::ErrorTimeout;

		switch (response->status)
		{
		case emulated_usb_udp::Status::Success:
			break;
		case emulated_usb_udp::Status::Timeout:
			return Device::WriteResult::ErrorTimeout;
		default:
			return Device::WriteResult::Error;
		}

		message->bytesWritten = std::min<uint32_t>(message->length, response->bytes_written);
		return Device::WriteResult::Success;
	}

	bool EmulatedUSBDevice::GetDescriptor(uint8 descType, uint8 descIndex, uint16 lang, uint8* output, uint32 outputMaxLength)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBDevice: Get Descriptor");
		if (!m_client || !m_is_opened)
			return false;

		auto response = m_client->RequestGetDescriptor(descType, descIndex, lang, outputMaxLength, kRequestTimeout);
		if (!response || response->status != emulated_usb_udp::Status::Success)
			return false;

		const uint32_t bytes_to_copy = std::min<uint32_t>(outputMaxLength, response->data.size());
		if (bytes_to_copy > 0)
			memcpy(output, response->data.data(), bytes_to_copy);
		return true;
	}

	bool EmulatedUSBDevice::SetIdle(uint8 ifIndex, uint8 reportId, uint8 duration)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBDevice: Set Idle");
		if (!m_client || !m_is_opened)
			return false;
		return m_client->RequestSetIdle(ifIndex, reportId, duration, kRequestTimeout);
	}

	bool EmulatedUSBDevice::SetProtocol(uint8 ifIndex, uint8 protocol)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBDevice: Set Protocol");
		if (!m_client || !m_is_opened)
			return false;
		return m_client->RequestSetProtocol(ifIndex, protocol, kRequestTimeout);
	}

	bool EmulatedUSBDevice::SetReport(ReportMessage* message)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBDevice: Set Report");
		if (!m_client || !m_is_opened)
			return false;
		return m_client->RequestSetReport(message->reportType, message->reportId, message->data, message->length, kRequestTimeout);
	}
} // namespace nsyshid