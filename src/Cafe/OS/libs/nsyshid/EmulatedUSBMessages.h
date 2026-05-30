#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace nsyshid::emulated_usb_udp
{
	enum class MessageType : uint32_t
	{
		DeviceInfoRequest = 0x200000,
		DeviceInfoResponse = 0x200001,
		OpenRequest = 0x200002,
		OpenResponse = 0x200003,
		CloseRequest = 0x200004,
		CloseResponse = 0x200005,
		ReadRequest = 0x200006,
		ReadResponse = 0x200007,
		WriteRequest = 0x200008,
		WriteResponse = 0x200009,
		GetDescriptorRequest = 0x20000A,
		GetDescriptorResponse = 0x20000B,
		SetIdleRequest = 0x20000C,
		SetIdleResponse = 0x20000D,
		SetProtocolRequest = 0x20000E,
		SetProtocolResponse = 0x20000F,
		SetReportRequest = 0x200010,
		SetReportResponse = 0x200011,
	};

	enum class Status : uint8_t
	{
		Success = 0,
		Error = 1,
		Timeout = 2,
		Unsupported = 3,
	};

#pragma pack(push, 1)

	class MessageHeader
	{
	  public:
		MessageHeader(uint32_t magic, uint32_t uid);

		[[nodiscard]] uint16_t GetSize() const
		{
			return sizeof(MessageHeader) + m_packet_size;
		}
		[[nodiscard]] bool IsClientMessage() const;
		[[nodiscard]] bool IsServerMessage() const;
		[[nodiscard]] uint32_t GetCRC32() const
		{
			return m_crc32;
		}
		void Finalize(size_t size);
		[[nodiscard]] uint32_t CRC32(size_t size) const;

	  private:
		uint32_t m_magic;
		uint16_t m_protocol_version;
		uint16_t m_packet_size = 0;
		mutable uint32_t m_crc32 = 0;
		uint32_t m_uid;
	};
	static_assert(sizeof(MessageHeader) == 0x10);

	class Message : public MessageHeader
	{
	  public:
		Message(uint32_t magic, uint32_t uid, MessageType type);
		[[nodiscard]] MessageType GetMessageType() const
		{
			return m_message_type;
		}

	  private:
		MessageType m_message_type;
	};
	static_assert(sizeof(Message) == 0x14);

	class ClientMessage : public Message
	{
	  public:
		ClientMessage(uint32_t uid, MessageType message_type);
	};

	class ServerMessage : public Message
	{
	  public:
		ServerMessage() = delete;
		[[nodiscard]] bool ValidateCRC32(size_t size) const;
	  };

	struct RequestHeader : public ClientMessage
	{
		RequestHeader(uint32_t uid, MessageType message_type, uint32_t request_id);

		uint32_t request_id;
	};

	struct ResponseHeader : public ServerMessage
	{
		uint32_t request_id;
	};

	struct DeviceInfoPayload
	{
		uint16_t vendor_id;
		uint16_t product_id;
		uint8_t interface_index;
		uint8_t interface_sub_class;
		uint8_t protocol;
		uint8_t reserved0;
		uint16_t max_packet_size_rx;
		uint16_t max_packet_size_tx;
	};

	struct DeviceInfoRequest : public RequestHeader
	{
		explicit DeviceInfoRequest(uint32_t uid, uint32_t request_id);
	};

	struct OpenRequest : public RequestHeader
	{
		explicit OpenRequest(uint32_t uid, uint32_t request_id);
	};

	struct CloseRequest : public RequestHeader
	{
		explicit CloseRequest(uint32_t uid, uint32_t request_id);
	};

	struct ReadRequest : public RequestHeader
	{
		ReadRequest(uint32_t uid, uint32_t request_id, uint32_t data_length, uint32_t timeout_ms);
		uint32_t data_length;
		uint32_t timeout_ms;
	};

	struct WriteRequestHeader : public RequestHeader
	{
		WriteRequestHeader(uint32_t uid, uint32_t request_id, uint32_t data_length);
		uint32_t data_length;
	};

	struct GetDescriptorRequest : public RequestHeader
	{
		GetDescriptorRequest(uint32_t uid, uint32_t request_id, uint8_t desc_type, uint8_t desc_index,
							 uint16_t lang, uint32_t max_length);
		uint8_t desc_type;
		uint8_t desc_index;
		uint16_t lang;
		uint32_t max_length;
	};

	struct SetIdleRequest : public RequestHeader
	{
		SetIdleRequest(uint32_t uid, uint32_t request_id, uint8_t if_index, uint8_t report_id, uint8_t duration);
		uint8_t if_index;
		uint8_t report_id;
		uint8_t duration;
		uint8_t reserved0;
	};

	struct SetProtocolRequest : public RequestHeader
	{
		SetProtocolRequest(uint32_t uid, uint32_t request_id, uint8_t if_index, uint8_t protocol);
		uint8_t if_index;
		uint8_t protocol;
		uint16_t reserved0;
	};

	struct SetReportRequestHeader : public RequestHeader
	{
		SetReportRequestHeader(uint32_t uid, uint32_t request_id, uint8_t report_type, uint8_t report_id,
							   uint32_t data_length);
		uint8_t report_type;
		uint8_t report_id;
		uint16_t reserved0;
		uint32_t data_length;
	};

	struct StatusResponse : public ResponseHeader
	{
		Status status;
		uint8_t reserved0[3];
	};

	struct DeviceInfoResponse : public ResponseHeader
	{
		Status status;
		uint8_t reserved0[3];
		DeviceInfoPayload info;
	};

	struct ReadResponseHeader : public ResponseHeader
	{
		Status status;
		uint8_t reserved0[3];
		uint32_t packet_index;
		uint32_t data_length;
	};

	struct WriteResponse : public ResponseHeader
	{
		Status status;
		uint8_t reserved0[3];
		uint32_t bytes_written;
	};

	struct GetDescriptorResponseHeader : public ResponseHeader
	{
		Status status;
		uint8_t reserved0[3];
		uint32_t data_length;
	};

#pragma pack(pop)

	std::vector<uint8_t> BuildDeviceInfoRequest(uint32_t uid, uint32_t request_id);
	std::vector<uint8_t> BuildOpenRequest(uint32_t uid, uint32_t request_id);
	std::vector<uint8_t> BuildCloseRequest(uint32_t uid, uint32_t request_id);
	std::vector<uint8_t> BuildReadRequest(uint32_t uid, uint32_t request_id, uint32_t data_length, uint32_t timeout_ms);
	std::vector<uint8_t> BuildWriteRequest(uint32_t uid, uint32_t request_id, const uint8_t* data, uint32_t data_length);
	std::vector<uint8_t> BuildGetDescriptorRequest(uint32_t uid, uint32_t request_id, uint8_t desc_type, uint8_t desc_index,
												   uint16_t lang, uint32_t max_length);
	std::vector<uint8_t> BuildSetIdleRequest(uint32_t uid, uint32_t request_id, uint8_t if_index, uint8_t report_id, uint8_t duration);
	std::vector<uint8_t> BuildSetProtocolRequest(uint32_t uid, uint32_t request_id, uint8_t if_index, uint8_t protocol);
	std::vector<uint8_t> BuildSetReportRequest(uint32_t uid, uint32_t request_id, uint8_t report_type, uint8_t report_id,
											   const uint8_t* data, uint32_t data_length);
} // namespace nsyshid::emulated_usb_udp