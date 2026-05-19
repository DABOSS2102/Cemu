#pragma once

#include <optional>
#include <vector>

#include "Skylander.h"

namespace nsyshid
{
	class SkylanderPortalManager
	{
	  public:
		struct SlotInfo
		{
			uint8 uiSlot = 0;
			bool loaded = false;
			uint8 portalSlot = 0xFF;
			uint16 skyId = 0;
			uint16 skyVar = 0;
			fs::path filePath;
		};

		static SkylanderPortalManager& GetInstance();

		std::vector<SlotInfo> GetSlots() const;
		bool LoadSkylanderFromPath(uint8 uiSlot, const fs::path& filePath, std::string& error);
		bool LoadSkylanderFromStorage(uint8 uiSlot, std::string_view relativePath, std::string& error);
		bool CreateSkylander(uint8 uiSlot, const fs::path& filePath, uint16 skyId, uint16 skyVar, std::string& error);
		bool CreateSkylanderInStorage(uint8 uiSlot, std::string_view relativePath, uint16 skyId, uint16 skyVar, std::string& error);
		bool ClearSkylander(uint8 uiSlot);
		void ClearAllSkylanders();

		fs::path GetStorageFolderPath() const;
		bool SetStorageFolderPath(const fs::path& folderPath, std::string& error);
		bool ResolveStorageFilePath(std::string_view relativePath, fs::path& resolved, std::string& error) const;

	  private:
		SkylanderPortalManager() = default;

		std::optional<SlotInfo> GetSlotUnsafe(uint8 uiSlot) const;
		static bool IsPathWithin(const fs::path& base, const fs::path& candidate);

	  private:
		mutable std::mutex m_mutex;
		std::array<std::optional<SlotInfo>, MAX_SKYLANDERS> m_slots{};
	};
} // namespace nsyshid

