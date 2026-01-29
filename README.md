# mod-gm-discord

## Overview
mod-gm-discord is an AzerothCore module that runs a Discord bot inside `worldserver` and mirrors core GM workflows to Discord. It lets authorized GMs work from Discord (phone/desktop) while the module enforces in‑game security rules and logs all actions.

## Goals
- Provide full GM capability via Discord with audited, secure access.
- Synchronize tickets and GM↔player conversations between Discord and the game.
- Keep the bot **in‑process** (no webhooks) and started with `worldserver`.
- Ensure a secure, explicit linking flow between Discord accounts and in‑game GM accounts.

## Current Features
- In‑process Discord bot (DPP) starts/stops with the server.
- Secure GM linking using in‑game secret + Discord `/gm-auth`.
- Command execution via Discord `/gm-command` with allowlist + GM security checks.
- Category-based permission checks per command group.
- Audit log table for all Discord actions.
- Rate limiting and spam protection for Discord actions.
- Ticket room automation (auto create & archive).
- Ticket assignment from Discord.
- Ticket/whisper embeds.
- Discord role-to-category mappings.
- Ticket events emitted to Discord via outbox queue.
- Whisper relay:
  - Discord → player: `/gm-whisper` sends a whisper as the GM name.
  - Player → Discord: replies are captured and pushed to the outbox.

## Architecture (High Level)
- **Discord Bot** (DPP, in worldserver process)
  - Slash commands enqueue actions into `gm_discord_inbox`.
  - Outbox polling posts ticket events and player replies.
- **Module Core**
  - Polls `gm_discord_inbox` and executes actions.
  - Hooks `TicketScript` to publish ticket changes.
  - Hooks player whispers to capture replies for Discord.
- **Database Queue**
  - Inbox: commands/auth/whisper requests from Discord.
  - Outbox: ticket updates and player replies to Discord.

## Security Model
- A GM must generate a **secret** in game: `.discord link <secret>`.
- Discord user calls `/gm-auth` with the secret.
- Secrets are hashed (Argon2) and expire after configurable TTL.
- Only linked + verified Discord users can execute commands or whisper.
- GM security level (e.g., SEC_GAMEMASTER) is enforced server‑side.
- Command allowlist restricts which commands can be executed.

## Commands
### In‑Game
- `.discord link <secret>`
  - Sets/updates a short‑lived secret for Discord linking.
- `.discord status`
  - Shows link status and secret status.
- `.discord unlink`
  - Removes the Discord link.

### Discord (Slash Commands)
- `/gm-auth secret:<secret>`
- `/gm-command command:<.command text>`
- `/gm-whisper player:<name> message:<text>`
- `/gm-ticket-assign ticket_id:<id> gm_name:<name>`

## Database Tables (Characters DB)
- `gm_discord_link`
- `gm_discord_inbox`
- `gm_discord_outbox`
- `gm_discord_audit`
- `gm_discord_ticket_room`
- `gm_discord_whisper_session`

SQL is in:
- `modules/mod-gm-discord/data/sql/db-characters/gm_discord_tables.sql`

## Configuration
Main config file:
- `modules/mod-gm-discord/conf/GMToDiscord.conf.dist`

Important keys:
- `GMDiscord.Enable`
- `GMDiscord.Bot.Enable`
- `GMDiscord.Bot.Id`
- `GMDiscord.Bot.Token`
- `GMDiscord.Bot.GuildId`
- `GMDiscord.Bot.OutboxChannelId`
- `GMDiscord.Bot.TicketRooms.*`
- `GMDiscord.Bot.RoleMappings`
- `GMDiscord.CommandAllowAll`
- `GMDiscord.CommandAllowList`
- `GMDiscord.CommandCategory.*.MinSecurity`
- `GMDiscord.MinSecurityLevel`
- `GMDiscord.SecretTtlSeconds`
- `GMDiscord.Whisper.Enable`
- `GMDiscord.RateLimit.*`
- `GMDiscord.Audit.PayloadMax`

Role mapping format:
- `GMDiscord.Bot.RoleMappings = "roleId:ticket,tele;roleId2:whisper,ticket"`
- Categories: `ticket`, `tele`, `gm`, `ban`, `account`, `character`, `lookup`, `server`, `debug`, `whisper`, `misc`

## DPP (Discord) Setup

This module requires DPP (D++) and expects the headers/libraries to be available to the build.

### vcpkg (recommended)

1. Install vcpkg:
   ```powershell
   git clone https://github.com/microsoft/vcpkg.git
   cd vcpkg
   .\bootstrap-vcpkg.bat
   .\vcpkg integrate install
   ```
2. Install DPP:
   ```powershell
   .\vcpkg install dpp:x64-windows
   ```
3. Configure your build to use the vcpkg toolchain file (CMake builds):
   ```powershell
   -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>\scripts\buildsystems\vcpkg.cmake
   ```

## Message Flow
### Link Flow
1. GM runs `.discord link <secret>` in game.
2. Discord user runs `/gm-auth` with that secret.
3. Server verifies the hash, links Discord ID, clears secret.

### Ticket Flow
- Ticket events are pushed to `gm_discord_outbox` and posted by the bot.

## Outbox JSON Payloads
All outbox payloads are structured JSON with a top-level `event` and a `timestamp` (epoch ms). Event payloads are nested by domain for stability.

### Ticket Events
Events: `ticket_create`, `ticket_update`, `ticket_close`, `ticket_status`, `ticket_resolve`

Payload shape:

- `event`: string
- `ticket`:
  - `id`: number
  - `player`: string
  - `message`: string
  - `comment`: string
  - `response`: string
  - `assignedTo`: string
  - `assignedToGuid`: number
  - `status`: `open|closed|completed`
  - `escalationStatus`: number
  - `viewed`: 0|1
  - `needResponse`: 0|1
  - `needMoreHelp`: 0|1
  - `createTime`: number
  - `lastModified`: number
  - `closedByGuid`: number
  - `resolvedByGuid`: number
  - `location`:
    - `mapId`: number
    - `x`: number
    - `y`: number
    - `z`: number

### Command Results
Event: `command_result`

Payload shape:
- `event`: string
- `command`:
  - `id`: number
  - `status`: `ok|error`
  - `output`: string
- `timestamp`: number

### Player Whisper Replies
Event: `player_whisper`

Payload shape:
- `event`: string
- `whisper`:
  - `player`: string
  - `playerGuid`: number
  - `gmName`: string
  - `discordUserId`: number
  - `ticketId`: number
  - `message`: string
- `timestamp`: number

### GM Whisper Messages
Event: `gm_whisper`

Payload shape:
- `event`: string
- `whisper`:
  - `player`: string
  - `playerGuid`: number
  - `gmName`: string
  - `discordUserId`: number
  - `ticketId`: number
  - `message`: string
- `timestamp`: number

### Whisper Flow
- `/gm-whisper` → inbox action `whisper` → server sends whisper.
- Player reply (whisper to GM name) → outbox `player_whisper`.

## Todo
### High Priority
- [x] Add structured JSON payloads for all outbox events (ticket fields, timestamps, status).
- [x] Add permission checks per command category (tickets, teleports, etc.).
- [x] Add audit log table for all Discord actions.
- [x] Add rate limiting and spam protection for Discord commands.

### Medium Priority
- [x] Implement Discord channel ticket rooms (auto create & archive).
- [x] Add ticket assignment from Discord.
- [x] Add Discord embeds for tickets and GM replies.
- [x] Add configurable command groups / role mappings.

### Long Term
- [x] Full GM panel in Discord (interaction‑driven, not text commands).
- [ ] Fine‑grained permissions per Discord role and GM account level.
- [x] Rich player context (location, class, level) in ticket views.
- [ ] Multi‑realm support & sharding.

## Notes
- The bot runs inside `worldserver`, so a crash or deadlock in the bot affects the server. Keep handlers lightweight and asynchronous.
- Slash commands are the current interface; message‑based commands are intentionally avoided.

## Status
Active development. The module is functional but still evolving.
