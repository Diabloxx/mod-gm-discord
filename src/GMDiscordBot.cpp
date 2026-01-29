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
#include "Database/Field.h"
#include "Database/QueryResult.h"
#include "Log.h"
#include "StringFormat.h"
#include "TicketMgr.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

        static std::string Trim(std::string const& value)
        {
            size_t start = 0;
            while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])))
                ++start;

            size_t end = value.size();
            while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])))
                --end;

            return std::string(value.substr(start, end - start));
        }

        static std::string ToLower(std::string const& value)
        {
            std::string out(value);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        static std::string SanitizeWhisperName(std::string const& value)
        {
            std::string out;
            out.reserve(value.size());
            for (char ch : value)
            {
                if (std::isalnum(static_cast<unsigned char>(ch)))
                    out.push_back(ch);
            }

            return out;
        }

        static std::vector<std::string> Split(std::string const& value, char delim)
        {
            std::vector<std::string> out;
            std::string current;
            for (char ch : value)
            {
                if (ch == delim)
                {
                    std::string trimmed = Trim(current);
                    if (!trimmed.empty())
                        out.emplace_back(trimmed);
                    current.clear();
                }
                else
                {
                    current.push_back(ch);
                }
            }

            std::string trimmed = Trim(current);
            if (!trimmed.empty())
                out.emplace_back(trimmed);
            return out;
        }

        static std::unordered_set<uint64_t> ParseRoleList(std::string const& value)
        {
            std::unordered_set<uint64_t> out;
            std::string current;
            for (char ch : value)
            {
                if (ch == ',')
                {
                    std::string roleStr = Trim(current);
                    if (!roleStr.empty())
                    {
                        try
                        {
                            out.insert(std::stoull(roleStr));
                        }
                        catch (...)
                        {
                        }
                    }
                    current.clear();
                }
                else
                {
                    current.push_back(ch);
                }
            }

            std::string roleStr = Trim(current);
            if (!roleStr.empty())
            {
                try
                {
                    out.insert(std::stoull(roleStr));
                }
                catch (...)
                {
                }
            }

            return out;
        }

        static std::string TruncateForDiscord(std::string const& text)
        {
            if (text.size() <= DISCORD_MESSAGE_LIMIT)
                return text;
            return text.substr(0, DISCORD_MESSAGE_LIMIT - 3) + "...";
        }

        static std::string EscapeFmtBraces(std::string const& value)
        {
            std::string out;
            out.reserve(value.size() + 8);
            for (char ch : value)
            {
                if (ch == '{' || ch == '}')
                    out.push_back(ch);
                out.push_back(ch);
            }

            return out;
        }

        static std::unordered_map<uint64_t, std::unordered_set<std::string>> ParseRoleMappings(std::string const& value)
        {
            std::unordered_map<uint64_t, std::unordered_set<std::string>> out;
            for (std::string const& entry : Split(value, ';'))
            {
                size_t sep = entry.find(':');
                if (sep == std::string::npos)
                    continue;

                std::string roleStr = Trim(entry.substr(0, sep));
                std::string categoriesStr = Trim(entry.substr(sep + 1));
                if (roleStr.empty() || categoriesStr.empty())
                    continue;

                uint64_t roleId = 0;
                try
                {
                    roleId = std::stoull(roleStr);
                }
                catch (...)
                {
                    continue;
                }

                auto& setRef = out[roleId];
                for (std::string const& cat : Split(categoriesStr, ','))
                    setRef.insert(ToLower(cat));
            }

            return out;
        }

        static std::string GetCommandRoot(std::string const& command)
        {
            std::string trimmed = Trim(command);
            if (trimmed.empty())
                return {};

            if (trimmed.front() == '.' || trimmed.front() == '!')
                trimmed.erase(trimmed.begin());

            trimmed = Trim(trimmed);
            if (trimmed.empty())
                return {};

            size_t space = trimmed.find(' ');
            std::string root = (space == std::string::npos) ? trimmed : trimmed.substr(0, space);
            return ToLower(root);
        }

        static std::string GetCommandCategory(std::string const& root)
        {
            std::string token = ToLower(root);
            if (token == "ticket" || token == "tickets")
                return "ticket";
            if (token == "tele" || token == "teleport" || token == "go")
                return "tele";
            if (token == "gm" || token == "gminfo" || token == "gmname")
                return "gm";
            if (token == "ban" || token == "unban")
                return "ban";
            if (token == "account" || token == "acc")
                return "account";
            if (token == "character" || token == "char")
                return "character";
            if (token == "lookup" || token == "who" || token == "name")
                return "lookup";
            if (token == "server" || token == "shutdown" || token == "restart")
                return "server";
            if (token == "debug")
                return "debug";
            return "misc";
        }

        static bool TryParseTicketIdFromThreadName(std::string const& name, uint32& outTicketId)
        {
            outTicketId = 0;
            constexpr char prefix[] = "ticket-";
            if (name.rfind(prefix, 0) != 0)
                return false;

            size_t start = sizeof(prefix) - 1;
            size_t end = name.find('-', start);
            std::string idStr = (end == std::string::npos) ? name.substr(start) : name.substr(start, end - start);
            if (idStr.empty())
                return false;

            try
            {
                outTicketId = static_cast<uint32>(std::stoul(idStr));
                return outTicketId > 0;
            }
            catch (...)
            {
                return false;
            }
        }

        static bool HasRoleForCategory(std::unordered_map<uint64_t, std::unordered_set<std::string>> const& roleMap,
            std::vector<dpp::snowflake> const& roles, std::string const& category)
        {
            if (roleMap.empty())
                return true;

            std::string cat = ToLower(category);
            for (dpp::snowflake roleId : roles)
            {
                auto it = roleMap.find(static_cast<uint64_t>(roleId));
                if (it != roleMap.end() && it->second.count(cat) > 0)
                    return true;
            }

            return false;
        }

        static bool FindJsonKeyStart(std::string const& payload, std::string const& key, size_t& pos)
        {
            std::string needle = Acore::StringFormat("\"{}\":", key);
            pos = payload.find(needle);
            if (pos != std::string::npos)
            {
                pos += needle.size();
                return true;
            }

            std::string escapedNeedle = Acore::StringFormat("\\\"{}\\\":", key);
            pos = payload.find(escapedNeedle);
            if (pos != std::string::npos)
            {
                pos += escapedNeedle.size();
                return true;
            }

            return false;
        }

        static bool ExtractJsonBlock(std::string const& payload, std::string const& key, std::string& out)
        {
            size_t start = 0;
            if (!FindJsonKeyStart(payload, key, start))
                return false;

            start = payload.find('{', start);
            if (start == std::string::npos)
                return false;

            int depth = 0;
            bool inString = false;
            bool escape = false;
            for (size_t i = start; i < payload.size(); ++i)
            {
                char ch = payload[i];
                if (escape)
                {
                    escape = false;
                    continue;
                }

                if (ch == '\\')
                {
                    escape = true;
                    continue;
                }

                if (ch == '"')
                {
                    inString = !inString;
                    continue;
                }

                if (inString)
                    continue;

                if (ch == '{')
                    ++depth;
                else if (ch == '}')
                {
                    --depth;
                    if (depth == 0)
                    {
                        out = payload.substr(start, i - start + 1);
                        return true;
                    }
                }
            }

            return false;
        }

        static bool ExtractJsonString(std::string const& payload, std::string const& key, std::string& out)
        {
            size_t pos = 0;
            if (!FindJsonKeyStart(payload, key, pos))
                return false;

            while (pos < payload.size() && std::isspace(static_cast<unsigned char>(payload[pos])))
                ++pos;

            if (pos >= payload.size())
                return false;

            if (payload[pos] == '\\' && pos + 1 < payload.size() && payload[pos + 1] == '"')
                pos += 2;
            else if (payload[pos] == '"')
                ++pos;
            else
                return false;

            std::string value;
            value.reserve(32);
            bool escape = false;
            for (; pos < payload.size(); ++pos)
            {
                char ch = payload[pos];
                if (escape)
                {
                    switch (ch)
                    {
                        case 'n': value.push_back('\n'); break;
                        case 'r': value.push_back('\r'); break;
                        case 't': value.push_back('\t'); break;
                        case '\\': value.push_back('\\'); break;
                        case '"': value.push_back('"'); break;
                        default: value.push_back(ch); break;
                    }
                    escape = false;
                    continue;
                }

                if (ch == '\\')
                {
                    escape = true;
                    continue;
                }

                if (ch == '"')
                {
                    out = value;
                    return true;
                }

                value.push_back(ch);
            }

            return false;
        }

        static bool ExtractJsonNumber(std::string const& payload, std::string const& key, std::string& out)
        {
            size_t pos = 0;
            if (!FindJsonKeyStart(payload, key, pos))
                return false;

            while (pos < payload.size() && std::isspace(static_cast<unsigned char>(payload[pos])))
                ++pos;

            size_t end = payload.find_first_of(",}", pos);
            if (end == std::string::npos)
                return false;

            out = Trim(payload.substr(pos, end - pos));
            return !out.empty();
        }

        static bool ExtractJsonUint(std::string const& payload, std::string const& key, uint32& out)
        {
            std::string number;
            if (!ExtractJsonNumber(payload, key, number))
                return false;

            try
            {
                out = static_cast<uint32>(std::stoul(number));
                return true;
            }
            catch (...)
            {
                return false;
            }
        }

        static bool BuildTicketEmbed(std::string const& eventType, std::string const& payload, dpp::embed& embed)
        {
            std::string ticketBlock;
            if (!ExtractJsonBlock(payload, "ticket", ticketBlock))
                return false;

            uint32 id = 0;
            std::string player;
            std::string message;
            std::string status;
            std::string assignedTo;
            std::string comment;
            std::string response;

            ExtractJsonUint(ticketBlock, "id", id);
            ExtractJsonString(ticketBlock, "player", player);
            ExtractJsonString(ticketBlock, "message", message);
            ExtractJsonString(ticketBlock, "status", status);
            ExtractJsonString(ticketBlock, "assignedTo", assignedTo);
            ExtractJsonString(ticketBlock, "comment", comment);
            ExtractJsonString(ticketBlock, "response", response);

            if (player.empty())
                player = "unknown";
            if (status.empty())
                status = "unknown";
            if (assignedTo.empty())
                assignedTo = "unassigned";

            embed = dpp::embed();
            embed.set_title(Acore::StringFormat("Ticket #{} - {}", id, player));
            embed.set_description(message);

            embed.add_field("Status", status, true);
            embed.add_field("Assigned", assignedTo, true);

            if (!comment.empty())
                embed.add_field("Comment", TruncateForDiscord(comment), false);
            if (!response.empty())
                embed.add_field("Response", TruncateForDiscord(response), false);

            if (eventType == "ticket_close" || eventType == "ticket_resolve")
                embed.set_color(0xFF5555);
            else if (eventType == "ticket_update" || eventType == "ticket_status")
                embed.set_color(0xF2C94C);
            else
                embed.set_color(0x2D9CDB);

            return true;
        }

        static bool BuildWhisperEmbed(std::string const& eventType, std::string const& payload, dpp::embed& embed)
        {
            std::string block;
            if (!ExtractJsonBlock(payload, "whisper", block))
                return false;

            std::string player;
            std::string gmName;
            std::string message;
            uint32 ticketId = 0;

            ExtractJsonString(block, "player", player);
            ExtractJsonString(block, "gmName", gmName);
            ExtractJsonString(block, "message", message);
            ExtractJsonUint(block, "ticketId", ticketId);

            if (player.empty())
                player = "unknown";
            if (gmName.empty())
                gmName = "unknown";

            embed = dpp::embed();
            embed.set_title(eventType == "gm_whisper" ? "GM Reply" : "Player Reply");
            embed.set_description(message);
            embed.add_field("Player", player, true);
            embed.add_field("GM", gmName, true);
            embed.add_field("Ticket", Acore::StringFormat("{}", ticketId), true);
            embed.set_color(eventType == "gm_whisper" ? 0x6FCF97 : 0x9B51E0);
            return true;
        }

        static bool BuildCommandResultEmbed(std::string const& payload, dpp::embed& embed)
        {
            std::string block;
            if (!ExtractJsonBlock(payload, "command", block))
                return false;

            uint32 id = 0;
            std::string status;
            std::string output;

            ExtractJsonUint(block, "id", id);
            ExtractJsonString(block, "status", status);
            ExtractJsonString(block, "output", output);

            if (status.empty())
                status = "unknown";

            embed = dpp::embed();
            embed.set_title(Acore::StringFormat("Command Result #{}", id));
            embed.set_description(output);
            embed.add_field("Status", status, true);
            embed.set_color(status == "ok" ? 0x6FCF97 : 0xEB5757);
            return true;
        }

        static std::string FormatTicketRoomName(std::string const& pattern, std::string const& player, uint32 ticketId)
        {
            std::string name = pattern.empty() ? "ticket-{id}-{player}" : pattern;
            size_t pos = name.find("{id}");
            while (pos != std::string::npos)
            {
                name.replace(pos, 4, std::to_string(ticketId));
                pos = name.find("{id}");
            }

            pos = name.find("{player}");
            while (pos != std::string::npos)
            {
                name.replace(pos, 8, player);
                pos = name.find("{player}");
            }

            // sanitize
            for (char& ch : name)
            {
                if (std::isspace(static_cast<unsigned char>(ch)))
                    ch = '-';
                else if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_')
                    ch = '-';
            }

            return ToLower(name);
        }

        static bool GetTicketRoomChannel(uint32 ticketId, uint64_t& channelId)
        {
            QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
                "SELECT channel_id FROM gm_discord_ticket_room WHERE ticket_id={} LIMIT 1",
                ticketId));

            if (!result)
                return false;

            Field* fields = result->Fetch();
            channelId = fields[0].Get<uint64_t>();
            return true;
        }

        static void UpsertTicketRoom(uint32 ticketId, uint64_t channelId, uint64_t guildId)
        {
            CharacterDatabase.Execute(Acore::StringFormat(
                "REPLACE INTO gm_discord_ticket_room (ticket_id, channel_id, guild_id, created_at) VALUES ({}, {}, {}, NOW())",
                ticketId, channelId, guildId));
        }

        static void MarkTicketRoomArchived(uint32 ticketId)
        {
            CharacterDatabase.Execute(Acore::StringFormat(
                "UPDATE gm_discord_ticket_room SET archived_at=NOW() WHERE ticket_id={} LIMIT 1",
                ticketId));
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

            Field* fields = result->Fetch();
            gmName = fields[0].Get<std::string>();
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
        _ticketRoomsEnabled = sConfigMgr->GetOption<bool>("GMDiscord.Bot.TicketRooms.Enable", false);
        _ticketRoomCategoryId = sConfigMgr->GetOption<uint64_t>("GMDiscord.Bot.TicketRooms.CategoryId", 0);
        _ticketRoomArchiveCategoryId = sConfigMgr->GetOption<uint64_t>("GMDiscord.Bot.TicketRooms.ArchiveCategoryId", 0);
        _ticketRoomNameFormat = sConfigMgr->GetOption<std::string>("GMDiscord.Bot.TicketRooms.NameFormat", "ticket-{id}-{player}");
        _ticketRoomPostUpdates = sConfigMgr->GetOption<bool>("GMDiscord.Bot.TicketRooms.PostUpdates", true);
        _ticketRoomArchiveOnClose = sConfigMgr->GetOption<bool>("GMDiscord.Bot.TicketRooms.ArchiveOnClose", true);
        _ticketRoomAllowedRoleIds = ParseRoleList(sConfigMgr->GetOption<std::string>("GMDiscord.Bot.TicketRooms.AllowedRoles", ""));
        _roleMappingsRaw = sConfigMgr->GetOption<std::string>("GMDiscord.Bot.RoleMappings", "");
        _roleCategoryMap = ParseRoleMappings(_roleMappingsRaw);
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
        cluster->intents = dpp::i_default_intents | dpp::i_message_content;
        _cluster = cluster;
        cluster->on_log([=](const dpp::log_t& event)
        {
            LOG_INFO("module.gm_discord", "DPP: {}", EscapeFmtBraces(event.message));
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

                dpp::slashcommand assign("gm-ticket-assign", "Assign a ticket to a GM", appId);
                assign.add_option(dpp::command_option(dpp::co_integer, "ticket_id", "Ticket ID", true));
                assign.add_option(dpp::command_option(dpp::co_string, "gm_name", "GM character name", true));

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

                if (_guildId)
                    cluster->guild_command_create(assign, _guildId);
                else
                    cluster->global_command_create(assign);

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

                        uint32 ticketId = 0;
                        bool hasTicketId = false;
                        if (eventType.rfind("ticket_", 0) == 0)
                        {
                            std::string ticketBlock;
                            if (ExtractJsonBlock(payload, "ticket", ticketBlock))
                                hasTicketId = ExtractJsonUint(ticketBlock, "id", ticketId);
                        }
                        else if (eventType == "player_whisper" || eventType == "gm_whisper")
                        {
                            std::string whisperBlock;
                            if (ExtractJsonBlock(payload, "whisper", whisperBlock))
                                hasTicketId = ExtractJsonUint(whisperBlock, "ticketId", ticketId);
                        }

                        dpp::embed embed;
                        bool hasEmbed = false;
                        if (eventType == "command_result")
                            hasEmbed = BuildCommandResultEmbed(payload, embed);
                        else if (eventType == "player_whisper" || eventType == "gm_whisper")
                            hasEmbed = BuildWhisperEmbed(eventType, payload, embed);
                        else if (eventType.rfind("ticket_", 0) == 0)
                            hasEmbed = BuildTicketEmbed(eventType, payload, embed);

                        if (_outboxChannelId)
                        {
                            bool createThread = (eventType == "ticket_create" && hasTicketId);
                            if (createThread)
                            {
                                std::string playerName;
                                if (!ExtractJsonString(payload, "player", playerName))
                                    playerName = "player";

                                std::string threadName = FormatTicketRoomName("ticket-{id}-{player}", playerName, ticketId);
                                dpp::message outMessage = hasEmbed
                                    ? dpp::message(_outboxChannelId, "").add_embed(embed)
                                    : dpp::message(_outboxChannelId, TruncateForDiscord(Acore::StringFormat("[{}] {}", eventType, payload)));

                                clusterPtr->message_create(outMessage, [this, clusterPtr, threadName, ticketId](const dpp::confirmation_callback_t& cb)
                                {
                                    if (cb.is_error())
                                        return;

                                    auto created = std::get<dpp::message>(cb.value);
                                    clusterPtr->thread_create_with_message(threadName, created.channel_id, created.id, 1440, 0,
                                        [this, ticketId](const dpp::confirmation_callback_t& threadCb)
                                        {
                                            if (threadCb.is_error())
                                                return;

                                            auto createdThread = std::get<dpp::thread>(threadCb.value);
                                            uint64_t threadId = static_cast<uint64_t>(createdThread.id);
                                            _ticketThreadIds[ticketId] = threadId;
                                            _threadTicketIds[threadId] = ticketId;
                                        });
                                });
                            }
                            else if (hasEmbed)
                            {
                                clusterPtr->message_create(dpp::message(_outboxChannelId, "").add_embed(embed));
                            }
                            else
                            {
                                clusterPtr->message_create(dpp::message(_outboxChannelId, TruncateForDiscord(Acore::StringFormat("[{}] {}", eventType, payload))));
                            }
                        }

                        if (_ticketRoomsEnabled && hasTicketId && _ticketRoomCategoryId && _guildId)
                        {
                            uint64_t channelId = 0;
                            GetTicketRoomChannel(ticketId, channelId);

                            if (channelId == 0 && eventType == "ticket_create")
                            {
                                std::string playerName;
                                if (!ExtractJsonString(payload, "player", playerName))
                                    playerName = "player";
                                std::string channelName = FormatTicketRoomName(_ticketRoomNameFormat, playerName, ticketId);

                                dpp::channel channel;
                                channel.set_name(channelName);
                                channel.set_type(dpp::CHANNEL_TEXT);
                                channel.set_parent_id(_ticketRoomCategoryId);

                                std::vector<dpp::permission_overwrite> overwrites;

                                std::unordered_set<uint64_t> allowedRoles = _ticketRoomAllowedRoleIds;
                                if (allowedRoles.empty())
                                {
                                    for (std::unordered_map<uint64_t, std::unordered_set<std::string>>::const_iterator it = _roleCategoryMap.begin();
                                        it != _roleCategoryMap.end(); ++it)
                                        allowedRoles.insert(it->first);
                                }

                                bool allowEveryone = allowedRoles.empty();
                                if (!allowEveryone)
                                {
                                    dpp::permission_overwrite everyone;
                                    everyone.id = _guildId;
                                    everyone.type = dpp::ot_role;
                                    everyone.allow = 0;
                                    everyone.deny = dpp::p_view_channel;
                                    overwrites.push_back(everyone);
                                }
                                else
                                {
                                    allowedRoles.insert(_guildId);
                                }

                                for (uint64_t roleId : allowedRoles)
                                {
                                    dpp::permission_overwrite roleOverwrite;
                                    roleOverwrite.id = roleId;
                                    roleOverwrite.type = dpp::ot_role;
                                    roleOverwrite.allow = dpp::p_view_channel | dpp::p_send_messages | dpp::p_read_message_history;
                                    roleOverwrite.deny = 0;
                                    overwrites.push_back(roleOverwrite);
                                }

                                channel.permission_overwrites = overwrites;

                                clusterPtr->channel_create(channel, [=](const dpp::confirmation_callback_t& cb)
                                {
                                    if (!cb.is_error())
                                    {
                                        auto created = std::get<dpp::channel>(cb.value);
                                        UpsertTicketRoom(ticketId, static_cast<uint64_t>(created.id), _guildId);
                                    }
                                });

                            }

                            if (channelId != 0 && _ticketRoomPostUpdates)
                            {
                                if (hasEmbed)
                                    clusterPtr->message_create(dpp::message(channelId, "").add_embed(embed));
                                else
                                    clusterPtr->message_create(dpp::message(channelId, TruncateForDiscord(Acore::StringFormat("[{}] {}", eventType, payload))));
                            }

                            if (eventType == "ticket_close" || eventType == "ticket_resolve")
                            {
                                auto threadIt = _ticketThreadIds.find(ticketId);
                                if (threadIt != _ticketThreadIds.end())
                                {
                                    uint64_t threadId = threadIt->second;
                                    clusterPtr->thread_get(threadId, [this, clusterPtr, threadId](const dpp::confirmation_callback_t& cb)
                                    {
                                        if (cb.is_error())
                                        {
                                            _threadTicketIds.erase(threadId);
                                            return;
                                        }

                                        auto threadInfo = std::get<dpp::thread>(cb.value);
                                        threadInfo.metadata.auto_archive_duration = 1440;
                                        threadInfo.metadata.archived = true;
                                        threadInfo.metadata.locked = true;
                                        clusterPtr->thread_edit(threadInfo);
                                    });
                                    _threadTicketIds.erase(threadIt->second);
                                    _ticketThreadIds.erase(threadIt);
                                }

                                if (!_ticketRoomArchiveOnClose)
                                    continue;

                                dpp::channel ch;
                                ch.id = channelId;
                                if (_ticketRoomArchiveCategoryId)
                                    ch.set_parent_id(_ticketRoomArchiveCategoryId);

                                if (!hasEmbed)
                                {
                                    clusterPtr->message_create(dpp::message(channelId, "Ticket closed."));
                                }

                                clusterPtr->channel_edit(ch);
                                MarkTicketRoomArchived(ticketId);
                            }
                        }

                        MarkOutboxDispatched(id);
                    } while (result->NextRow());
                }, 5);
            }
        });

        cluster->on_message_create([=](const dpp::message_create_t& event)
        {
            if (!_outboxChannelId)
                return;

            if (event.msg.author.is_bot())
                return;

            auto* clusterPtr = static_cast<dpp::cluster*>(_cluster);
            if (!clusterPtr)
                return;

            uint64 threadId = static_cast<uint64_t>(event.msg.channel_id);
            uint64 discordUserId = event.msg.author.id;

            std::string displayName = event.msg.member.get_nickname();
            if (displayName.empty())
                displayName = event.msg.author.username;
            displayName = SanitizeWhisperName(displayName);
            std::string content = Trim(event.msg.content);
            if (content.empty())
                return;

            auto processTicket = [this, clusterPtr, threadId, discordUserId, displayName, content](uint32 ticketId)
            {
                GmTicket* ticket = sTicketMgr->GetTicket(ticketId);
                if (!ticket || ticket->IsClosed())
                {
                    clusterPtr->message_create(dpp::message(threadId, "Ticket is closed or unavailable."));
                    return;
                }

                std::string gmName;
                if (!GetGmNameForDiscordUser(discordUserId, gmName))
                {
                    clusterPtr->message_create(dpp::message(threadId, "You are not linked. Use in-game .discord link <secret>."));
                    return;
                }

                std::string senderName = displayName.empty() ? gmName : displayName;
                std::string payload = ticket->GetPlayerName() + "|" + senderName + "|" + content;
                InsertInboxAction(discordUserId, "whisper", payload);
            };

            auto threadIt = _threadTicketIds.find(threadId);
            if (threadIt != _threadTicketIds.end())
            {
                processTicket(threadIt->second);
                return;
            }

            clusterPtr->thread_get(threadId, [this, threadId, processTicket](const dpp::confirmation_callback_t& cb)
            {
                if (cb.is_error())
                    return;

                auto threadInfo = std::get<dpp::thread>(cb.value);
                uint32 ticketId = 0;
                if (!TryParseTicketIdFromThreadName(threadInfo.name, ticketId))
                    return;

                _threadTicketIds[threadId] = ticketId;
                _ticketThreadIds[ticketId] = threadId;
                processTicket(ticketId);
            });
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
            std::vector<dpp::snowflake> roles = event.command.member.get_roles();

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
                std::string category = GetCommandCategory(GetCommandRoot(cmd));
                if (!HasRoleForCategory(_roleCategoryMap, roles, category))
                {
                    event.reply(dpp::message("You are not allowed to run this command category.").set_flags(dpp::m_ephemeral));
                    return;
                }

                InsertInboxAction(discordUserId, "command", cmd);
                event.reply(dpp::message("Command queued.").set_flags(dpp::m_ephemeral));
                return;
            }

            if (name == "gm-whisper")
            {
                std::string player = std::get<std::string>(event.get_parameter("player"));
                std::string message = std::get<std::string>(event.get_parameter("message"));

                if (!HasRoleForCategory(_roleCategoryMap, roles, "whisper"))
                {
                    event.reply(dpp::message("You are not allowed to send whispers.").set_flags(dpp::m_ephemeral));
                    return;
                }

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

            if (name == "gm-ticket-assign")
            {
                if (!HasRoleForCategory(_roleCategoryMap, roles, "ticket"))
                {
                    event.reply(dpp::message("You are not allowed to assign tickets.").set_flags(dpp::m_ephemeral));
                    return;
                }

                uint32 ticketId = static_cast<uint32>(std::get<int64_t>(event.get_parameter("ticket_id")));
                std::string gmName = std::get<std::string>(event.get_parameter("gm_name"));

                if (!ticketId || gmName.empty())
                {
                    event.reply(dpp::message("Invalid ticket assignment input.").set_flags(dpp::m_ephemeral));
                    return;
                }

                std::string payload = Acore::StringFormat("{}|{}", ticketId, gmName);
                InsertInboxAction(discordUserId, "ticket_assign", payload);
                event.reply(dpp::message("Ticket assignment queued.").set_flags(dpp::m_ephemeral));
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
