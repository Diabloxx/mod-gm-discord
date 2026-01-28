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

#include "GMDiscordBot.h"

#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "StringFormat.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#if __has_include(<dpp/dpp.h>)
#  include <dpp/dpp.h>
#  define GM_DISCORD_HAVE_DPP 1
#else
#  define GM_DISCORD_HAVE_DPP 0
#endif

namespace GMDiscord
{
    namespace
    {
        constexpr size_t DISCORD_MESSAGE_LIMIT = 1900;

        static std::string EscapeSql(std::string const& input)
        {
            std::string escaped = input;
            CharacterDatabase.EscapeString(escaped);
            return escaped;
        }

        static std::string Trim(std::string_view value)
        {
            size_t start = 0;
            while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
                ++start;

            size_t end = value.size();
            while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
                --end;

            return std::string(value.substr(start, end - start));
        }

        static std::string TruncateForDiscord(std::string const& text)
        {
            if (text.size() <= DISCORD_MESSAGE_LIMIT)
                return text;
            return text.substr(0, DISCORD_MESSAGE_LIMIT - 3) + "...";
        }

        static void InsertInboxAction(uint64 discordUserId, std::string const& action, std::string const& payload)
        {
            std::string actionEsc = EscapeSql(action);
            std::string payloadEsc = EscapeSql(payload);
            CharacterDatabase.Execute(Acore::StringFormat(
                "INSERT INTO gm_discord_inbox (discord_user_id, action, payload) VALUES ({}, '{}', '{}')",
                discordUserId, actionEsc, payloadEsc));
        }

        static bool GetGmNameForDiscordUser(uint64 discordUserId, std::string& gmName)
        {
            QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
                "SELECT gm_name FROM gm_discord_link WHERE discord_user_id={} AND verified=1 LIMIT 1",
                discordUserId));

            if (!result)
                return false;

            gmName = (*result)[0].Get<std::string>();
            gmName = Trim(gmName);
            return !gmName.empty();
        }

        static bool MarkOutboxDispatched(uint32 id)
        {
            CharacterDatabase.Execute(Acore::StringFormat(
                "UPDATE gm_discord_outbox SET dispatched=1, dispatched_at=NOW() WHERE id={} LIMIT 1",
                id));
            return true;
        }
    }
    DiscordBot& DiscordBot::Instance()
    {
        static DiscordBot instance;
        return instance;
    }

    void DiscordBot::LoadConfig()
    {
        _enabled = sConfigMgr->GetOption<bool>("GMDiscord.Bot.Enable", false);
        _botId = sConfigMgr->GetOption<std::string>("GMDiscord.Bot.Id", "");
        _botToken = sConfigMgr->GetOption<std::string>("GMDiscord.Bot.Token", "");
        _guildId = sConfigMgr->GetOption<uint64_t>("GMDiscord.Bot.GuildId", 0);
        _outboxChannelId = sConfigMgr->GetOption<uint64_t>("GMDiscord.Bot.OutboxChannelId", 0);
    }

    void DiscordBot::Start()
    {
        if (!_enabled)
        {
            LOG_INFO("module.gm_discord", "Discord bot is disabled.");
            return;
        }

        if (_botId.empty() || _botToken.empty())
        {
            LOG_ERROR("module.gm_discord", "Discord bot cannot start: missing bot id or token.");
            return;
        }
#if !GM_DISCORD_HAVE_DPP
        LOG_ERROR("module.gm_discord", "Discord bot cannot start: DPP headers not found.");
        return;
#else
        if (_running.exchange(true))
            return;

        uint64_t appId = 0;
        try
        {
            appId = std::stoull(_botId);
        }
        catch (...)
        {
            LOG_ERROR("module.gm_discord", "Discord bot cannot start: invalid bot id.");
            _running = false;
            return;
        }

        auto* cluster = new dpp::cluster(_botToken);
        _cluster = cluster;
        cluster->on_log([=](const dpp::log_t& event)
        {
            LOG_INFO("module.gm_discord", "DPP: {}", event.message);
        });

        cluster->on_ready([=](const dpp::ready_t& event)
        {
            if (dpp::run_once<struct gm_discord_ready>())
            {
                dpp::slashcommand auth("gm-auth", "Link your GM account", appId);
                auth.add_option(dpp::command_option(dpp::co_string, "secret", "Secret from in-game .discord link", true));

                dpp::slashcommand command("gm-command", "Execute GM command", appId);
                command.add_option(dpp::command_option(dpp::co_string, "command", "GM command, e.g. .ticket list", true));

                dpp::slashcommand whisper("gm-whisper", "Whisper a player as your GM name", appId);
                whisper.add_option(dpp::command_option(dpp::co_string, "player", "Player name", true));
                whisper.add_option(dpp::command_option(dpp::co_string, "message", "Message to send", true));

                if (_guildId)
                    cluster->guild_command_create(auth, _guildId);
                else
                    cluster->global_command_create(auth);

                if (_guildId)
                    cluster->guild_command_create(command, _guildId);
                else
                    cluster->global_command_create(command);

                if (_guildId)
                    cluster->guild_command_create(whisper, _guildId);
                else
                    cluster->global_command_create(whisper);

                LOG_INFO("module.gm_discord", "Discord bot ready.");
            }

            if (_outboxChannelId)
            {
                cluster->start_timer([this](dpp::timer timer)
                {
                    auto* clusterPtr = static_cast<dpp::cluster*>(_cluster);
                    if (!clusterPtr)
                        return;

                    QueryResult result = CharacterDatabase.Query(
                        "SELECT id, event_type, payload FROM gm_discord_outbox WHERE dispatched=0 ORDER BY id ASC LIMIT 10");
                    if (!result)
                        return;

                    do
                    {
                        Field* fields = result->Fetch();
                        uint32 id = fields[0].Get<uint32>();
                        std::string eventType = fields[1].Get<std::string>();
                        std::string payload = fields[2].Get<std::string>();

                        std::string content = TruncateForDiscord(Acore::StringFormat("[{}] {}", eventType, payload));
                        clusterPtr->message_create(dpp::message(_outboxChannelId, content));
                        MarkOutboxDispatched(id);
                    } while (result->NextRow());
                }, 5);
            }
        });

        cluster->on_slashcommand([=](const dpp::slashcommand_t& event)
        {
            if (_guildId && event.command.guild_id != _guildId)
            {
                event.reply(dpp::message("This bot is not enabled in this guild.").set_flags(dpp::m_ephemeral));
                return;
            }

            uint64 discordUserId = event.command.usr.id;
            std::string name = event.command.get_command_name();

            if (name == "gm-auth")
            {
                std::string secret = std::get<std::string>(event.get_parameter("secret"));
                InsertInboxAction(discordUserId, "auth", secret);
                event.reply(dpp::message("Link request submitted.").set_flags(dpp::m_ephemeral));
                return;
            }

            if (name == "gm-command")
            {
                std::string cmd = std::get<std::string>(event.get_parameter("command"));
                InsertInboxAction(discordUserId, "command", cmd);
                event.reply(dpp::message("Command queued.").set_flags(dpp::m_ephemeral));
                return;
            }

            if (name == "gm-whisper")
            {
                std::string player = std::get<std::string>(event.get_parameter("player"));
                std::string message = std::get<std::string>(event.get_parameter("message"));

                std::string gmName;
                if (!GetGmNameForDiscordUser(discordUserId, gmName))
                {
                    event.reply(dpp::message("You are not linked or GM name is missing. Use in-game .discord link <secret>.").set_flags(dpp::m_ephemeral));
                    return;
                }

                std::string payload = player + "|" + gmName + "|" + message;
                InsertInboxAction(discordUserId, "whisper", payload);
                event.reply(dpp::message("Whisper queued.").set_flags(dpp::m_ephemeral));
                return;
            }
        });

        LOG_INFO("module.gm_discord", "Discord bot starting (id: {}).", _botId);
        _thread = std::thread([this]()
        {
            auto* clusterPtr = static_cast<dpp::cluster*>(_cluster);
            if (clusterPtr)
                clusterPtr->start(dpp::st_wait);
        });
#endif
    }

    void DiscordBot::Stop()
    {
        if (!_enabled)
            return;

#if GM_DISCORD_HAVE_DPP
        if (!_running.exchange(false))
            return;

        LOG_INFO("module.gm_discord", "Discord bot stopping.");
        auto* clusterPtr = static_cast<dpp::cluster*>(_cluster);
        if (clusterPtr)
            clusterPtr->shutdown();

        if (_thread.joinable())
            _thread.join();

        delete clusterPtr;
        _cluster = nullptr;
#endif
    }
}
