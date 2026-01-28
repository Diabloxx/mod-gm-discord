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
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Random.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TicketMgr.h"
#include "World.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

using namespace Acore::ChatCommands;

namespace GMDiscord
{
	struct Settings
	{
		bool enabled = true;
		bool outboxEnabled = true;
		bool allowAllCommands = false;
		uint32 pollIntervalMs = 1000;
		uint32 maxBatchSize = 25;
		uint32 minSecurity = SEC_GAMEMASTER;
		uint32 linkCodeTtlSeconds = 900;
		uint32 maxResultLength = 4000;
		std::vector<std::string> commandAllowList;
	};

	static Settings g_Settings;

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
		g_Settings.allowAllCommands = sConfigMgr->GetOption<bool>("GMDiscord.CommandAllowAll", false);
		g_Settings.pollIntervalMs = sConfigMgr->GetOption<uint32>("GMDiscord.PollIntervalMs", 1000);
		g_Settings.maxBatchSize = sConfigMgr->GetOption<uint32>("GMDiscord.MaxBatchSize", 25);
		g_Settings.minSecurity = sConfigMgr->GetOption<uint32>("GMDiscord.MinSecurityLevel", SEC_GAMEMASTER);
		g_Settings.linkCodeTtlSeconds = sConfigMgr->GetOption<uint32>("GMDiscord.LinkCodeTtlSeconds", 900);
		g_Settings.maxResultLength = sConfigMgr->GetOption<uint32>("GMDiscord.MaxResultLength", 4000);

		std::string allowList = sConfigMgr->GetOption<std::string>("GMDiscord.CommandAllowList", ".ticket;.gm");
		g_Settings.commandAllowList = SplitAllowList(allowList);
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
				R"({"event":"command_result","id":{},"status":"{}","output":"{}"})",
				ctx->id,
				success ? "ok" : "error",
				EscapeJson(ctx->output));
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
			"SELECT id, discord_user_id, command FROM gm_discord_inbox WHERE processed=0 ORDER BY id ASC LIMIT {}",
			g_Settings.maxBatchSize));

		if (!result)
			return;

		do
		{
			Field* fields = result->Fetch();
			uint32 id = fields[0].Get<uint32>();
			uint64 discordUserId = fields[1].Get<uint64>();
			std::string command = fields[2].Get<std::string>();

			if (!IsCommandAllowed(command))
			{
				MarkInboxResult(id, "forbidden", "Command not allowed by GMDiscord.CommandAllowList");
				continue;
			}

			uint32 accountId = 0;
			bool verified = false;
			if (!GetLinkedAccount(discordUserId, accountId, verified))
			{
				MarkInboxResult(id, "not_linked", "Discord user is not linked to a GM account");
				continue;
			}

			if (!verified)
			{
				MarkInboxResult(id, "not_verified", "Discord user is not verified");
				continue;
			}

			uint32 security = AccountMgr::GetSecurity(accountId);
			if (security < g_Settings.minSecurity)
			{
				MarkInboxResult(id, "forbidden", "Account security is too low");
				continue;
			}

			MarkInboxProcessing(id);
			QueueCommand(id, discordUserId, accountId, command);
		} while (result->NextRow());
	}

	static std::string BuildTicketPayload(GmTicket* ticket, std::string_view eventName)
	{
		if (!ticket)
			return "{}";

		std::string status = ticket->IsClosed() ? "closed" : (ticket->IsCompleted() ? "completed" : "open");
		std::string assignedTo = ticket->GetAssignedToName();
		std::string message = ticket->GetMessage();

		return Acore::StringFormat(
			R"({"event":"{}","ticketId":{},"player":"{}","message":"{}","assignedTo":"{}","status":"{}","lastModified":{}})",
			eventName,
			ticket->GetId(),
			EscapeJson(ticket->GetPlayerName()),
			EscapeJson(message),
			EscapeJson(assignedTo),
			status,
			ticket->GetLastModifiedTime());
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
			{ "gmdiscord", gmDiscordSubTable }
		};

		return commandTable;
	}

	static bool HandleLinkCommand(ChatHandler* handler)
	{
		WorldSession* session = handler->GetSession();
		if (!session)
		{
			handler->SendErrorMessage("This command can only be used in-game.");
			return false;
		}

		uint32 accountId = session->GetAccountId();
		uint32 code = urand(100000, 999999);

		CharacterDatabase.Execute(Acore::StringFormat(
			"INSERT INTO gm_discord_link (account_id, discord_user_id, verified, link_code, link_code_expires) "
			"VALUES ({}, NULL, 0, '{}', DATE_ADD(NOW(), INTERVAL {} SECOND)) "
			"ON DUPLICATE KEY UPDATE discord_user_id=NULL, verified=0, link_code='{}', link_code_expires=DATE_ADD(NOW(), INTERVAL {} SECOND)",
			accountId,
			code,
			GMDiscord::g_Settings.linkCodeTtlSeconds,
			code,
			GMDiscord::g_Settings.linkCodeTtlSeconds));

		handler->PSendSysMessage("Your GM Discord link code is: {} (valid for {} minutes)",
			code, GMDiscord::g_Settings.linkCodeTtlSeconds / 60);
		handler->PSendSysMessage("Use this code in Discord to link your account.");
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
			"SELECT discord_user_id, verified FROM gm_discord_link WHERE account_id={} LIMIT 1",
			accountId));

		if (!result)
		{
			handler->SendSysMessage("No Discord link found for this account.");
			return true;
		}

		Field* fields = result->Fetch();
		uint64 discordId = fields[0].Get<uint64>();
		bool verified = fields[1].Get<uint8>() != 0;

		handler->PSendSysMessage("Discord link status: {} (Discord ID: {})", verified ? "verified" : "pending", discordId ? std::to_string(discordId) : "none");
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

void AddSC_gm_discord()
{
	GMDiscord::LoadSettings();
	new GMDiscordTicketScript();
	new GMDiscordWorldScript();
	new GMDiscordCommandScript();
}
