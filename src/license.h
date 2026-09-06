/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace censr {

/* Process-wide license state for the plugin (one license, not per filter).
 *
 * Backed by Lemon Squeezy's license API (no API key needed — the license key
 * authenticates itself). A key is ACTIVATED once against this machine (creating
 * an instance), then re-VALIDATED periodically. Redaction ALWAYS runs; a missing
 * or invalid license only adds an on-stream watermark. We never leak a secret to
 * push a sale. Validity is cached with an offline grace period so a transient
 * network drop doesn't nag a paying user. */
class License {
public:
	static License &instance();

	/* True when a valid license is active (within the offline grace window).
	 * Cheap + lock-free; safe to call from the render thread. */
	bool active() const { return active_.load(); }

	std::string key();
	std::string status_text(); /* one line for the properties panel */

	/* Load the stored license on plugin start and kick off a background
	 * re-validation. */
	void load();

	/* Activate a key (blocking network call — never on the render thread).
	 * Returns true if the key is now active. */
	bool activate(const std::string &key);

	/* Release this machine's activation and clear local state. */
	void deactivate();

private:
	License() = default;
	void persist_locked();
	std::string config_path();

	std::atomic<bool> active_{false};
	std::mutex mu_;
	std::string key_;
	std::string instance_id_;
	std::string status_ = "Unlicensed \xE2\x80\x94 a watermark shows on stream.";
	long long last_valid_epoch_ = 0;
};

inline License &license() { return License::instance(); }

} // namespace censr
