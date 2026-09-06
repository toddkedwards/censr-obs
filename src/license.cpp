/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "license.h"
#include "http.h"
#include "plugin-support.h"

#include <obs-module.h>
#include <util/platform.h>

#include <ctime>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace censr {

namespace {

constexpr const char *LS_ACTIVATE = "https://api.lemonsqueezy.com/v1/licenses/activate";
constexpr const char *LS_VALIDATE = "https://api.lemonsqueezy.com/v1/licenses/validate";
constexpr const char *LS_DEACTIVATE = "https://api.lemonsqueezy.com/v1/licenses/deactivate";
constexpr long long GRACE_SECONDS = 14ll * 24 * 3600; /* offline grace */

std::string urlencode(const std::string &s)
{
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out += (char)c;
		else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 0xF];
		}
	}
	return out;
}

std::string machine_name()
{
	char buf[256] = {0};
#ifdef _WIN32
	DWORD n = sizeof(buf);
	if (GetComputerNameA(buf, &n) && buf[0])
		return buf;
#else
	if (gethostname(buf, sizeof(buf) - 1) == 0 && buf[0])
		return buf;
#endif
	return "Censr for OBS";
}

/* Extract a nested string, e.g. obj "instance" -> field "id". */
std::string get_nested(obs_data_t *d, const char *obj, const char *field)
{
	obs_data_t *o = obs_data_get_obj(d, obj);
	std::string v;
	if (o) {
		const char *s = obs_data_get_string(o, field);
		if (s)
			v = s;
		obs_data_release(o);
	}
	return v;
}

} // namespace

License &License::instance()
{
	static License inst;
	return inst;
}

std::string License::config_path()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *p = obs_module_config_path("license.json");
	std::string path = p ? p : "";
	if (p)
		bfree(p);
	return path;
}

void License::persist_locked()
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "license_key", key_.c_str());
	obs_data_set_string(d, "instance_id", instance_id_.c_str());
	obs_data_set_int(d, "last_valid", (long long)last_valid_epoch_);
	std::string path = config_path();
	if (!path.empty())
		obs_data_save_json_safe(d, path.c_str(), "tmp", "bak");
	obs_data_release(d);
}

std::string License::key()
{
	std::lock_guard<std::mutex> lk(mu_);
	return key_;
}

std::string License::status_text()
{
	std::lock_guard<std::mutex> lk(mu_);
	return status_;
}

void License::load()
{
	std::string path = config_path();
	std::string key, inst;
	long long last = 0;
	if (!path.empty()) {
		obs_data_t *d = obs_data_create_from_json_file(path.c_str());
		if (d) {
			const char *k = obs_data_get_string(d, "license_key");
			const char *i = obs_data_get_string(d, "instance_id");
			key = k ? k : "";
			inst = i ? i : "";
			last = obs_data_get_int(d, "last_valid");
			obs_data_release(d);
		}
	}
	if (key.empty())
		return;

	{
		std::lock_guard<std::mutex> lk(mu_);
		key_ = key;
		instance_id_ = inst;
		last_valid_epoch_ = last;
		/* Trust the cached validity within the grace window while we
		 * re-check online in the background. */
		bool grace = (time(nullptr) - last) < GRACE_SECONDS;
		active_.store(grace);
		status_ = grace ? "Licensed \xE2\x9C\x93" : "License needs re-checking\xE2\x80\xA6";
	}

	/* Re-validate online without blocking plugin load. */
	std::thread([this] {
		std::string k, i;
		{
			std::lock_guard<std::mutex> lk(mu_);
			k = key_;
			i = instance_id_;
		}
		std::string body = "license_key=" + urlencode(k) +
				   "&instance_id=" + urlencode(i);
		long code = 0;
		std::string resp;
		if (!https_post(LS_VALIDATE, "application/x-www-form-urlencoded", body,
				code, resp))
			return; /* offline: keep the grace-based state */
		obs_data_t *d = obs_data_create_from_json(resp.c_str());
		bool valid = d && obs_data_get_bool(d, "valid");
		if (d)
			obs_data_release(d);
		std::lock_guard<std::mutex> lk(mu_);
		if (valid) {
			last_valid_epoch_ = (long long)time(nullptr);
			active_.store(true);
			status_ = "Licensed \xE2\x9C\x93";
			persist_locked();
		} else if (code == 400 || code == 404) {
			/* Definitive rejection (revoked / unknown) — drop it. */
			active_.store(false);
			status_ = "License invalid \xE2\x80\x94 a watermark shows on stream.";
		}
	}).detach();
}

bool License::activate(const std::string &keyIn)
{
	std::string key = keyIn;
	/* trim */
	while (!key.empty() && (key.front() == ' ' || key.front() == '\t'))
		key.erase(key.begin());
	while (!key.empty() && (key.back() == ' ' || key.back() == '\t' ||
				key.back() == '\n' || key.back() == '\r'))
		key.pop_back();
	if (key.empty()) {
		std::lock_guard<std::mutex> lk(mu_);
		status_ = "Paste your Censr license key first.";
		return false;
	}

	std::string body = "license_key=" + urlencode(key) +
			   "&instance_name=" + urlencode(machine_name());
	long code = 0;
	std::string resp;
	if (!https_post(LS_ACTIVATE, "application/x-www-form-urlencoded", body, code,
			resp)) {
		std::lock_guard<std::mutex> lk(mu_);
		status_ = "Couldn't reach the license server \xE2\x80\x94 check your connection.";
		return false;
	}

	obs_data_t *d = obs_data_create_from_json(resp.c_str());
	bool activated = d && obs_data_get_bool(d, "activated");
	std::string inst = d ? get_nested(d, "instance", "id") : "";
	std::string err = d ? (obs_data_get_string(d, "error") ? obs_data_get_string(d, "error") : "") : "";
	if (d)
		obs_data_release(d);

	std::lock_guard<std::mutex> lk(mu_);
	if (activated && !inst.empty()) {
		key_ = key;
		instance_id_ = inst;
		last_valid_epoch_ = (long long)time(nullptr);
		active_.store(true);
		status_ = "Licensed \xE2\x9C\x93 \xE2\x80\x94 thank you!";
		persist_locked();
		obs_log(LOG_INFO, "Censr: license activated");
		return true;
	}
	active_.store(false);
	status_ = err.empty() ? "That license key wasn't accepted."
			      : ("License: " + err);
	obs_log(LOG_WARNING, "Censr: license activation failed (%ld): %s", code,
		err.c_str());
	return false;
}

void License::deactivate()
{
	std::string k, i;
	{
		std::lock_guard<std::mutex> lk(mu_);
		k = key_;
		i = instance_id_;
	}
	if (!k.empty() && !i.empty()) {
		std::string body = "license_key=" + urlencode(k) +
				   "&instance_id=" + urlencode(i);
		long code = 0;
		std::string resp;
		https_post(LS_DEACTIVATE, "application/x-www-form-urlencoded", body,
			   code, resp);
	}
	std::lock_guard<std::mutex> lk(mu_);
	key_.clear();
	instance_id_.clear();
	last_valid_epoch_ = 0;
	active_.store(false);
	status_ = "Unlicensed \xE2\x80\x94 a watermark shows on stream.";
	persist_locked();
}

} // namespace censr
