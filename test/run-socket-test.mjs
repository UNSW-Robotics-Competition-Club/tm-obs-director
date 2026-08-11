// Drives test-field-set-socket against the mock TM server's websocket.
//
// Each scenario starts the mock on an ephemeral port, launches the C++ binary
// against it, and then plays a script of events at it -- emitting only once a
// client has actually attached, so there is no sleep-and-hope anywhere.
//
// Run: node test/run-socket-test.mjs <path-to-test-field-set-socket>

import { spawn } from "node:child_process";
import { createMockServer, matchCycle } from "./mock-tm-server.mjs";

const binary = process.argv[2];

if (!binary) {
	console.error("usage: node test/run-socket-test.mjs <path-to-test-field-set-socket>");
	process.exit(2);
}

const delay = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

// A drop needs to land after the client has definitely read the first event.
// The socket polls every 50ms, so this is several times the worst case.
const SETTLE_MS = 300;

let failures = 0;

function fail(message) {
	console.log(`  FAIL ${message}`);
	failures++;
}

function pass(message) {
	console.log(`  ok   ${message}`);
}

function startServer(scenario) {
	const { server, requests, fieldset } = createMockServer({ scenario, log: false });
	return new Promise((resolve) => {
		server.listen(0, "127.0.0.1", () => {
			resolve({
				server,
				requests,
				fieldset,
				base: `http://127.0.0.1:${server.address().port}`,
			});
		});
	});
}

function runBinary(scenario, base) {
	return new Promise((resolve) => {
		const args = base ? [scenario, base] : [scenario];
		const child = spawn(binary, args, { stdio: "inherit" });
		child.on("close", (code) => resolve(code ?? 1));
		child.on("error", (err) => {
			console.error(`  could not run ${binary}: ${err.message}`);
			resolve(1);
		});
	});
}

// Each case pairs a C++ scenario with the mock scenario it needs and the script
// the server plays while the binary is running. `server: null` means run with no
// server at all, which is the point of the unreachable case.
const CASES = [
	{
		client: "parse",
		server: null,
		base: null,
	},
	{
		client: "stream",
		server: "ok",
		async drive(fieldset) {
			await fieldset.waitForClient();
			for (const event of matchCycle({ fieldId: 1, matchNumber: 1 })) {
				fieldset.emit(event);
				await delay(30);
			}
		},
		verify(fieldset, requests) {
			const upgrades = requests.filter((r) => r.upgrade);
			if (upgrades.length !== 1) return fail(`expected 1 upgrade request, saw ${upgrades.length}`);
			pass("one upgrade request");

			// The whole reason libcurl won over LibDataChannel: the upgrade has
			// to carry the same three auth headers a REST call does. If these
			// stopped being sent the mock would refuse the upgrade, but assert
			// it directly so the failure names the cause.
			const headers = upgrades[0].headers;
			const signed = headers["authorization"] && headers["x-tm-date"] && headers["x-tm-signature"];
			if (!signed) return fail("upgrade request was missing auth headers");
			pass("upgrade carried the three auth headers");
		},
	},
	{
		client: "command",
		server: "ok",
		async drive(fieldset) {
			// Nothing to send: the binary drives this one. Just stay up until it
			// has issued its command and seen the echo.
			await fieldset.waitForClient();
		},
		verify(fieldset) {
			if (fieldset.commands.length !== 1) {
				return fail(`expected 1 command, server saw ${fieldset.commands.length}`);
			}
			const command = fieldset.commands[0];
			if (command.cmd !== "setAudienceDisplay" || command.display !== "RANKINGS") {
				return fail(`unexpected command: ${JSON.stringify(command)}`);
			}
			// Reaching here means the frame arrived masked and well-formed: the
			// decoder destroys the connection on an unmasked client frame.
			pass("server received a correctly framed setAudienceDisplay command");
		},
	},
	{
		client: "reconnect",
		server: "ok",
		async drive(fieldset) {
			await fieldset.waitForClient();
			fieldset.emit({ type: "matchStarted", fieldID: 1 });
			await delay(SETTLE_MS);

			// No close frame, no FIN handshake -- exactly what a TM restart or a
			// yanked cable looks like from our end.
			fieldset.dropAll();

			await fieldset.waitForClient();
			fieldset.emit({ type: "matchStopped", fieldID: 1 });
		},
		verify(fieldset) {
			if (fieldset.connections.length < 2) {
				return fail(`expected >= 2 upgrades, saw ${fieldset.connections.length}`);
			}
			pass(`client reconnected (${fieldset.connections.length} upgrades)`);
		},
	},
	{
		// Nothing forced here: a healthy server rejecting a genuinely wrong
		// signature. This is what proves the upgrade is really signed.
		client: "bad-key",
		server: "ok",
		async drive() {},
		verify(fieldset) {
			if (fieldset.connections.length !== 0) {
				return fail(`server accepted ${fieldset.connections.length} upgrades with a bad key`);
			}
			pass("server accepted no upgrade with a wrong API key");
		},
	},
	{
		client: "bad-signature",
		server: "bad-signature",
		async drive() {},
	},
	{
		client: "api-disabled",
		server: "api-disabled",
		async drive() {},
	},
	{
		client: "unreachable",
		server: null,
		base: "http://127.0.0.1:1",
	},
];

for (const testCase of CASES) {
	console.log(`\n=== ${testCase.client} ===`);

	if (testCase.server === null) {
		const code = await runBinary(testCase.client, testCase.base);
		if (code !== 0) failures++;
		continue;
	}

	const { server, requests, fieldset, base } = await startServer(testCase.server);

	try {
		const running = runBinary(testCase.client, base);

		try {
			await testCase.drive(fieldset);
		} catch (err) {
			fail(`driving the scenario: ${err.message}`);
		}

		const code = await running;
		if (code !== 0) failures++;

		if (testCase.verify) testCase.verify(fieldset, requests);
	} finally {
		fieldset.dropAll();
		server.close();
	}
}

console.log(failures === 0 ? "\nall field set socket tests passed" : `\n${failures} scenario(s) FAILED`);
process.exit(failures === 0 ? 0 : 1);
