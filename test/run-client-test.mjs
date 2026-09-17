// Drives test-tm-client against the mock TM server.
//
// Starts the mock on an ephemeral port for each scenario, runs the C++ binary
// against it, and shuts it down. Hermetic: no TM, no LAN, no credentials, no
// fixed port to collide with.
//
// Run: node test/run-client-test.mjs <path-to-test-tm-client>

import { spawn } from "node:child_process";
import { createMockServer } from "./mock-tm-server.mjs";

const binary = process.argv[2];

if (!binary) {
	console.error("usage: node test/run-client-test.mjs <path-to-test-tm-client>");
	process.exit(2);
}

// Each entry pairs a C++ scenario with the mock server scenario it needs.
// "unreachable" runs without a server at all -- that is the point of it.
const CASES = [
	{ client: "ok", server: "ok" },
	{ client: "bad-key", server: "ok" },
	// The three sign-in key faults the operator can actually see the difference
	// between: a key the server refuses, no key at all, and a mistyped one. The
	// last two never reach the network.
	{ client: "revoked-key", server: "ok" },
	{ client: "no-key", server: "ok" },
	{ client: "malformed-key", server: "ok" },
	{ client: "upgrade-required", server: "cerberus-upgrade-required" },
	{ client: "rate-limited", server: "cerberus-rate-limited" },
	{ client: "credentials-invalid", server: "cerberus-credentials-invalid" },
	{ client: "upstream-down", server: "cerberus-upstream" },
	// Both shapes of "the Local TM API is off". 503 is what real TM sends; 404 is
	// kept because the client used to only handle that one, and only that one.
	{ client: "api-disabled", server: "api-disabled" },
	{ client: "api-disabled", server: "api-disabled-404" },
	{ client: "clock-skew", server: "clock-skew" },
	{ client: "unreachable", server: null },
];

function startServer(scenario) {
	const { server, requests } = createMockServer({ scenario, log: false });
	return new Promise((resolve) => {
		server.listen(0, "127.0.0.1", () => {
			resolve({ server, requests, base: `http://127.0.0.1:${server.address().port}` });
		});
	});
}

function runBinary(scenario, base) {
	return new Promise((resolve) => {
		const child = spawn(binary, [scenario, base], { stdio: "inherit" });
		child.on("close", (code) => resolve(code ?? 1));
		child.on("error", (err) => {
			console.error(`  could not run ${binary}: ${err.message}`);
			resolve(1);
		});
	});
}

let failures = 0;

for (const { client, server: scenario } of CASES) {
	console.log(`\n=== ${client} ===`);

	if (scenario === null) {
		const code = await runBinary(client, "http://127.0.0.1:1");
		if (code !== 0) failures++;
		continue;
	}

	const { server, requests, base } = await startServer(scenario);
	try {
		const code = await runBinary(client, base);
		if (code !== 0) failures++;

		// The API guide asks clients not to hammer the server, and Cerberus exists
		// so DWAB is called once an hour rather than once a request. A client that
		// asked Cerberus every time would move the stampede one hop closer rather
		// than remove it, so assert the caching rather than trust it.
		if (client === "ok") {
			const tokenRequests = requests.filter((r) => r.path === "/v1/token").length;
			if (tokenRequests === 1) {
				console.log("  ok   bearer minted exactly once and cached");
			} else {
				console.log(`  FAIL bearer minted ${tokenRequests} times, expected 1`);
				failures++;
			}

			const signed = requests.filter((r) => r.path.startsWith("/api/"));
			const allSigned = signed.every(
				(r) => r.headers["x-tm-signature"] && r.headers["x-tm-date"] && r.headers["authorization"],
			);
			if (allSigned && signed.length > 0) {
				console.log(`  ok   all ${signed.length} API requests carried the three auth headers`);
			} else {
				console.log("  FAIL some API requests were missing auth headers");
				failures++;
			}
		}
	} finally {
		server.close();
	}
}

console.log(failures === 0 ? "\nall tm-client tests passed" : `\n${failures} scenario(s) FAILED`);
process.exit(failures === 0 ? 0 : 1);
