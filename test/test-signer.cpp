/*
obs-tm-director -- signer unit tests

Standalone: links only against tm-signer.cpp and mbedTLS, so it runs in CI
without OBS or Qt present.

This is the highest-risk code in the plugin. A wrong signature surfaces as a
401 from Tournament Manager that looks identical to a mistyped API key, on
event day, with no useful diagnostics. Pin it hard.
*/

#include "../src/tm/tm-signer.hpp"

#include <cstdio>
#include <string>

namespace {

struct SignerVector {
	const char *name;
	const char *method;
	const char *url;
	const char *token;
	const char *tmDate;
	const char *apiKey;
	const char *expectedStringToSign;
	const char *expectedSignature;
};

#include "signer-vectors.inc"

int gFailures = 0;

void expectEqual(const std::string &label, const std::string &actual, const std::string &expected)
{
	if (actual == expected) {
		std::printf("  ok   %s\n", label.c_str());
		return;
	}

	++gFailures;
	std::printf("  FAIL %s\n         expected: %s\n         actual:   %s\n", label.c_str(), expected.c_str(),
		    actual.c_str());
}

void expectTrue(const std::string &label, bool condition)
{
	if (condition) {
		std::printf("  ok   %s\n", label.c_str());
		return;
	}
	++gFailures;
	std::printf("  FAIL %s\n", label.c_str());
}

/// Every generated vector must reproduce both the canonical string and the digest.
void testGoldenVectors()
{
	std::printf("golden vectors (vs vex-tm-client v1.5.3)\n");

	for (const SignerVector &v : kSignerVectors) {
		tmd::SignableUrl parsed;
		if (!tmd::parseSignableUrl(v.url, parsed)) {
			++gFailures;
			std::printf("  FAIL %s -- could not parse %s\n", v.name, v.url);
			continue;
		}

		expectEqual(std::string(v.name) + " [canonical string]",
			    tmd::buildStringToSign(v.method, parsed, v.token, v.tmDate), v.expectedStringToSign);

		expectEqual(std::string(v.name) + " [signature]",
			    tmd::signRequest(v.method, v.url, v.token, v.tmDate, v.apiKey), v.expectedSignature);
	}
}

/// The default-port rule is the subtlety most likely to be got wrong.
void testHostPortNormalisation()
{
	std::printf("host normalisation\n");

	struct Case {
		const char *url;
		const char *expectedHost;
		const char *expectedPath;
	};

	const Case cases[] = {
		{"http://tm.local/api/event", "tm.local", "/api/event"},
		{"http://tm.local:80/api/event", "tm.local", "/api/event"},
		{"http://tm.local:8080/api/event", "tm.local:8080", "/api/event"},
		{"https://tm.local:443/api/event", "tm.local", "/api/event"},
		{"https://tm.local:8443/api/event", "tm.local:8443", "/api/event"},
		{"ws://tm.local/api/fieldsets/1", "tm.local", "/api/fieldsets/1"},
		{"ws://tm.local:8080/api/fieldsets/1", "tm.local:8080", "/api/fieldsets/1"},
		{"http://192.168.1.50", "192.168.1.50", "/"},
		{"http://tm.local/api/rankings/1/QUAL?x=1", "tm.local", "/api/rankings/1/QUAL?x=1"},
		{"http://tm.local/api/event#frag", "tm.local", "/api/event"},
	};

	for (const Case &c : cases) {
		tmd::SignableUrl parsed;
		if (!tmd::parseSignableUrl(c.url, parsed)) {
			++gFailures;
			std::printf("  FAIL %s -- parse failed\n", c.url);
			continue;
		}
		expectEqual(std::string(c.url) + " [host]", parsed.host, c.expectedHost);
		expectEqual(std::string(c.url) + " [path]", parsed.pathAndQuery, c.expectedPath);
	}
}

void testRejectsMalformedUrls()
{
	std::printf("malformed input\n");

	const char *bad[] = {"", "not a url", "/api/event", "ftp://tm.local/x", "http://", "://x"};

	for (const char *url : bad) {
		tmd::SignableUrl parsed;
		expectTrue(std::string("rejects \"") + url + "\"", !tmd::parseSignableUrl(url, parsed));
	}
}

/// x-tm-date must be a C-locale HTTP-date regardless of the host's locale.
void testHttpDateFormat()
{
	std::printf("http date formatting\n");

	// 2026-08-11T03:14:00Z -- a Tuesday.
	expectEqual("epoch 1786418040", tmd::formatHttpDate(1786418040), "Tue, 11 Aug 2026 03:14:00 GMT");
	expectEqual("unix epoch", tmd::formatHttpDate(0), "Thu, 01 Jan 1970 00:00:00 GMT");
}

/// An empty key or an unparseable URL must fail closed, never emit a partial signature.
void testFailureModes()
{
	std::printf("failure modes\n");

	expectTrue("unparseable url yields empty signature",
		   tmd::signRequest("GET", "nonsense", "tok", "date", "key").empty());

	expectTrue("valid request yields 64 hex chars",
		   tmd::signRequest("GET", "http://tm.local/api/event", "tok", "date", "key").length() == 64);
}

} // namespace

int main()
{
	std::printf("== tm-signer ==\n");

	testGoldenVectors();
	testHostPortNormalisation();
	testRejectsMalformedUrls();
	testHttpDateFormat();
	testFailureModes();

	if (gFailures == 0) {
		std::printf("\nall signer tests passed\n");
		return 0;
	}

	std::printf("\n%d assertion(s) failed\n", gFailures);
	return 1;
}
