/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <atomic>
#include <mutex>
#include <string>

namespace censr {

/* Process-wide license state, shared by the OBS plugin, the standalone host, and
 * the virtual cameras (all link censr-core). libobs-free: it logs via censr_log,
 * stores its state in a plain file under a config dir the host provides, and does
 * its own minimal JSON parsing of Lemon Squeezy responses.
 *
 * Backed by Lemon Squeezy's license API (no API key needed — the license key
 * authenticates itself). Activation is locked to the Censr product, so a key from
 * another store/product can't unlock it. Redaction ALWAYS runs; a missing/invalid
 * license only adds an on-stream watermark. Validity is cached with an offline
 * grace window so a transient network drop doesn't nag a paying user. */
class License {
public:
	static License &instance();

	/* Where the license file lives (host-provided). Reads any stored license
	 * and kicks off a background re-validation. Call once at startup. */
	void load(const std::string &config_dir);

	/* Cheap, lock-free; safe from the render thread. */
	bool active() const { return active_.load(); }

	std::string key();
	std::string status_text(); /* one line for a settings panel */

	/* Activate a key (blocking network call — never on the render thread).
	 * Returns true if the key is now active. Uses the dir from load(). */
	bool activate(const std::string &key);

	/* Release this machine's activation and clear local state. */
	void deactivate();

private:
	License() = default;
	void persist_locked();
	std::string state_path();

	std::atomic<bool> active_{false};
	std::mutex mu_;
	std::string config_dir_;
	std::string key_;
	std::string instance_id_;
	std::string status_ = "Unlicensed \xE2\x80\x94 a watermark shows on stream.";
	long long last_valid_epoch_ = 0;
};

inline License &license() { return License::instance(); }

} // namespace censr
