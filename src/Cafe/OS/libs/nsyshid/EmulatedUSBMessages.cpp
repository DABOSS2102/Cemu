#include "EmulatedUSBMessages.h"

#include <cstring>

#include "util/crypto/crc32.h"

namespace nsyshid::emulated_usb_udp
{
	constexpr uint32_t kMagicClient = 'CUSB';
	constexpr uint32_t kMagicServer = 'SUSB';
	constexpr uint16_t kProtocolVersion = 1;

	MessageHeader::MessageHeader(uint32_t magic, uint32_t uid)
		: m_magic(magic), m_protocol_version(kProtocolVersion), m_uid(uid)
	{
	}

	void MessageHeader::Finalize(size_t size)
	{
		cemuLog_logDebug(LogType::Force, "MessageHeader: Finalize");
		m_packet_size = static_cast<uint16_t>(size - sizeof(MessageHeader));
		m_crc32 = CRC32(size);
	}

	uint32_t MessageHeader::CRC32(size_t size) const
	{
		cemuLog_logDebug(LogType::Force, "MessageHeader: CRC32");
		uint32_t tmp = m_crc32;
		m_crc32 = 0;
		const uint32_t tmp2 = crc32_calc(this, size);
		m_crc32 = tmp;
		return tmp2;
	}

	bool MessageHeader::IsClientMessage() const
	{
		cemuLog_logDebug(LogType::Force, "MessageHeader: Is Client Message");
		return m_magic == kMagicClient;
	}
	bool MessageHeader::IsServerMessage() const
	{
		cemuLog_logDebug(LogType::Force, "MessageHeader: Is Server Message");
		return m_magic == kMagicServer;
	}

	Message::Message(uint32_t magic, uint32_t uid, MessageType type)
		: MessageHeader(magic, uid), m_message_type(type)
	{
		cemuLog_logDebug(LogType::Force, "Message: Message");
	}

	ClientMessage::ClientMessage(uint32_t uid, MessageType message_type)
		: Message(kMagicClient, uid, message_type)
	{
		cemuLog_logDebug(LogType::Force, "ClientMessage: Client Message");
	}

	bool ServerMessage::ValidateCRC32(size_t size) const
	{
		cemuLog_logDebug(LogType::Force, "ServerMessage: Validate CRC32");
		return GetCRC32() == CRC32(size);
	}

	RequestHeader::RequestHeader(uint32_t uid, MessageType message_type, uint32_t request_id)
		: ClientMessage(uid, message_type), request_id(request_id)
	{
		cemuLog_logDebug(LogType::Force, "RequestHeader: Request Header");
	}

	DeviceInfoRequest::DeviceInfoRequest(uint32_t uid, uint32_t request_id)
		: RequestHeader(uid, MessageType::DeviceInfoRequest, request_id)
	{
		cemuLog_logDebug(LogType::Force, "DeviceInfoRequest: Device Info Request");
		Finalize(sizeof(DeviceInfoRequest));
	}

	OpenRequest::OpenRequest(uint32_t uid, uint32_t request_id)
		: RequestHeader(uid, MessageType::OpenRequest, request_id)
	{
		cemuLog_logDebug(LogType::Force, "OpenRequest: Open Request");
		Finalize(sizeof(OpenRequest));
	}

	CloseRequest::CloseRequest(uint32_t uid, uint32_t request_id)
		: RequestHeader(uid, MessageType::CloseRequest, request_id)
	{
		cemuLog_logDebug(LogType::Force, "CloseRequest: Close Request");
		Finalize(sizeof(CloseRequest));
	}

	ReadRequest::ReadRequest(uint32_t uid, uint32_t request_id, uint32_t data_length, uint32_t timeout_ms)
		: RequestHeader(uid, MessageType::ReadRequest, request_id), data_length(data_length), timeout_ms(timeout_ms)
	{
		cemuLog_logDebug(LogType::Force, "ReadRequest: Read Request");
		Finalize(sizeof(ReadRequest));
	}

	WriteRequestHeader::WriteRequestHeader(uint32_t uid, uint32_t request_id, uint32_t data_length)
		: RequestHeader(uid, MessageType::WriteRequest, request_id), data_length(data_length)
	{
		cemuLog_logDebug(LogType::Force, "WriteRequestHeader: Write Request Header");
	}

	GetDescriptorRequest::GetDescriptorRequest(uint32_t uid, uint32_t request_id, uint8_t desc_type, uint8_t desc_index,
											   uint16_t lang, uint32_t max_length)
		: RequestHeader(uid, MessageType::GetDescriptorRequest, request_id),
		  desc_type(desc_type),
		  desc_index(desc_index),
		  lang(lang),
		  max_length(max_length)
	{
		cemuLog_logDebug(LogType::Force, "GetDescriptorRequest: Get Descriptor Request");
		Finalize(sizeof(GetDescriptorRequest));
	}

	SetIdleRequest::SetIdleRequest(uint32_t uid, uint32_t request_id, uint8_t if_index, uint8_t report_id, uint8_t duration)
		: RequestHeader(uid, MessageType::SetIdleRequest, request_id),
		  if_index(if_index),
		  report_id(report_id),
		  duration(duration),
		  reserved0(0)
	{
		cemuLog_logDebug(LogType::Force, "SetIdleRequest: Set Idle Request");
		Finalize(sizeof(SetIdleRequest));
	}

	SetProtocolRequest::SetProtocolRequest(uint32_t uid, uint32_t request_id, uint8_t if_index, uint8_t protocol)
		: RequestHeader(uid, MessageType::SetProtocolRequest, request_id),
		  if_index(if_index),
		  protocol(protocol),
		  reserved0(0)
	{
		cemuLog_logDebug(LogType::Force, "SetProtocolRequest: Set Protocol Request");
		Finalize(sizeof(SetProtocolRequest));
	}

	SetReportRequestHeader::SetReportRequestHeader(uint32_t uid, uint32_t request_id, uint8_t report_type, uint8_t report_id,
												   uint32_t data_length)
		: RequestHeader(uid, MessageType::SetReportRequest, request_id),
		  report_type(report_type),
		  report_id(report_id),
		  reserved0(0),
		  data_length(data_length)
	{
		cemuLog_logDebug(LogType::Force, "SetReportRequestHeader: Set Report Request Header");
	}

	std::vector<uint8_t> BuildDeviceInfoRequest(uint32_t uid, uint32_t request_id)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBMessages: Build Device Info Request");
		DeviceInfoRequest request(uid, request_id);
		return std::vector<uint8_t>(reinterpret_cast<uint8_t*>(&request),
									reinterpret_cast<uint8_t*>(&request) + sizeof(DeviceInfoRequest));
	}

	std::vector<uint8_t> BuildOpenRequest(uint32_t uid, uint32_t request_id)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBMessages: Build Open Request");
		OpenRequest request(uid, request_id);
		return std::vector<uint8_t>(reinterpret_cast<uint8_t*>(&request),
									reinterpret_cast<uint8_t*>(&request) + sizeof(OpenRequest));
	}

	std::vector<uint8_t> BuildCloseRequest(uint32_t uid, uint32_t request_id)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBMessages: Build Close Request");
		CloseRequest request(uid, request_id);
		return std::vector<uint8_t>(reinterpret_cast<uint8_t*>(&request),
									reinterpret_cast<uint8_t*>(&request) + sizeof(CloseRequest));
	}

	std::vector<uint8_t> BuildReadRequest(uint32_t uid, uint32_t request_id, uint32_t data_length, uint32_t timeout_ms)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBMessages: Build Read Request");
		ReadRequest request(uid, request_id, data_length, timeout_ms);
		return std::vector<uint8_t>(reinterpret_cast<uint8_t*>(&request),
									reinterpret_cast<uint8_t*>(&request) + sizeof(ReadRequest));
	}

	std::vector<uint8_t> BuildWriteRequest(uint32_t uid, uint32_t request_id, const uint8_t* data, uint32_t data_length)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBMessages: Build Write Request");
		WriteRequestHeader request(uid, request_id, data_length);
		std::vector<uint8_t> buffer(sizeof(WriteRequestHeader) + data_length);
		memcpy(buffer.data(), &request, sizeof(WriteRequestHeader));
		if (data_length > 0)
			memcpy(buffer.data() + sizeof(WriteRequestHeader), data, data_length);
		reinterpret_cast<MessageHeader*>(buffer.data())->Finalize(buffer.size());
		return buffer;
	}

	std::vector<uint8_t> BuildGetDescriptorRequest(uint32_t uid, uint32_t request_id, uint8_t desc_type, uint8_t desc_index,
												   uint16_t lang, uint32_t max_length)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBMessages: Build Get Descriptor Request");
		GetDescriptorRequest request(uid, request_id, desc_type, desc_index, lang, max_length);
		return std::vector<uint8_t>(reinterpret_cast<uint8_t*>(&request),
									reinterpret_cast<uint8_t*>(&request) + sizeof(GetDescriptorRequest));
	}

	std::vector<uint8_t> BuildSetIdleRequest(uint32_t uid, uint32_t request_id, uint8_t if_index, uint8_t report_id, uint8_t duration)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBMessages: Build Set Idle Request");
		SetIdleRequest request(uid, request_id, if_index, report_id, duration);
		return std::vector<uint8_t>(reinterpret_cast<uint8_t*>(&request),
									reinterpret_cast<uint8_t*>(&request) + sizeof(SetIdleRequest));
	}

	std::vector<uint8_t> BuildSetProtocolRequest(uint32_t uid, uint32_t request_id, uint8_t if_index, uint8_t protocol)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBMessages: Build Set Protocol Request");
		SetProtocolRequest request(uid, request_id, if_index, protocol);
		return std::vector<uint8_t>(reinterpret_cast<uint8_t*>(&request),
									reinterpret_cast<uint8_t*>(&request) + sizeof(SetProtocolRequest));
	}

	std::vector<uint8_t> BuildSetReportRequest(uint32_t uid, uint32_t request_id, uint8_t report_type, uint8_t report_id,
											   const uint8_t* data, uint32_t data_length)
	{
		cemuLog_logDebug(LogType::Force, "EmulatedUSBMessages: Build Set Report Request");
		SetReportRequestHeader request(uid, request_id, report_type, report_id, data_length);
		std::vector<uint8_t> buffer(sizeof(SetReportRequestHeader) + data_length);
		memcpy(buffer.data(), &request, sizeof(SetReportRequestHeader));
		if (data_length > 0)
			memcpy(buffer.data() + sizeof(SetReportRequestHeader), data, data_length);
		reinterpret_cast<MessageHeader*>(buffer.data())->Finalize(buffer.size());
		return buffer;
	}
} // namespace nsyshid::emulated_usb_udp