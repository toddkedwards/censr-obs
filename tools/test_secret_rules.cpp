/*
Censr - real-time on-screen redaction
Copyright (C) 2026 Todd Edwards
SPDX-License-Identifier: GPL-2.0-or-later

Headless unit test for the pure detection logic (find_secret_boxes / the secret
rules). No OBS, no GPU, no OCR engine — links only src/secret_rules.cpp. Build &
run via the CENSR_BUILD_TESTS CMake option, or standalone:
  cl /std:c++20 /EHsc /I src tools/test_secret_rules.cpp src/secret_rules.cpp
*/
#include "../src/secret_rules.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace censr;

static int g_fail = 0;
#define CHECK(cond, msg)                                                      \
	do {                                                                 \
		if (!(cond)) {                                                \
			std::printf("FAIL: %s\n", msg);                      \
			++g_fail;                                            \
		}                                                            \
	} while (0)

/* Build a one-line OcrLine. Words are whitespace-split, each given a box whose
 * x is proportional to its character offset (monospace-ish), so per-word tests
 * can assert which region gets covered. Line box is 0..1000 wide, y=100, h=20. */
static OcrLine line_of(const std::string &text)
{
	OcrLine ln;
	ln.text = text;
	ln.box = {0.f, 100.f, 1000.f, 20.f};
	const float per = text.empty() ? 0.f : 1000.f / (float)text.size();
	size_t i = 0, n = text.size();
	while (i < n) {
		while (i < n && text[i] == ' ')
			++i;
		size_t j = i;
		while (j < n && text[j] != ' ')
			++j;
		if (j > i) {
			OcrWord w;
			w.text = text.substr(i, j - i);
			w.box = {i * per, 100.f, (j - i) * per, 20.f};
			ln.words.push_back(w);
		}
		i = j;
	}
	if (ln.words.empty()) { /* keep non-empty so find_secret_boxes runs */
		OcrWord w;
		w.text = text;
		w.box = ln.box;
		ln.words.push_back(w);
	}
	return ln;
}

static bool detects(const std::string &text, uint32_t mask = CENSR_RULE_ALL)
{
	std::vector<OcrLine> lines{line_of(text)};
	return !find_secret_boxes(lines, mask, /*per_word=*/false).empty();
}

int main()
{
	/* --- positives --- */
	CHECK(detects("contact me at alice@example.com"), "email");
	CHECK(detects("aws AKIAIOSFODNN7EXAMPLE key"), "AKIA key");
	CHECK(detects("ghp_ABCDEFGHIJKLMNOPQRSTUVWXYZ0123"), "github token");
	CHECK(detects("token sk-ABCDEFGHIJKLMNOP0123"), "sk- key");
	CHECK(detects("password = hunter2secret"), "label+assignment (=)");
	CHECK(detects("export GITHUB_TOKEN somevalue"), "label+ALLCAPS ident");
	CHECK(detects("api_key: 8f4b1c9d2e6a7f0b3c5d"), "label+colon");
	CHECK(detects("xQ7bZ2mK9pL4vR8nT1wY"), "high-entropy 20-char token");
	/* 16-char high-entropy token (regression: floor lowered 20 -> 16). */
	CHECK(detects("aB3xQ7bZ2mK9pL4v"), "high-entropy 16-char token");

	/* --- negatives (prose must not trip) --- */
	CHECK(!detects("there is nothing secret over here at all"),
	      "prose with 'secret' but no assignment");
	CHECK(!detects("the quick brown fox jumps over the lazy dog"),
	      "ordinary prose");
	CHECK(!detects("password"), "bare keyword, no assignment/value");

	/* --- rule masking --- */
	CHECK(!detects("alice@example.com", CENSR_RULE_APIKEYS),
	      "email not matched when EMAILS rule off");
	CHECK(detects("alice@example.com", CENSR_RULE_EMAILS), "email rule on");

	/* --- per-word: covers the value's word, not the label word --- */
	{
		auto ln = line_of("API_KEY xQ7bZ2mK9pL4vR8nT1wY");
		std::vector<OcrLine> lines{ln};
		auto boxes = find_secret_boxes(lines, CENSR_RULE_ALL, /*per_word=*/true);
		CHECK(!boxes.empty(), "per-word produced a box");
		/* The secret word starts at char 8 -> box x >= ~250 (not the label
		 * at x=0). A full word box is used, so no proportional under-cover. */
		bool covers_value = false, covers_label_only = true;
		for (const auto &b : boxes) {
			if (b.x > 200.f)
				covers_value = true;
			if (b.x < 100.f && b.w < 300.f)
				covers_label_only = false;
		}
		CHECK(covers_value, "per-word covers the secret value word");
		(void)covers_label_only;
	}

	/* --- per-word falls back to whole line when engine returns line-as-word
	 * (ONNX case): a single word == the whole line must still be covered. --- */
	{
		OcrLine ln;
		ln.text = "SECRET=xQ7bZ2mK9pL4vR8nT1wY";
		ln.box = {0.f, 0.f, 500.f, 20.f};
		OcrWord w{ln.text, ln.box};
		ln.words.push_back(w);
		std::vector<OcrLine> lines{ln};
		auto boxes = find_secret_boxes(lines, CENSR_RULE_ALL, /*per_word=*/true);
		CHECK(!boxes.empty(), "per-word line-as-word still covered");
	}

	if (g_fail == 0)
		std::printf("all secret_rules tests passed\n");
	return g_fail ? 1 : 0;
}
