#pragma once

#include <atomic>

#include "Backend.h"
#include "EmulatedUSBUDPClient.h"

namespace nsyshid
{
	class EmulatedUSBDevice final : public Device
	{
	  public:
		static std::shared_ptr<EmulatedUSBDevice> Create(const emulated_usb_udp::EmulatedUSBUDPClient::Settings& settings);
		~EmulatedUSBDevice() override;

		bool Open() override;
		void Close() override;
		bool IsOpened() override;
		ReadResult Read(ReadMessage* message) override;
		WriteResult Write(WriteMessage* message) override;
		bool GetDescriptor(uint8 descType, uint8 descIndex, uint16 lang, uint8* output, uint32 outputMaxLength) override;
		bool SetIdle(uint8 ifIndex, uint8 reportId, uint8 duration) override;
		bool SetProtocol(uint8 ifIndex, uint8 protocol) override;
		bool SetReport(ReportMessage* message) override;

	  private:
		EmulatedUSBDevice(const emulated_usb_udp::DeviceInfoPayload& info,
						  std::unique_ptr<emulated_usb_udp::EmulatedUSBUDPClient> client);

		std::unique_ptr<emulated_usb_udp::EmulatedUSBUDPClient> m_client;
		std::atomic_bool m_is_opened{false};
	};
} // namespace nsyshid