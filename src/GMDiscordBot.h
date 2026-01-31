/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MOD_GM_DISCORD_BOT_H
#define MOD_GM_DISCORD_BOT_H

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace GMDiscord
{
    class DiscordBot
    {
    public:
        static DiscordBot& Instance();

        void LoadConfig();
        void Start();
        void Stop();

        bool IsEnabled() const { return _enabled; }
        std::string const& GetBotId() const { return _botId; }

    private:
        DiscordBot() = default;

        bool _enabled = false;
        std::string _botId;
        std::string _botToken;
        uint64_t _guildId = 0;
        uint64_t _outboxChannelId = 0;
        bool _ticketRoomsEnabled = false;
        uint64_t _ticketRoomCategoryId = 0;
        uint64_t _ticketRoomArchiveCategoryId = 0;
        std::string _ticketRoomNameFormat;
        bool _ticketRoomPostUpdates = true;
        bool _ticketRoomArchiveOnClose = true;
        std::unordered_set<uint64_t> _ticketRoomAllowedRoleIds;
        std::unordered_map<uint32_t, uint64_t> _ticketThreadIds;
        std::unordered_map<uint64_t, uint32_t> _threadTicketIds;
        std::unordered_map<uint32_t, uint64_t> _ticketMessageIds;
        std::string _roleMappingsRaw;

        std::unordered_map<uint64_t, std::unordered_set<std::string>> _roleCategoryMap;

        std::atomic_bool _running{false};
        std::thread _thread;
        void* _cluster = nullptr;
    };
}

#endif
