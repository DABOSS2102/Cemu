#include "SkylanderPortalManager.h"

#include "Common/FileStream.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"

namespace nsyshid
{
	namespace
	{
		constexpr uint8 SKY_ID_OFFSET_LOW = 0x10;
		constexpr uint8 SKY_ID_OFFSET_HIGH = 0x11;
		constexpr uint8 SKY_VAR_OFFSET_LOW = 0x1C;
		constexpr uint8 SKY_VAR_OFFSET_HIGH = 0x1D;
	}

	SkylanderPortalManager& SkylanderPortalManager::GetInstance()
	{
		static SkylanderPortalManager s_instance;
		return s_instance;
	}

	std::optional<SkylanderPortalManager::SlotInfo> SkylanderPortalManager::GetSlotUnsafe(uint8 uiSlot) const
	{
		if (uiSlot >= MAX_SKYLANDERS)
			return {};
		return m_slots[uiSlot];
	}

	std::vector<SkylanderPortalManager::SlotInfo> SkylanderPortalManager::GetSlots() const
	{
		std::lock_guard lock(m_mutex);
		std::vector<SlotInfo> slots;
		slots.reserve(MAX_SKYLANDERS);
		for (uint8 i = 0; i < MAX_SKYLANDERS; ++i)
		{
			if (const auto slot = GetSlotUnsafe(i); slot.has_value())
				slots.emplace_back(*slot);
			else
			{
				SlotInfo s;
				s.uiSlot = i;
				slots.emplace_back(s);
			}
		}
		return slots;
	}

	fs::path SkylanderPortalManager::GetStorageFolderPath() const
	{
		const auto folder = GetConfig().emulated_usb_devices.skylander_api_storage_path.GetValue();
		if (!folder.empty())
			return _utf8ToPath(folder);
		return ActiveSettings::GetUserDataPath("skylanders");
	}

	bool SkylanderPortalManager::IsPathWithin(const fs::path& base, const fs::path& candidate)
	{
		auto baseIt = base.begin();
		auto candIt = candidate.begin();
		for (; baseIt != base.end() && candIt != candidate.end(); ++baseIt, ++candIt)
		{
			if (*baseIt != *candIt)
				return false;
		}
		if (baseIt != base.end())
			return false;
		return true;
	}

	bool SkylanderPortalManager::SetStorageFolderPath(const fs::path& folderPath, std::string& error)
	{
		error.clear();
		std::error_code ec;
		const fs::path normalized = folderPath.lexically_normal();
		if (normalized.empty())
		{
			error = "Storage folder path is empty";
			return false;
		}
		if (!fs::exists(normalized, ec))
			fs::create_directories(normalized, ec);
		if (ec)
		{
			error = fmt::format("Failed to create folder: {}", ec.message());
			return false;
		}
		if (!fs::is_directory(normalized, ec) || ec)
		{
			error = "Storage path is not a directory";
			return false;
		}
		GetConfig().emulated_usb_devices.skylander_api_storage_path = _pathToUtf8(normalized);
		GetConfigHandle().Save();
		return true;
	}

	bool SkylanderPortalManager::ResolveStorageFilePath(std::string_view relativePath, fs::path& resolved, std::string& error) const
	{
		error.clear();
		const fs::path root = GetStorageFolderPath().lexically_normal();
		std::error_code ec;
		if (!fs::exists(root, ec))
			fs::create_directories(root, ec);
		if (ec)
		{
			error = fmt::format("Failed to prepare storage folder: {}", ec.message());
			return false;
		}
		if (relativePath.empty())
		{
			error = "File path is empty";
			return false;
		}

		fs::path relative = _utf8ToPath(std::string(relativePath));
		if (relative.is_absolute())
		{
			error = "Absolute paths are not allowed";
			return false;
		}

		const fs::path candidate = (root / relative).lexically_normal();
		if (!IsPathWithin(root, candidate))
		{
			error = "Path traversal outside storage folder is not allowed";
			return false;
		}
		resolved = candidate;
		return true;
	}

	bool SkylanderPortalManager::LoadSkylanderFromPath(uint8 uiSlot, const fs::path& filePath, std::string& error)
	{
		error.clear();
		if (uiSlot >= MAX_SKYLANDERS)
		{
			error = "Invalid UI slot";
			return false;
		}

		std::unique_ptr<FileStream> skyFile(FileStream::openFile2(filePath, true));
		if (!skyFile)
		{
			error = fmt::format("Failed to open file: {}", _pathToUtf8(filePath));
			return false;
		}

		std::array<uint8, SKY_FIGURE_SIZE> fileData{};
		if (skyFile->readData(fileData.data(), fileData.size()) != fileData.size())
		{
			error = "Failed to read full Skylander figure data";
			return false;
		}

		const uint16 skyId = uint16(fileData[SKY_ID_OFFSET_HIGH]) << 8 | uint16(fileData[SKY_ID_OFFSET_LOW]);
		const uint16 skyVar = uint16(fileData[SKY_VAR_OFFSET_HIGH]) << 8 | uint16(fileData[SKY_VAR_OFFSET_LOW]);

		std::lock_guard lock(m_mutex);
		if (m_slots[uiSlot].has_value())
		{
			const auto previousPortalSlot = m_slots[uiSlot]->portalSlot;
			g_skyportal.RemoveSkylander(previousPortalSlot);
			m_slots[uiSlot].reset();
		}

		uint8 portalSlot = g_skyportal.LoadSkylander(fileData.data(), std::move(skyFile));
		if (portalSlot == 0xFF)
		{
			error = "No free portal slot available";
			return false;
		}

		SlotInfo slotInfo;
		slotInfo.uiSlot = uiSlot;
		slotInfo.loaded = true;
		slotInfo.portalSlot = portalSlot;
		slotInfo.skyId = skyId;
		slotInfo.skyVar = skyVar;
		slotInfo.filePath = filePath;
		m_slots[uiSlot] = slotInfo;
		return true;
	}

	bool SkylanderPortalManager::LoadSkylanderFromStorage(uint8 uiSlot, std::string_view relativePath, std::string& error)
	{
		fs::path resolved;
		if (!ResolveStorageFilePath(relativePath, resolved, error))
			return false;
		return LoadSkylanderFromPath(uiSlot, resolved, error);
	}

	bool SkylanderPortalManager::CreateSkylander(uint8 uiSlot, const fs::path& filePath, uint16 skyId, uint16 skyVar, std::string& error)
	{
		error.clear();
		if (uiSlot >= MAX_SKYLANDERS)
		{
			error = "Invalid UI slot";
			return false;
		}

		std::error_code ec;
		if (const auto parentPath = filePath.parent_path(); !parentPath.empty() && !fs::exists(parentPath, ec))
			fs::create_directories(parentPath, ec);
		if (ec)
		{
			error = fmt::format("Failed to create output directory: {}", ec.message());
			return false;
		}

		if (!g_skyportal.CreateSkylander(filePath, skyId, skyVar))
		{
			error = "Failed to create Skylander file";
			return false;
		}
		return LoadSkylanderFromPath(uiSlot, filePath, error);
	}

	bool SkylanderPortalManager::CreateSkylanderInStorage(uint8 uiSlot, std::string_view relativePath, uint16 skyId, uint16 skyVar, std::string& error)
	{
		fs::path resolved;
		if (!ResolveStorageFilePath(relativePath, resolved, error))
			return false;
		return CreateSkylander(uiSlot, resolved, skyId, skyVar, error);
	}

	bool SkylanderPortalManager::ClearSkylander(uint8 uiSlot)
	{
		if (uiSlot >= MAX_SKYLANDERS)
			return false;

		std::lock_guard lock(m_mutex);
		if (!m_slots[uiSlot].has_value())
			return false;
		g_skyportal.RemoveSkylander(m_slots[uiSlot]->portalSlot);
		m_slots[uiSlot].reset();
		return true;
	}

	void SkylanderPortalManager::ClearAllSkylanders()
	{
		std::lock_guard lock(m_mutex);
		for (uint8 i = 0; i < MAX_SKYLANDERS; ++i)
		{
			if (m_slots[i].has_value())
			{
				g_skyportal.RemoveSkylander(m_slots[i]->portalSlot);
				m_slots[i].reset();
			}
		}
	}
} // namespace nsyshid
