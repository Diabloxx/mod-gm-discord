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

        std::atomic_bool _running{false};
        std::thread _thread;
        void* _cluster = nullptr;
    };
}

#endif
