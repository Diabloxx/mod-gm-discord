-- GM Discord module tables

DROP TABLE IF EXISTS `gm_discord_link`;
CREATE TABLE `gm_discord_link` (
  `account_id` INT UNSIGNED NOT NULL,
  `discord_user_id` BIGINT UNSIGNED NULL,
  `verified` TINYINT NOT NULL DEFAULT 0,
  `secret_hash` VARCHAR(128) NULL,
  `secret_expires_at` DATETIME NULL,
  `gm_name` VARCHAR(24) NULL,
  `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`account_id`),
  UNIQUE KEY `uniq_discord_user_id` (`discord_user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DROP TABLE IF EXISTS `gm_discord_inbox`;
CREATE TABLE `gm_discord_inbox` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `discord_user_id` BIGINT UNSIGNED NOT NULL,
  `action` VARCHAR(16) NOT NULL,
  `payload` TEXT NOT NULL,
  `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `processed` TINYINT NOT NULL DEFAULT 0,
  `processed_at` DATETIME NULL,
  `status` VARCHAR(16) NULL,
  `result` MEDIUMTEXT NULL,
  PRIMARY KEY (`id`),
  KEY `idx_processed` (`processed`),
  KEY `idx_discord_user_id` (`discord_user_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DROP TABLE IF EXISTS `gm_discord_outbox`;
CREATE TABLE `gm_discord_outbox` (
  `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
  `event_type` VARCHAR(32) NOT NULL,
  `payload` MEDIUMTEXT NOT NULL,
  `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  `dispatched` TINYINT NOT NULL DEFAULT 0,
  `dispatched_at` DATETIME NULL,
  PRIMARY KEY (`id`),
  KEY `idx_dispatched` (`dispatched`),
  KEY `idx_event_type` (`event_type`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

DROP TABLE IF EXISTS `gm_discord_whisper_session`;
CREATE TABLE `gm_discord_whisper_session` (
  `player_guid` BIGINT UNSIGNED NOT NULL,
  `discord_user_id` BIGINT UNSIGNED NOT NULL,
  `gm_name` VARCHAR(24) NOT NULL,
  `updated_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`player_guid`),
  KEY `idx_discord_user_id` (`discord_user_id`),
  KEY `idx_gm_name` (`gm_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
