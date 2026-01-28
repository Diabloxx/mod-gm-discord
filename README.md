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

## Database Tables (Characters DB)
- `gm_discord_link`
- `gm_discord_inbox`
- `gm_discord_outbox`
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
- `GMDiscord.CommandAllowAll`
- `GMDiscord.CommandAllowList`
- `GMDiscord.MinSecurityLevel`
- `GMDiscord.SecretTtlSeconds`
- `GMDiscord.Whisper.Enable`

## Message Flow
### Link Flow
1. GM runs `.discord link <secret>` in game.
2. Discord user runs `/gm-auth` with that secret.
3. Server verifies the hash, links Discord ID, clears secret.

### Ticket Flow
- Ticket events are pushed to `gm_discord_outbox` and posted by the bot.

### Whisper Flow
- `/gm-whisper` → inbox action `whisper` → server sends whisper.
- Player reply (whisper to GM name) → outbox `player_whisper`.

## Todo
### High Priority
- [ ] Add structured JSON payloads for all outbox events (ticket fields, timestamps, status).
- [ ] Add permission checks per command category (tickets, teleports, etc.).
- [ ] Add audit log table for all Discord actions.
- [ ] Add rate limiting and spam protection for Discord commands.

### Medium Priority
- [ ] Implement Discord channel ticket rooms (auto create & archive).
- [ ] Add ticket assignment from Discord.
- [ ] Add Discord embeds for tickets and GM replies.
- [ ] Add configurable command groups / role mappings.

### Long Term
- [ ] Full GM panel in Discord (interaction‑driven, not text commands).
- [ ] Fine‑grained permissions per Discord role and GM account level.
- [ ] Rich player context (location, class, level) in ticket views.
- [ ] Multi‑realm support & sharding.

## Notes
- The bot runs inside `worldserver`, so a crash or deadlock in the bot affects the server. Keep handlers lightweight and asynchronous.
- Slash commands are the current interface; message‑based commands are intentionally avoided.

## Status
Active development. The module is functional but still evolving.
