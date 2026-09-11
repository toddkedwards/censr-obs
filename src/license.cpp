/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "license.h"
#include "http.h"
#include "censr_log.h"

#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
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

/* Lock activation to the Censr for OBS product so a key from another Lemon
 * Squeezy store/product can't unlock it. 0 disables the check. */
constexpr long long CENSR_LS_PRODUCT_ID = 1342934;
constexpr long long CENSR_LS_STORE_ID = 0; /* optional extra lock; 0 = off */

std::string urlencode(const std::string &s)
{
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
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

/* --- a tiny JSON reader for the flat + one-level-nested Lemon Squeezy shapes.
 * Not a general parser; it finds "key": and returns the string/number/bool. --- */

std::string json_raw(const std::string &s, const std::string &key)
{
	std::string pat = "\"" + key + "\"";
	size_t p = s.find(pat);
	if (p == std::string::npos)
		return "";
	p = s.find(':', p + pat.size());
	if (p == std::string::npos)
		return "";
	++p;
	while (p < s.size() && std::isspace((unsigned char)s[p]))
		++p;
	if (p >= s.size())
		return "";
	if (s[p] == '"') {
		std::string out;
		for (++p; p < s.size() && s[p] != '"'; ++p) {
			if (s[p] == '\\' && p + 1 < s.size()) {
				out += s[++p];
				continue;
			}
			out += s[p];
		}
		return out;
	}
	size_t start = p;
	while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']' &&
	       !std::isspace((unsigned char)s[p]))
		++p;
	return s.substr(start, p - start);
}

/* The object value {...} for key, so nested fields can be looked up in isolation. */
std::string json_object(const std::string &s, const std::string &key)
{
	std::string pat = "\"" + key + "\"";
	size_t p = s.find(pat);
	if (p == std::string::npos)
		return "";
	p = s.find('{', p);
	if (p == std::string::npos)
		return "";
	int depth = 0;
	size_t start = p;
	for (; p < s.size(); ++p) {
		if (s[p] == '{')
			++depth;
		else if (s[p] == '}' && --depth == 0)
			return s.substr(start, p - start + 1);
	}
	return "";
}

bool json_bool(const std::string &s, const std::string &key) { return json_raw(s, key) == "true"; }
long long json_int(const std::string &s, const std::string &key)
{
	std::string v = json_raw(s, key);
	return v.empty() ? 0 : atoll(v.c_str());
}

std::string trim(std::string v)
{
	while (!v.empty() && std::isspace((unsigned char)v.front()))
		v.erase(v.begin());
	while (!v.empty() && std::isspace((unsigned char)v.back()))
		v.pop_back();
	return v;
}

} // namespace

License &License::instance()
{
	static License inst;
	return inst;
}

std::string License::state_path()
{
	return config_dir_.empty() ? std::string("censr-license.dat")
				   : (config_dir_ + "/censr-license.dat");
}

void License::persist_locked()
{
	std::error_code ec;
	if (!config_dir_.empty())
		std::filesystem::create_directories(config_dir_, ec);
	std::ofstream f(state_path(), std::ios::trunc);
	if (!f)
		return;
	f << key_ << "\n" << instance_id_ << "\n" << last_valid_epoch_ << "\n";
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

void License::load(const std::string &config_dir)
{
	std::string key, inst;
	long long last = 0;
	{
		std::lock_guard<std::mutex> lk(mu_);
		config_dir_ = config_dir;
	}

	/* QA hook: CENSR_ACTIVATE_KEY forces an activation on load so the full
	 * round-trip can be verified headlessly. Dev/testing only. */
	if (const char *envkey = getenv("CENSR_ACTIVATE_KEY"); envkey && *envkey) {
		censr_log(LOG_INFO, "Censr: CENSR_ACTIVATE_KEY set — activating for QA");
		activate(envkey);
		return;
	}

	{
		std::ifstream f(state_path());
		if (f) {
			std::getline(f, key);
			std::getline(f, inst);
			std::string ln;
			if (std::getline(f, ln))
				last = ln.empty() ? 0 : atoll(ln.c_str());
		}
	}
	if (key.empty())
		return;

	{
		std::lock_guard<std::mutex> lk(mu_);
		key_ = key;
		instance_id_ = inst;
		last_valid_epoch_ = last;
		bool grace = (time(nullptr) - last) < GRACE_SECONDS;
		active_.store(grace);
		status_ = grace ? "Licensed \xE2\x9C\x93" : "License needs re-checking\xE2\x80\xA6";
	}

	/* Re-validate online without blocking startup. */
	std::thread([this] {
		std::string k, i;
		{
			std::lock_guard<std::mutex> lk(mu_);
			k = key_;
			i = instance_id_;
		}
		std::string body = "license_key=" + urlencode(k) + "&instance_id=" + urlencode(i);
		long code = 0;
		std::string resp;
		if (!https_post(LS_VALIDATE, "application/x-www-form-urlencoded", body, code, resp))
			return; /* offline: keep grace-based state */
		bool valid = json_bool(resp, "valid");
		std::lock_guard<std::mutex> lk(mu_);
		if (valid) {
			last_valid_epoch_ = (long long)time(nullptr);
			active_.store(true);
			status_ = "Licensed \xE2\x9C\x93";
			persist_locked();
		} else if (code == 400 || code == 404) {
			active_.store(false);
			status_ = "License invalid \xE2\x80\x94 a watermark shows on stream.";
		}
	}).detach();
}

bool License::activate(const std::string &keyIn)
{
	std::string key = trim(keyIn);
	if (key.empty()) {
		std::lock_guard<std::mutex> lk(mu_);
		status_ = "Paste your Censr license key first.";
		return false;
	}

	std::string body = "license_key=" + urlencode(key) +
			   "&instance_name=" + urlencode(machine_name());
	long code = 0;
	std::string resp;
	if (!https_post(LS_ACTIVATE, "application/x-www-form-urlencoded", body, code, resp)) {
		std::lock_guard<std::mutex> lk(mu_);
		status_ = "Couldn't reach the license server \xE2\x80\x94 check your connection.";
		return false;
	}

	bool activated = json_bool(resp, "activated");
	std::string inst = json_raw(json_object(resp, "instance"), "id");
	std::string err = json_raw(resp, "error");
	if (err == "null")
		err.clear();
	std::string meta = json_object(resp, "meta");
	long long product_id = json_int(meta, "product_id");
	long long store_id = json_int(meta, "store_id");

	bool wrong_product =
		(CENSR_LS_PRODUCT_ID != 0 && product_id != CENSR_LS_PRODUCT_ID) ||
		(CENSR_LS_STORE_ID != 0 && store_id != CENSR_LS_STORE_ID);

	/* LS activates (creating an instance) before we can check the product, so a
	 * wrong-product key would burn one of the user's activation slots. Release it. */
	if (activated && wrong_product && !inst.empty()) {
		std::string db = "license_key=" + urlencode(key) +
				 "&instance_id=" + urlencode(inst);
		long dc = 0;
		std::string dr;
		https_post(LS_DEACTIVATE, "application/x-www-form-urlencoded", db, dc, dr);
	}

	std::lock_guard<std::mutex> lk(mu_);
	if (activated && !inst.empty() && !wrong_product) {
		key_ = key;
		instance_id_ = inst;
		last_valid_epoch_ = (long long)time(nullptr);
		active_.store(true);
		status_ = "Licensed \xE2\x9C\x93 \xE2\x80\x94 thank you!";
		persist_locked();
		censr_log(LOG_INFO, "Censr: license activated");
		return true;
	}
	active_.store(false);
	if (activated && wrong_product)
		status_ = "That license key is for a different product.";
	else
		status_ = err.empty() ? "That license key wasn't accepted." : ("License: " + err);
	censr_log(LOG_WARNING, "Censr: license activation failed (%ld): %s", code, err.c_str());
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
		std::string body = "license_key=" + urlencode(k) + "&instance_id=" + urlencode(i);
		long code = 0;
		std::string resp;
		https_post(LS_DEACTIVATE, "application/x-www-form-urlencoded", body, code, resp);
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
