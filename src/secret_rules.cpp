/*
Censr - real-time on-screen redaction for OBS
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "secret_rules.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <string>
#include <utility>

namespace censr {

namespace {

/* Data-driven pattern rules. Kept as a small table so the full set can be
 * expanded later without touching logic. */
const std::regex &email_re()
{
	static const std::regex re(R"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})");
	return re;
}
const std::regex &aws_re()
{
	static const std::regex re(R"(AKIA[0-9A-Z]{16})");
	return re;
}
const std::regex &github_re()
{
	static const std::regex re(R"(gh[pousr]_[A-Za-z0-9]{20,})");
	return re;
}
const std::regex &sk_re()
{
	static const std::regex re(R"(sk-[A-Za-z0-9\-]{16,})");
	return re;
}
/* Secret-context label keyword (no trailing [:=] required, because OCR often
 * drops the "=value" part). To avoid matching prose like "nothing secret here",
 * a keyword only counts when the line ALSO looks like an assignment: it has a
 * ':'/'=' or an ALL-CAPS identifier (e.g. GITHUB_TOKEN, SUPPORT_EMAIL). */
const std::regex &label_re()
{
	/* Boundary is any non-letter (incl. '_' and digits), not \b, so keywords
	 * inside UPPER_SNAKE env-var names match (GITHUB_TOKEN, SUPPORT_EMAIL,
	 * AWS_SECRET_ACCESS_KEY) while substrings of real words do not
	 * (tokenizer, passwordless). The leading group is consumed — this regex
	 * is only used as a boolean gate, never for span offsets. */
	static const std::regex re(
		R"((?:^|[^A-Za-z])(pass(word|wd)?|pwd|secret|token|api[_\- ]?key|apikey|access[_\- ]?key|secret[_\- ]?key|private[_\- ]?key|client[_\- ]?secret|github|gitlab|bearer|authorization|credential|e[\- ]?mail)(?![A-Za-z]))",
		std::regex::icase);
	return re;
}
const std::regex &assignment_re()
{
	static const std::regex re(R"([:=])");
	return re;
}
const std::regex &upper_ident_re()
{
	static const std::regex re(R"([A-Z][A-Z0-9_]{3,})");
	return re;
}

/* Shannon entropy over the characters of s (bits/char). */
double shannon_entropy(const std::string &s)
{
	if (s.empty())
		return 0.0;
	int counts[256] = {0};
	for (unsigned char c : s)
		counts[c]++;
	double H = 0.0;
	const double n = (double)s.size();
	for (int i = 0; i < 256; ++i) {
		if (counts[i]) {
			double p = counts[i] / n;
			H -= p * std::log2(p);
		}
	}
	return H;
}

bool has_mixed_classes(const std::string &s)
{
	bool lower = false, upper = false, digit = false;
	for (unsigned char c : s) {
		if (c >= 'a' && c <= 'z')
			lower = true;
		else if (c >= 'A' && c <= 'Z')
			upper = true;
		else if (c >= '0' && c <= '9')
			digit = true;
	}
	return (lower + upper + digit) >= 2;
}

/* A word that looks like a high-entropy secret token. */
bool is_high_entropy_token(const std::string &w)
{
	/* 16 chars is a common minimum for real keys/tokens; the mixed-class +
	 * entropy gates below keep ordinary prose words out. */
	if (w.size() < 16)
		return false;
	if (!has_mixed_classes(w))
		return false;
	return shannon_entropy(w) >= 3.2;
}

bool line_is_secret(const OcrLine &line, uint32_t mask)
{
	const std::string &t = line.text;
	if ((mask & CENSR_RULE_EMAILS) && std::regex_search(t, email_re()))
		return true;
	if (mask & CENSR_RULE_APIKEYS) {
		if (std::regex_search(t, aws_re()))
			return true;
		if (std::regex_search(t, github_re()))
			return true;
		if (std::regex_search(t, sk_re()))
			return true;
	}
	/* Label keyword only counts in an assignment-like context. */
	if ((mask & CENSR_RULE_LABELS) && std::regex_search(t, label_re()) &&
	    (std::regex_search(t, assignment_re()) ||
	     std::regex_search(t, upper_ident_re())))
		return true;
	if (!(mask & CENSR_RULE_APIKEYS))
		return false;
	/* High-entropy check on whitespace-separated tokens (not the whole
	 * line — the GPU engine returns a line as one "word", which would make
	 * ordinary long prose lines look high-entropy). */
	{
		size_t i = 0, n = t.size();
		while (i < n) {
			while (i < n && std::isspace((unsigned char)t[i]))
				++i;
			size_t j = i;
			while (j < n && !std::isspace((unsigned char)t[j]))
				++j;
			if (j > i && is_high_entropy_token(t.substr(i, j - i)))
				return true;
			i = j;
		}
	}
	return false;
}

struct Span {
	size_t s, e;
};

void add_span(std::vector<Span> &out, size_t s, size_t e)
{
	if (e > s)
		out.push_back({s, e});
}

/* Character spans of the secret token(s) within a line, for per-word redaction. */
std::vector<Span> secret_spans(const std::string &t, uint32_t mask)
{
	std::vector<Span> spans;
	auto collect = [&](const std::regex &re) {
		for (auto it = std::sregex_iterator(t.begin(), t.end(), re);
		     it != std::sregex_iterator(); ++it)
			add_span(spans, (size_t)it->position(),
				 (size_t)(it->position() + it->length()));
	};
	if (mask & CENSR_RULE_EMAILS)
		collect(email_re());
	if (mask & CENSR_RULE_APIKEYS) {
		collect(aws_re());
		collect(github_re());
		collect(sk_re());
		size_t i = 0, n = t.size();
		while (i < n) {
			while (i < n && std::isspace((unsigned char)t[i]))
				++i;
			size_t j = i;
			while (j < n && !std::isspace((unsigned char)t[j]))
				++j;
			if (j > i) {
				/* For a LABEL=value token, treat the value (after
				 * '='/':') as the secret so the label stays shown. */
				std::string tok = t.substr(i, j - i);
				size_t eqp = tok.find_last_of("=:");
				size_t vs = (eqp == std::string::npos) ? 0 : eqp + 1;
				if (is_high_entropy_token(tok.substr(vs)))
					add_span(spans, i + vs, j);
			}
			i = j;
		}
	}
	if ((mask & CENSR_RULE_LABELS) && std::regex_search(t, label_re()) &&
	    (std::regex_search(t, assignment_re()) ||
	     std::regex_search(t, upper_ident_re()))) {
		size_t eq = t.find_first_of("=:");
		if (eq != std::string::npos)
			add_span(spans, eq + 1, t.size()); /* the value */
		else
			add_span(spans, 0, t.size()); /* can't isolate */
	}

	std::sort(spans.begin(), spans.end(),
		  [](const Span &a, const Span &b) { return a.s < b.s; });
	std::vector<Span> merged;
	for (const auto &sp : spans) {
		if (!merged.empty() && sp.s <= merged.back().e)
			merged.back().e = std::max(merged.back().e, sp.e);
		else
			merged.push_back(sp);
	}
	return merged;
}

} // namespace

std::vector<RectF> find_secret_boxes(const std::vector<OcrLine> &lines,
				     uint32_t rule_mask, bool per_word)
{
	std::vector<RectF> boxes;
	for (const auto &line : lines) {
		if (line.words.empty())
			continue;

		if (!per_word) {
			if (line_is_secret(line, rule_mask))
				boxes.push_back(line.box);
			continue;
		}

		auto spans = secret_spans(line.text, rule_mask);
		if (spans.empty())
			continue;

		/* Reconstruct each word's character range within line.text so a
		 * secret char-span maps to the OCR engine's real word boxes.
		 * Covering a word's whole box never leaves part of the token
		 * visible — proportional char->pixel mapping does, on proportional
		 * fonts or when OCR drops characters. When a span can't be resolved
		 * to a word (or the engine returns the line as one word), fall back
		 * to covering the whole line box (over-cover, never leak). */
		std::vector<std::pair<size_t, size_t>> wranges;
		wranges.reserve(line.words.size());
		size_t cur = 0;
		for (const auto &w : line.words) {
			size_t pos = line.text.find(w.text, cur);
			if (pos == std::string::npos) {
				wranges.push_back({SIZE_MAX, SIZE_MAX});
			} else {
				wranges.push_back({pos, pos + w.text.size()});
				cur = pos + w.text.size();
			}
		}

		for (const auto &sp : spans) {
			bool matched = false;
			for (size_t k = 0; k < line.words.size(); ++k) {
				size_t ws = wranges[k].first, we = wranges[k].second;
				if (ws == SIZE_MAX)
					continue;
				if (ws < sp.e && sp.s < we) { /* char ranges overlap */
					boxes.push_back(line.words[k].box);
					matched = true;
				}
			}
			if (!matched)
				boxes.push_back(line.box);
		}
	}
	return boxes;
}

} // namespace censr
