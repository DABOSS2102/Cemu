#include "BackendEmulated.h"

#include "Dimensions.h"
#include "EmulatedUSBDevice.h"
#include "Infinity.h"
#include "Skylander.h"
#include "config/CemuConfig.h"
#include "SkylanderXbox360.h"

namespace nsyshid::backend::emulated
{
	BackendEmulated::BackendEmulated()
	{
		cemuLog_logDebug(LogType::Force, "nsyshid::BackendEmulated: emulated backend initialized");
	}

	BackendEmulated::~BackendEmulated() = default;

	bool BackendEmulated::IsInitialisedOk()
	{
		return true;
	}

	void BackendEmulated::AttachVisibleDevices()
	{
		if (GetConfig().emulated_usb_devices.emulate_skylander_portal && !FindDeviceById(0x1430, 0x0150))
		{
			cemuLog_logDebug(LogType::Force, "Attaching Emulated Portal");
			// Add Skylander Portal
			auto device = std::make_shared<SkylanderPortalDevice>();
			AttachDevice(device);
		}
#ifdef HAS_LIBUSB
		else if (auto usb_portal = FindDeviceById(0x1430, 0x1F17))
		{
			cemuLog_logDebug(LogType::Force, "Attaching Xbox 360 Portal");
			// Add Skylander Xbox 360 Portal
			auto device = std::make_shared<SkylanderXbox360PortalLibusb>(usb_portal);
			AttachDevice(device);
		}
#endif
		if (GetConfig().emulated_usb_devices.emulate_infinity_base && !FindDeviceById(0x0E6F, 0x0129))
		{
			cemuLog_logDebug(LogType::Force, "Attaching Emulated Base");
			// Add Infinity Base
			auto device = std::make_shared<InfinityBaseDevice>();
			AttachDevice(device);
		}
		if (GetConfig().emulated_usb_devices.emulate_dimensions_toypad && !FindDeviceById(0x0E6F, 0x0241))
		{
			cemuLog_logDebug(LogType::Force, "Attaching Emulated Toypad");
			// Add Dimensions Toypad
			auto device = std::make_shared<DimensionsToypadDevice>();
			AttachDevice(device);
		}
		if (GetConfig().emulated_usb_devices.emulate_udp_device)
		{
			cemuLog_logDebug(LogType::Force, "emulated_usb_devices.emulate_udp_device was true");
			emulated_usb_udp::EmulatedUSBUDPClient::Settings settings{
				GetConfig().emulated_usb_devices.udp_host.GetValue(),
				GetConfig().emulated_usb_devices.udp_port.GetValue()};
			cemuLog_logDebug(LogType::Force, "emulated_usb_devices.udp_host: {}", settings.host);
			cemuLog_logDebug(LogType::Force, "emulated_usb_devices.udp_port: {}", settings.port);
			auto device = EmulatedUSBDevice::Create(settings);
			if (device)
			{
				if (!FindDeviceById(device->m_vendorId, device->m_productId))
				{
					cemuLog_logDebug(LogType::Force, "Attaching Emulated UDP USB device");
					AttachDevice(device);
				}
				else
				{
					cemuLog_logDebug(LogType::Force, "Could not FindDeviceById()");
					cemuLog_logDebug(LogType::Force, "Vendor ID: {}", device->m_vendorId);
					cemuLog_logDebug(LogType::Force, "Product ID: {}", device->m_productId);
				}
			}
			else
			{
				cemuLog_logDebug(LogType::Force, "Failed to attach Emulated UDP USB device (device info unavailable)");
			}
		}
	}
} // namespace nsyshid::backend::emulated