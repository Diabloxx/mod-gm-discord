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

#include "AccountMgr.h"
#include "Argon2.h"
#include "BigNumber.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GMDiscordBot.h"
#include "GameTime.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TicketMgr.h"
#include "World.h"
#include "WorldPacket.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace GMDiscord
{
	struct Settings
	{
		bool enabled = true;
		bool outboxEnabled = true;
		bool whisperEnabled = true;
		bool allowAllCommands = false;
		bool rateLimitEnabled = true;
		uint32 pollIntervalMs = 1000;
		uint32 maxBatchSize = 25;
		uint32 minSecurity = SEC_GAMEMASTER;
		uint32 linkCodeTtlSeconds = 900;
		uint32 secretTtlSeconds = 900;
		uint32 maxResultLength = 4000;
		uint32 rateLimitWindowSeconds = 10;
		uint32 rateLimitMaxActions = 5;
		uint32 rateLimitMinIntervalMs = 500;
		uint32 auditPayloadMax = 1024;
		std::vector<std::string> commandAllowList;
		std::unordered_map<std::string, uint32> categoryMinSecurity;
	};

	static Settings g_Settings;
	static std::unordered_map<uint64, std::deque<uint64>> g_RateLimiter;

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

	static std::string ToLower(std::string_view value)
	{
		std::string out(value);
		std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return out;
	}

	static std::vector<std::string> SplitAllowList(std::string_view value)
	{
		std::vector<std::string> out;
		std::string current;
		for (char ch : value)
		{
			if (ch == ';' || ch == ',')
			{
				std::string trimmed = Trim(current);
				if (!trimmed.empty())
					out.emplace_back(ToLower(trimmed));
				current.clear();
			}
			else
			{
				current.push_back(ch);
			}
		}

		std::string trimmed = Trim(current);
		if (!trimmed.empty())
			out.emplace_back(ToLower(trimmed));

		return out;
	}

	static std::string EscapeJson(std::string_view input)
	{
		std::string out;
		out.reserve(input.size() + 8);
		for (char ch : input)
		{
			switch (ch)
			{
				case '\\': out += "\\\\"; break;
				case '"': out += "\\\""; break;
				case '\n': out += "\\n"; break;
				case '\r': out += "\\r"; break;
				case '\t': out += "\\t"; break;
				default: out += ch; break;
			}
		}
		return out;
	}

	static std::string EscapeSql(std::string const& input)
	{
		std::string escaped = input;
		CharacterDatabase.EscapeString(escaped);
		return escaped;
	}

	static void LoadSettings()
	{
		g_Settings.enabled = sConfigMgr->GetOption<bool>("GMDiscord.Enable", true);
		g_Settings.outboxEnabled = sConfigMgr->GetOption<bool>("GMDiscord.Outbox.Enable", true);
		g_Settings.whisperEnabled = sConfigMgr->GetOption<bool>("GMDiscord.Whisper.Enable", true);
		g_Settings.allowAllCommands = sConfigMgr->GetOption<bool>("GMDiscord.CommandAllowAll", false);
		g_Settings.rateLimitEnabled = sConfigMgr->GetOption<bool>("GMDiscord.RateLimit.Enable", true);
		g_Settings.pollIntervalMs = sConfigMgr->GetOption<uint32>("GMDiscord.PollIntervalMs", 1000);
		g_Settings.maxBatchSize = sConfigMgr->GetOption<uint32>("GMDiscord.MaxBatchSize", 25);
		g_Settings.minSecurity = sConfigMgr->GetOption<uint32>("GMDiscord.MinSecurityLevel", SEC_GAMEMASTER);
		g_Settings.linkCodeTtlSeconds = sConfigMgr->GetOption<uint32>("GMDiscord.LinkCodeTtlSeconds", 900);
		g_Settings.secretTtlSeconds = sConfigMgr->GetOption<uint32>("GMDiscord.SecretTtlSeconds", 900);
		g_Settings.maxResultLength = sConfigMgr->GetOption<uint32>("GMDiscord.MaxResultLength", 4000);
		g_Settings.rateLimitWindowSeconds = sConfigMgr->GetOption<uint32>("GMDiscord.RateLimit.WindowSeconds", 10);
		g_Settings.rateLimitMaxActions = sConfigMgr->GetOption<uint32>("GMDiscord.RateLimit.MaxActions", 5);
		g_Settings.rateLimitMinIntervalMs = sConfigMgr->GetOption<uint32>("GMDiscord.RateLimit.MinIntervalMs", 500);
		g_Settings.auditPayloadMax = sConfigMgr->GetOption<uint32>("GMDiscord.Audit.PayloadMax", 1024);

		std::string allowList = sConfigMgr->GetOption<std::string>("GMDiscord.CommandAllowList", ".ticket;.gm");
		g_Settings.commandAllowList = SplitAllowList(allowList);

		g_Settings.categoryMinSecurity.clear();
		auto setCategory = [&](std::string const& name, uint32 def)
		{
			g_Settings.categoryMinSecurity[name] = sConfigMgr->GetOption<uint32>(
				Acore::StringFormat("GMDiscord.CommandCategory.{}.MinSecurity", name), def);
		};
		setCategory("ticket", SEC_GAMEMASTER);
		setCategory("tele", SEC_GAMEMASTER);
		setCategory("gm", SEC_GAMEMASTER);
		setCategory("ban", SEC_ADMINISTRATOR);
		setCategory("account", SEC_ADMINISTRATOR);
		setCategory("character", SEC_GAMEMASTER);
		setCategory("lookup", SEC_MODERATOR);
		setCategory("server", SEC_ADMINISTRATOR);
		setCategory("debug", SEC_ADMINISTRATOR);
		setCategory("whisper", SEC_GAMEMASTER);
		setCategory("misc", SEC_GAMEMASTER);
	}

	static bool IsCommandAllowed(std::string_view command)
	{
		if (g_Settings.allowAllCommands)
			return true;

		std::string trimmed = ToLower(Trim(command));
		if (trimmed.empty())
			return false;

		for (std::string const& prefix : g_Settings.commandAllowList)
		{
			if (!prefix.empty() && trimmed.rfind(prefix, 0) == 0)
				return true;
		}

		return false;
	}

	static std::string GetCommandRoot(std::string_view command)
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

	static std::string GetCommandCategory(std::string_view root)
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

	static uint32 GetCategoryMinSecurity(std::string const& category)
	{
		auto it = g_Settings.categoryMinSecurity.find(category);
		if (it != g_Settings.categoryMinSecurity.end())
			return it->second;
		return g_Settings.minSecurity;
	}

	static bool CheckRateLimit(uint64 discordUserId, std::string const& action, std::string& reason)
	{
		if (!g_Settings.rateLimitEnabled)
			return true;

		uint64 nowMs = GameTime::GetGameTime().count();
		uint64 windowMs = uint64(g_Settings.rateLimitWindowSeconds) * 1000;
		auto& bucket = g_RateLimiter[discordUserId];
		while (!bucket.empty() && nowMs - bucket.front() > windowMs)
			bucket.pop_front();

		if (!bucket.empty() && g_Settings.rateLimitMinIntervalMs > 0 && nowMs - bucket.back() < g_Settings.rateLimitMinIntervalMs)
		{
			reason = Acore::StringFormat("Rate limit for {}: wait {} ms", action, g_Settings.rateLimitMinIntervalMs);
			return false;
		}

		if (g_Settings.rateLimitMaxActions > 0 && bucket.size() >= g_Settings.rateLimitMaxActions)
		{
			reason = Acore::StringFormat("Rate limit exceeded for {}", action);
			return false;
		}

		bucket.push_back(nowMs);
		return true;
	}

	static std::string TruncateAuditPayload(std::string const& payload)
	{
		if (g_Settings.auditPayloadMax == 0 || payload.size() <= g_Settings.auditPayloadMax)
			return payload;
		return payload.substr(0, g_Settings.auditPayloadMax);
	}

	static void LogAudit(uint64 discordUserId, uint32 accountId, std::string const& action, std::string const& category,
		std::string const& status, std::string const& detail, std::string const& payload)
	{
		std::string actionEsc = EscapeSql(action);
		std::string categoryEsc = EscapeSql(category);
		std::string statusEsc = EscapeSql(status);
		std::string detailEsc = EscapeSql(detail);
		std::string payloadEsc = EscapeSql(TruncateAuditPayload(payload));
		CharacterDatabase.Execute(Acore::StringFormat(
			"INSERT INTO gm_discord_audit (discord_user_id, account_id, action, category, status, detail, payload) "
			"VALUES ({}, {}, '{}', '{}', '{}', '{}', '{}')",
			discordUserId, accountId, actionEsc, categoryEsc, statusEsc, detailEsc, payloadEsc));
	}

	static void EnqueueOutbox(std::string const& eventType, std::string const& payload)
	{
		if (!g_Settings.enabled || !g_Settings.outboxEnabled)
			return;

		std::string eventEsc = EscapeSql(eventType);
		std::string payloadEsc = EscapeSql(payload);
		CharacterDatabase.Execute(Acore::StringFormat(
			"INSERT INTO gm_discord_outbox (event_type, payload) VALUES ('{}', '{}')",
			eventEsc, payloadEsc));
	}

	static void MarkInboxResult(uint32 id, std::string const& status, std::string const& result)
	{
		std::string statusEsc = EscapeSql(status);
		std::string resultEsc = EscapeSql(result);
		CharacterDatabase.Execute(Acore::StringFormat(
			"UPDATE gm_discord_inbox SET processed=1, processed_at=NOW(), status='{}', result='{}' WHERE id={}",
			statusEsc, resultEsc, id));
	}

	static void MarkInboxProcessing(uint32 id)
	{
		CharacterDatabase.Execute(Acore::StringFormat(
			"UPDATE gm_discord_inbox SET processed=2 WHERE id={} AND processed=0",
			id));
	}

	static bool ParseWhisperPayload(std::string const& payload, std::string& playerName, std::string& gmName, std::string& message)
	{
		size_t first = payload.find('|');
		if (first == std::string::npos)
			return false;
		size_t second = payload.find('|', first + 1);
		if (second == std::string::npos)
			return false;

		playerName = payload.substr(0, first);
		gmName = payload.substr(first + 1, second - first - 1);
		message = payload.substr(second + 1);
		playerName = Trim(playerName);
		gmName = Trim(gmName);
		message = Trim(message);
		return !playerName.empty() && !gmName.empty() && !message.empty();
	}

	static bool ParseTicketAssignPayload(std::string const& payload, uint32& ticketId, std::string& gmName)
	{
		size_t sep = payload.find('|');
		if (sep == std::string::npos)
			return false;

		std::string idStr = Trim(payload.substr(0, sep));
		gmName = Trim(payload.substr(sep + 1));
		if (idStr.empty() || gmName.empty())
			return false;

		try
		{
			ticketId = static_cast<uint32>(std::stoul(idStr));
		}
		catch (...)
		{
			return false;
		}

		return ticketId > 0;
	}

	static void SendWhisperToPlayer(Player* player, std::string const& gmName, std::string const& message)
	{
		if (!player || !player->GetSession())
			return;

		WorldPacket data;
		ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_UNIVERSAL,
			ObjectGuid::Empty, player->GetGUID(), message, 0, gmName, player->GetName());
		player->GetSession()->SendPacket(&data);
	}

	static void UpsertWhisperSession(Player* player, uint64 discordUserId, std::string const& gmName)
	{
		if (!player)
			return;

		std::string gmEsc = EscapeSql(gmName);
		CharacterDatabase.Execute(Acore::StringFormat(
			"REPLACE INTO gm_discord_whisper_session (player_guid, discord_user_id, gm_name, updated_at) "
			"VALUES ({}, {}, '{}', NOW())",
			player->GetGUID().GetRawValue(), discordUserId, gmEsc));
	}

	static bool TryGetWhisperSession(std::string const& gmName, uint64& discordUserId)
	{
		std::string gmEsc = EscapeSql(gmName);
		QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
			"SELECT discord_user_id FROM gm_discord_whisper_session WHERE LOWER(gm_name) = LOWER('{}') LIMIT 1",
			gmEsc));

		if (!result)
			return false;

		discordUserId = (*result)[0].Get<uint64>();
		return true;
	}

	static bool VerifyAndLinkSecret(uint64 discordUserId, std::string const& secret, uint32& outAccountId)
	{
		outAccountId = 0;
		QueryResult result = CharacterDatabase.Query(
			"SELECT account_id, secret_hash FROM gm_discord_link WHERE secret_hash IS NOT NULL AND secret_expires_at > NOW()");

		if (!result)
			return false;

		do
		{
			Field* fields = result->Fetch();
			uint32 accountId = fields[0].Get<uint32>();
			std::string hash = fields[1].Get<std::string>();
			if (hash.empty())
				continue;

			if (Acore::Crypto::Argon2::Verify(secret, hash))
			{
				CharacterDatabase.Execute(Acore::StringFormat(
					"UPDATE gm_discord_link SET discord_user_id={}, verified=1, secret_hash=NULL, secret_expires_at=NULL, updated_at=NOW() WHERE account_id={} LIMIT 1",
					discordUserId, accountId));
				outAccountId = accountId;
				return true;
			}
		} while (result->NextRow());

		return false;
	}

	static bool CheckCommandPermissions(std::string_view command, uint32 accountId, std::string& outCategory, std::string& outReason)
	{
		if (!IsCommandAllowed(command))
		{
			outReason = "Command not allowed by GMDiscord.CommandAllowList";
			return false;
		}

		std::string root = GetCommandRoot(command);
		outCategory = GetCommandCategory(root);
		uint32 security = AccountMgr::GetSecurity(accountId);
		uint32 categoryMin = GetCategoryMinSecurity(outCategory);
		uint32 required = std::max(g_Settings.minSecurity, categoryMin);

		if (security < required)
		{
			outReason = Acore::StringFormat("Account security too low for category '{}'", outCategory);
			return false;
		}

		return true;
	}

	struct CommandContext
	{
		uint32 id = 0;
		uint64 discordUserId = 0;
		uint32 accountId = 0;
		std::string output;

		explicit CommandContext(uint32 commandId, uint64 discordId, uint32 accId)
			: id(commandId), discordUserId(discordId), accountId(accId) { }

		static void Print(void* arg, std::string_view text)
		{
			if (text.empty())
				return;

			CommandContext* ctx = static_cast<CommandContext*>(arg);
			if (!ctx)
				return;

			if (ctx->output.size() >= g_Settings.maxResultLength)
				return;

			size_t remaining = g_Settings.maxResultLength - ctx->output.size();
			ctx->output.append(text.substr(0, remaining));
		}

		static void Finished(void* arg, bool success)
		{
			CommandContext* ctx = static_cast<CommandContext*>(arg);
			if (!ctx)
				return;

			if (ctx->output.empty())
				ctx->output = success ? "OK" : "Error";

			MarkInboxResult(ctx->id, success ? "ok" : "error", ctx->output);

			std::string payload = Acore::StringFormat(
				R"({"event":"command_result","command":{"id":{},"status":"{}","output":"{}"},"timestamp":{}})",
				ctx->id,
				success ? "ok" : "error",
				EscapeJson(ctx->output),
				GameTime::GetGameTime().count());
			EnqueueOutbox("command_result", payload);

			delete ctx;
		}
	};

	static void QueueCommand(uint32 inboxId, uint64 discordUserId, uint32 accountId, std::string const& command)
	{
		CommandContext* ctx = new CommandContext(inboxId, discordUserId, accountId);
		CliCommandHolder* cmd = new CliCommandHolder(ctx, command.c_str(), &CommandContext::Print, &CommandContext::Finished);
		sWorld->QueueCliCommand(cmd);
	}

	static bool GetLinkedAccount(uint64 discordUserId, uint32& accountId, bool& verified)
	{
		QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
			"SELECT account_id, verified FROM gm_discord_link WHERE discord_user_id={} LIMIT 1",
			discordUserId));

		if (!result)
			return false;

		Field* fields = result->Fetch();
		accountId = fields[0].Get<uint32>();
		verified = fields[1].Get<uint8>() != 0;
		return true;
	}

	static void ProcessInbox()
	{
		if (!g_Settings.enabled)
			return;

		QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
			"SELECT id, discord_user_id, action, payload FROM gm_discord_inbox WHERE processed=0 ORDER BY id ASC LIMIT {}",
			g_Settings.maxBatchSize));

		if (!result)
			return;

		do
		{
			Field* fields = result->Fetch();
			uint32 id = fields[0].Get<uint32>();
			uint64 discordUserId = fields[1].Get<uint64>();
			std::string action = ToLower(fields[2].Get<std::string>());
			std::string payload = fields[3].Get<std::string>();

			std::string rateReason;
			if (!CheckRateLimit(discordUserId, action, rateReason))
			{
				MarkInboxResult(id, "rate_limited", rateReason);
				LogAudit(discordUserId, 0, action, action, "rate_limited", rateReason, payload);
				continue;
			}

			if (action == "command")
			{
				uint32 accountId = 0;
				bool verified = false;
				if (!GetLinkedAccount(discordUserId, accountId, verified))
				{
					MarkInboxResult(id, "not_linked", "Discord user is not linked to a GM account");
					LogAudit(discordUserId, accountId, action, "command", "not_linked", "Discord user is not linked", payload);
					continue;
				}

				if (!verified)
				{
					MarkInboxResult(id, "not_verified", "Discord user is not verified");
					LogAudit(discordUserId, accountId, action, "command", "not_verified", "Discord user is not verified", payload);
					continue;
				}

				std::string category;
				std::string reason;
				if (!CheckCommandPermissions(payload, accountId, category, reason))
				{
					MarkInboxResult(id, "forbidden", reason);
					LogAudit(discordUserId, accountId, action, category.empty() ? "command" : category, "forbidden", reason, payload);
					continue;
				}

				MarkInboxProcessing(id);
				QueueCommand(id, discordUserId, accountId, payload);
				LogAudit(discordUserId, accountId, action, category, "queued", "Command queued", payload);
			}
			else if (action == "auth")
			{
				if (payload.empty())
				{
					MarkInboxResult(id, "invalid", "Missing secret payload");
					LogAudit(discordUserId, 0, action, "auth", "invalid", "Missing secret payload", payload);
					continue;
				}

				uint32 linkedAccountId = 0;
				if (!VerifyAndLinkSecret(discordUserId, payload, linkedAccountId))
				{
					MarkInboxResult(id, "invalid", "Secret not found or expired");
					LogAudit(discordUserId, 0, action, "auth", "invalid", "Secret not found or expired", payload);
					continue;
				}

				MarkInboxResult(id, "ok", "Discord user linked successfully");
				LogAudit(discordUserId, linkedAccountId, action, "auth", "ok", "Discord user linked successfully", payload);
			}
			else if (action == "whisper")
			{
				if (!g_Settings.whisperEnabled)
				{
					MarkInboxResult(id, "disabled", "Whisper relay disabled");
					LogAudit(discordUserId, 0, action, "whisper", "disabled", "Whisper relay disabled", payload);
					continue;
				}

				uint32 accountId = 0;
				bool verified = false;
				if (!GetLinkedAccount(discordUserId, accountId, verified) || !verified)
				{
					MarkInboxResult(id, "not_verified", "Discord user is not verified");
					LogAudit(discordUserId, accountId, action, "whisper", "not_verified", "Discord user is not verified", payload);
					continue;
				}

				uint32 security = AccountMgr::GetSecurity(accountId);
				uint32 required = std::max(g_Settings.minSecurity, GetCategoryMinSecurity("whisper"));
				if (security < required)
				{
					MarkInboxResult(id, "forbidden", "Account security is too low");
					LogAudit(discordUserId, accountId, action, "whisper", "forbidden", "Account security is too low", payload);
					continue;
				}

				std::string playerName;
				std::string gmName;
				std::string message;
				if (!ParseWhisperPayload(payload, playerName, gmName, message))
				{
					MarkInboxResult(id, "invalid", "Invalid whisper payload");
					LogAudit(discordUserId, accountId, action, "whisper", "invalid", "Invalid whisper payload", payload);
					continue;
				}

				Player* player = ObjectAccessor::FindPlayerByName(playerName, false);
				if (!player)
				{
					MarkInboxResult(id, "player_offline", "Player is offline");
					LogAudit(discordUserId, accountId, action, "whisper", "player_offline", "Player is offline", payload);
					continue;
				}

				SendWhisperToPlayer(player, gmName, message);
				UpsertWhisperSession(player, discordUserId, gmName);
				MarkInboxResult(id, "ok", "Whisper delivered");
				LogAudit(discordUserId, accountId, action, "whisper", "ok", "Whisper delivered", payload);

				uint32 ticketId = 0;
				if (GmTicket* ticket = sTicketMgr->GetTicketByPlayer(player->GetGUID()))
					ticketId = ticket->GetId();

				std::string outPayload = Acore::StringFormat(
					R"({"event":"gm_whisper","whisper":{"player":"{}","playerGuid":{},"gmName":"{}","discordUserId":{},"ticketId":{},"message":"{}"},"timestamp":{}})",
					EscapeJson(player->GetName()),
					player->GetGUID().GetRawValue(),
					EscapeJson(gmName),
					discordUserId,
					ticketId,
					EscapeJson(message),
					GameTime::GetGameTime().count());
				EnqueueOutbox("gm_whisper", outPayload);
			}
			else if (action == "ticket_assign")
			{
				uint32 accountId = 0;
				bool verified = false;
				if (!GetLinkedAccount(discordUserId, accountId, verified) || !verified)
				{
					MarkInboxResult(id, "not_verified", "Discord user is not verified");
					LogAudit(discordUserId, accountId, action, "ticket", "not_verified", "Discord user is not verified", payload);
					continue;
				}

				std::string category;
				std::string reason;
				if (!CheckCommandPermissions(".ticket assign", accountId, category, reason))
				{
					MarkInboxResult(id, "forbidden", reason);
					LogAudit(discordUserId, accountId, action, "ticket", "forbidden", reason, payload);
					continue;
				}

				uint32 ticketId = 0;
				std::string gmName;
				if (!ParseTicketAssignPayload(payload, ticketId, gmName))
				{
					MarkInboxResult(id, "invalid", "Invalid ticket assignment payload");
					LogAudit(discordUserId, accountId, action, "ticket", "invalid", "Invalid ticket assignment payload", payload);
					continue;
				}

				std::string command = Acore::StringFormat(".ticket assign {} {}", ticketId, gmName);
				MarkInboxProcessing(id);
				QueueCommand(id, discordUserId, accountId, command);
				LogAudit(discordUserId, accountId, action, "ticket", "queued", "Ticket assignment queued", payload);
			}
			else
			{
				MarkInboxResult(id, "invalid", "Unknown action");
				LogAudit(discordUserId, 0, action, action, "invalid", "Unknown action", payload);
			}
		} while (result->NextRow());
	}

	static std::string BuildTicketPayload(GmTicket* ticket, std::string_view eventName)
	{
		if (!ticket)
			return "{}";

		std::string status = ticket->IsClosed() ? "closed" : (ticket->IsCompleted() ? "completed" : "open");
		std::string assignedTo = ticket->GetAssignedToName();

		return Acore::StringFormat(
			R"({"event":"{}","ticket":{"id":{},"player":"{}","message":"{}","comment":"{}","response":"{}","assignedTo":"{}","assignedToGuid":{},"status":"{}","escalationStatus":{},"viewed":{},"needResponse":{},"needMoreHelp":{},"createTime":{},"lastModified":{},"closedByGuid":{},"resolvedByGuid":{},"location":{"mapId":{},"x":{},"y":{},"z":{}}}})",
			eventName,
			ticket->GetId(),
			EscapeJson(ticket->GetPlayerName()),
			EscapeJson(ticket->GetMessage()),
			EscapeJson(ticket->GetComment()),
			EscapeJson(ticket->GetResponseText()),
			EscapeJson(assignedTo),
			ticket->GetAssignedToGUID().GetRawValue(),
			status,
			static_cast<uint32>(ticket->GetEscalatedStatus()),
			ticket->IsViewed() ? 1 : 0,
			ticket->NeedResponse() ? 1 : 0,
			ticket->NeedMoreHelp() ? 1 : 0,
			ticket->GetCreateTime(),
			ticket->GetLastModifiedTime(),
			ticket->GetClosedByGUID().GetRawValue(),
			ticket->GetResolvedByGUID().GetRawValue(),
			ticket->GetMapId(),
			ticket->GetPositionX(),
			ticket->GetPositionY(),
			ticket->GetPositionZ());
	}
}

class GMDiscordTicketScript : public TicketScript
{
public:
	GMDiscordTicketScript() : TicketScript("GMDiscordTicketScript") { }

	void OnTicketCreate(GmTicket* ticket) override
	{
		GMDiscord::EnqueueOutbox("ticket_create", GMDiscord::BuildTicketPayload(ticket, "ticket_create"));
	}

	void OnTicketUpdateLastChange(GmTicket* ticket) override
	{
		GMDiscord::EnqueueOutbox("ticket_update", GMDiscord::BuildTicketPayload(ticket, "ticket_update"));
	}

	void OnTicketClose(GmTicket* ticket) override
	{
		GMDiscord::EnqueueOutbox("ticket_close", GMDiscord::BuildTicketPayload(ticket, "ticket_close"));
	}

	void OnTicketStatusUpdate(GmTicket* ticket) override
	{
		GMDiscord::EnqueueOutbox("ticket_status", GMDiscord::BuildTicketPayload(ticket, "ticket_status"));
	}

	void OnTicketResolve(GmTicket* ticket) override
	{
		GMDiscord::EnqueueOutbox("ticket_resolve", GMDiscord::BuildTicketPayload(ticket, "ticket_resolve"));
	}
};

class GMDiscordWorldScript : public WorldScript
{
public:
	GMDiscordWorldScript() : WorldScript("GMDiscordWorldScript") { }

	void OnAfterConfigLoad(bool /*reload*/) override
	{
		GMDiscord::LoadSettings();
		GMDiscord::DiscordBot::Instance().LoadConfig();
	}

	void OnStartup() override
	{
		GMDiscord::DiscordBot::Instance().Start();
	}

	void OnShutdown() override
	{
		GMDiscord::DiscordBot::Instance().Stop();
	}

	void OnUpdate(uint32 diff) override
	{
		if (!GMDiscord::g_Settings.enabled)
			return;

		if (_timer <= diff)
		{
			_timer = GMDiscord::g_Settings.pollIntervalMs;
			GMDiscord::ProcessInbox();
		}
		else
		{
			_timer -= diff;
		}
	}

private:
	uint32 _timer = 0;
};

class GMDiscordCommandScript : public CommandScript
{
public:
	GMDiscordCommandScript() : CommandScript("gm_discord_commandscript") { }

	ChatCommandTable GetCommands() const override
	{
		static ChatCommandTable gmDiscordSubTable =
		{
			{ "link",   HandleLinkCommand,   SEC_GAMEMASTER, Console::No },
			{ "status", HandleStatusCommand, SEC_GAMEMASTER, Console::No },
			{ "unlink", HandleUnlinkCommand, SEC_GAMEMASTER, Console::No },
		};

		static ChatCommandTable commandTable =
		{
			{ "discord", gmDiscordSubTable },
			{ "gmdiscord", gmDiscordSubTable }
		};

		return commandTable;
	}

	static bool HandleLinkCommand(ChatHandler* handler, std::string secret)
	{
		WorldSession* session = handler->GetSession();
		if (!session)
		{
			handler->SendErrorMessage("This command can only be used in-game.");
			return false;
		}

		secret = GMDiscord::Trim(secret);
		if (secret.size() < 8)
		{
			handler->SendErrorMessage("Secret must be at least 8 characters.");
			return false;
		}

		BigNumber salt;
		salt.SetRand(128);
		Optional<std::string> hash = Acore::Crypto::Argon2::Hash(secret, salt);
		if (!hash)
		{
			handler->SendErrorMessage("Failed to hash secret.");
			return false;
		}

		uint32 accountId = session->GetAccountId();
		std::string hashEsc = GMDiscord::EscapeSql(*hash);
		std::string gmNameEsc = GMDiscord::EscapeSql(session->GetPlayer()->GetName());
		CharacterDatabase.Execute(Acore::StringFormat(
			"INSERT INTO gm_discord_link (account_id, discord_user_id, verified, secret_hash, secret_expires_at, gm_name) "
			"VALUES ({}, NULL, 0, '{}', DATE_ADD(NOW(), INTERVAL {} SECOND), '{}') "
			"ON DUPLICATE KEY UPDATE discord_user_id=NULL, verified=0, secret_hash='{}', secret_expires_at=DATE_ADD(NOW(), INTERVAL {} SECOND), gm_name='{}', updated_at=NOW()",
			accountId,
			hashEsc,
			GMDiscord::g_Settings.secretTtlSeconds,
			gmNameEsc,
			hashEsc,
			GMDiscord::g_Settings.secretTtlSeconds,
			gmNameEsc));

		handler->PSendSysMessage("Discord link secret set. It expires in {} minutes.", GMDiscord::g_Settings.secretTtlSeconds / 60);
		return true;
	}

	static bool HandleStatusCommand(ChatHandler* handler)
	{
		WorldSession* session = handler->GetSession();
		if (!session)
		{
			handler->SendErrorMessage("This command can only be used in-game.");
			return false;
		}

		uint32 accountId = session->GetAccountId();
		QueryResult result = CharacterDatabase.Query(Acore::StringFormat(
			"SELECT discord_user_id, verified, secret_expires_at FROM gm_discord_link WHERE account_id={} LIMIT 1",
			accountId));

		if (!result)
		{
			handler->SendSysMessage("No Discord link found for this account.");
			return true;
		}

		Field* fields = result->Fetch();
		uint64 discordId = fields[0].Get<uint64>();
		bool verified = fields[1].Get<uint8>() != 0;
		bool hasSecret = !fields[2].IsNull();

		handler->PSendSysMessage("Discord link status: {} (Discord ID: {}, Secret: {})",
			verified ? "verified" : "pending",
			discordId ? std::to_string(discordId) : "none",
			hasSecret ? "set" : "not set");
		return true;
	}

	static bool HandleUnlinkCommand(ChatHandler* handler)
	{
		WorldSession* session = handler->GetSession();
		if (!session)
		{
			handler->SendErrorMessage("This command can only be used in-game.");
			return false;
		}

		uint32 accountId = session->GetAccountId();
		CharacterDatabase.Execute(Acore::StringFormat(
			"DELETE FROM gm_discord_link WHERE account_id={} LIMIT 1",
			accountId));

		handler->SendSysMessage("Discord link removed.");
		return true;
	}
};

class GMDiscordPlayerScript : public PlayerScript
{
public:
	GMDiscordPlayerScript() : PlayerScript("GMDiscordPlayerScript") { }

	bool OnPlayerWhisper(Player* player, uint32 type, uint32 language, std::string& msg, std::string const& receiverName, Player* receiver) override
	{
		if (!GMDiscord::g_Settings.enabled || !GMDiscord::g_Settings.whisperEnabled)
			return true;

		if (!player || type != CHAT_MSG_WHISPER)
			return true;

		if (receiver)
			return true;

		uint64 discordUserId = 0;
		if (!GMDiscord::TryGetWhisperSession(receiverName, discordUserId))
			return true;

		uint32 ticketId = 0;
		if (GmTicket* ticket = sTicketMgr->GetTicketByPlayer(player->GetGUID()))
			ticketId = ticket->GetId();

		std::string payload = Acore::StringFormat(
			R"({"event":"player_whisper","whisper":{"player":"{}","playerGuid":{},"gmName":"{}","discordUserId":{},"ticketId":{},"message":"{}"},"timestamp":{}})",
			GMDiscord::EscapeJson(player->GetName()),
			player->GetGUID().GetRawValue(),
			GMDiscord::EscapeJson(receiverName),
			discordUserId,
			ticketId,
			GMDiscord::EscapeJson(msg),
			GameTime::GetGameTime().count());
		GMDiscord::EnqueueOutbox("player_whisper", payload);

		return false; // handled, prevent "player not found"
	}
};

void AddSC_gm_discord()
{
	GMDiscord::LoadSettings();
	new GMDiscordTicketScript();
	new GMDiscordWorldScript();
	new GMDiscordCommandScript();
	new GMDiscordPlayerScript();
}
