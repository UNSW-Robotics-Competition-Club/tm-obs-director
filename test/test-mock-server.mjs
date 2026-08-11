// Self-test for the mock TM API server.
//
// The mock is about to become the oracle the C++ client is developed against,
// so it needs to be trustworthy first. This asserts that it enforces auth the
// way the real API does and that each failure mode is distinguishable, which is
// exactly what the dock's Test Connection button depends on.
//
// Run: node test/test-mock-server.mjs

import { connect } from "node:net";
import { createHash, randomBytes } from "node:crypto";

import { createMockServer, API_KEY, CLIENT_ID, CLIENT_SECRET } from "./mock-tm-server.mjs";
import { sign } from "./tm-sign.mjs";

let failures = 0;

function check(name, condition, detail) {
	if (condition) {
		console.log(`  ok   ${name}`);
	} else {
		failures++;
		console.log(`  FAIL ${name}${detail ? `\n         ${detail}` : ""}`);
	}
}

function listen(scenario) {
	const { server, requests, fieldset } = createMockServer({ scenario, log: false });
	return new Promise((resolve) => {
		server.listen(0, "127.0.0.1", () => {
			const { port } = server.address();
			resolve({ server, requests, fieldset, base: `http://127.0.0.1:${port}` });
		});
	});
}

// --- raw websocket client --------------------------------------------------
//
// Node's global WebSocket cannot set request headers, and the TM upgrade is
// authorised by three of them, so the handshake is spoken directly over TCP.
// That is not only a workaround: it is the only way to send a deliberately
// malformed frame and check the server rejects it.

const WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

function textFrame(text, { masked = true } = {}) {
	const data = Buffer.from(text, "utf8");
	if (data.length >= 126) throw new Error("test frames are meant to be small");

	const header = Buffer.alloc(2);
	header[0] = 0x81; // FIN + text

	if (!masked) {
		header[1] = data.length;
		return Buffer.concat([header, data]);
	}

	header[1] = 0x80 | data.length;
	const mask = randomBytes(4);
	const payload = Buffer.from(data);
	for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
	return Buffer.concat([header, mask, payload]);
}

/// Server frames are never masked, and everything here is short.
function decodeServerFrame(buffer) {
	const length = buffer[1] & 0x7f;
	return { opcode: buffer[0] & 0x0f, payload: buffer.subarray(2, 2 + length).toString("utf8") };
}

function openWebSocket(base, { token, apiKey = API_KEY, path = "/api/fieldsets/1" } = {}) {
	const url = new URL(base);
	const tmDate = new Date().toUTCString();
	const signature = sign({ method: "GET", url: `${base}${path}`, token, tmDate, apiKey });
	const key = randomBytes(16).toString("base64");

	return new Promise((resolve, reject) => {
		const socket = connect(Number(url.port), url.hostname, () => {
			socket.write(
				`GET ${path} HTTP/1.1\r\n` +
					`Host: ${url.host}\r\n` +
					"Upgrade: websocket\r\n" +
					"Connection: Upgrade\r\n" +
					"Sec-WebSocket-Version: 13\r\n" +
					`Sec-WebSocket-Key: ${key}\r\n` +
					`Authorization: Bearer ${token}\r\n` +
					`x-tm-date: ${tmDate}\r\n` +
					`x-tm-signature: ${signature}\r\n\r\n`,
			);
		});

		const timer = setTimeout(() => reject(new Error("handshake timed out")), 5000);
		let head = Buffer.alloc(0);

		const onData = (chunk) => {
			head = Buffer.concat([head, chunk]);
			const end = head.indexOf("\r\n\r\n");
			if (end === -1) return;

			clearTimeout(timer);
			socket.off("data", onData);

			const text = head.subarray(0, end).toString("ascii");
			const accept = /sec-websocket-accept:\s*(\S+)/i.exec(text)?.[1];

			resolve({
				socket,
				key,
				status: Number(text.split(" ")[1]),
				accept,
				expectedAccept: createHash("sha1").update(key + WS_GUID).digest("base64"),
			});
		};

		socket.on("data", onData);
		socket.on("error", (err) => {
			clearTimeout(timer);
			reject(err);
		});
	});
}

/// Resolves with the next frame the server sends, or null if it hangs up first.
function nextFrame(socket, timeoutMs = 3000) {
	return new Promise((resolve) => {
		const done = (value) => {
			clearTimeout(timer);
			socket.off("data", onData);
			socket.off("close", onClose);
			resolve(value);
		};
		const timer = setTimeout(() => done(null), timeoutMs);
		const onData = (chunk) => done(decodeServerFrame(chunk));
		const onClose = () => done(null);

		socket.on("data", onData);
		socket.on("close", onClose);
	});
}

function waitForClose(socket, timeoutMs = 3000) {
	return new Promise((resolve) => {
		const timer = setTimeout(() => resolve(false), timeoutMs);
		socket.on("close", () => {
			clearTimeout(timer);
			resolve(true);
		});
	});
}

async function getToken(base) {
	const res = await fetch(`${base}/oauth2/token`, {
		method: "POST",
		headers: { "Content-Type": "application/x-www-form-urlencoded" },
		body: new URLSearchParams({
			client_id: CLIENT_ID,
			client_secret: CLIENT_SECRET,
			grant_type: "client_credentials",
		}),
	});
	return { status: res.status, body: await res.json() };
}

// Mirrors what the C++ client must do for every call.
async function signedGet(base, path, { token, apiKey = API_KEY, tmDate, extraHeaders = {} } = {}) {
	const date = tmDate ?? new Date().toUTCString();
	const signature = sign({ method: "GET", url: `${base}${path}`, token, tmDate: date, apiKey });
	const res = await fetch(`${base}${path}`, {
		headers: {
			Authorization: `Bearer ${token}`,
			"x-tm-date": date,
			"x-tm-signature": signature,
			...extraHeaders,
		},
	});
	const text = await res.text();
	return { status: res.status, headers: res.headers, body: text ? JSON.parse(text) : null };
}

async function run() {
	// --- happy path ---------------------------------------------------------
	console.log("happy path");
	{
		const { server, base } = await listen("ok");
		try {
			const tok = await getToken(base);
			check("token endpoint returns 200", tok.status === 200, `got ${tok.status}`);
			check("token has access_token", typeof tok.body.access_token === "string");
			check("token expires_in ~2h", tok.body.expires_in === 7200, `got ${tok.body.expires_in}`);

			const token = tok.body.access_token;

			const event = await signedGet(base, "/api/event", { token });
			check("signed GET /api/event is 200", event.status === 200, JSON.stringify(event.body));
			check("event payload shape", event.body?.event?.code === "RE-V5RC-25-0000");
			check("Last-Modified present", !!event.headers.get("last-modified"));

			const fieldsets = await signedGet(base, "/api/fieldsets", { token });
			check("fieldsets listed", fieldsets.body?.fieldSets?.length === 2);

			const fields = await signedGet(base, "/api/fieldsets/1/fields", { token });
			check("fields for field set 1", fields.body?.fields?.length === 2);

			const matches = await signedGet(base, "/api/matches/1", { token });
			check("matches for division 1", matches.body?.matches?.length === 3);
			check(
				"match tuple shape",
				matches.body?.matches?.[0]?.matchInfo?.matchTuple?.round === "QUAL",
			);
			check(
				"unplayed match has state",
				matches.body?.matches?.[2]?.matchInfo?.state === "UNPLAYED",
			);

			const div2 = await signedGet(base, "/api/teams/1", { token });
			check("teams filtered by division", div2.body?.teams?.length === 4);

			// Conditional GET: a client that echoes Last-Modified gets a 304.
			const lastModified = event.headers.get("last-modified");
			const cached = await signedGet(base, "/api/event", {
				token,
				extraHeaders: { "If-Modified-Since": lastModified },
			});
			check("If-Modified-Since yields 304", cached.status === 304, `got ${cached.status}`);

			const stale = await signedGet(base, "/api/event", {
				token,
				extraHeaders: { "If-Modified-Since": new Date("2020-01-01").toUTCString() },
			});
			check("stale If-Modified-Since yields 200", stale.status === 200, `got ${stale.status}`);

			const missing = await signedGet(base, "/api/matches/99", { token });
			check("unknown resource is 404", missing.status === 404, `got ${missing.status}`);
		} finally {
			server.close();
		}
	}

	// --- auth failures, each distinguishable --------------------------------
	console.log("auth failures");
	{
		const { server, base } = await listen("ok");
		try {
			const { body } = await getToken(base);
			const token = body.access_token;

			const noAuth = await fetch(`${base}/api/event`);
			check("unsigned request is 401", noAuth.status === 401);
			check("unsigned request says missing_token", (await noAuth.json()).error === "missing_token");

			const badToken = await signedGet(base, "/api/event", { token: "not-a-real-token" });
			check("unknown token rejected", badToken.body?.error === "unknown_token", JSON.stringify(badToken.body));

			// Wrong TM API key -> signature will not match. This is the case a
			// user hits when they paste the key from the wrong event.
			const badKey = await signedGet(base, "/api/event", { token, apiKey: "WRONG-KEY" });
			check("wrong API key is bad_signature", badKey.body?.error === "bad_signature", JSON.stringify(badKey.body));

			// Skewed clock. Distinct from a bad key even though both are 401,
			// because the remedy is completely different.
			const skewed = await signedGet(base, "/api/event", {
				token,
				tmDate: new Date(Date.now() - 3600_000).toUTCString(),
			});
			check("clock skew is clock_skew", skewed.body?.error === "clock_skew", JSON.stringify(skewed.body));

			const noDate = await fetch(`${base}/api/event`, {
				headers: { Authorization: `Bearer ${token}`, "x-tm-signature": "0".repeat(64) },
			});
			check("missing x-tm-date rejected", (await noDate.json()).error === "missing_date");
		} finally {
			server.close();
		}
	}

	// --- scenarios ----------------------------------------------------------
	console.log("scenarios");
	{
		const { server, base } = await listen("unauthorized");
		try {
			const tok = await getToken(base);
			check("unauthorized scenario rejects credentials", tok.status === 401, `got ${tok.status}`);
			check("reports invalid_client", tok.body.error === "invalid_client");
		} finally {
			server.close();
		}
	}
	{
		const { server, base } = await listen("api-disabled");
		try {
			// No auth needed: the point is that /api/* does not exist at all, so
			// the client must not mistake this for an auth problem.
			const res = await fetch(`${base}/api/event`);
			check("api-disabled yields 404 not 401", res.status === 404, `got ${res.status}`);
		} finally {
			server.close();
		}
	}
	{
		const { server, base } = await listen("bad-signature");
		try {
			const { body } = await getToken(base);
			const res = await signedGet(base, "/api/event", { token: body.access_token });
			check("bad-signature scenario forces rejection", res.body?.error === "bad_signature");
		} finally {
			server.close();
		}
	}
	{
		const { server, base } = await listen("token-expired");
		try {
			const { body } = await getToken(base);
			check("token-expired issues expires_in <= 0", body.expires_in <= 0, `got ${body.expires_in}`);
			const res = await signedGet(base, "/api/event", { token: body.access_token });
			check("expired token rejected", res.body?.error === "expired_token", JSON.stringify(res.body));
		} finally {
			server.close();
		}
	}

	// --- field set websocket ------------------------------------------------
	//
	// The websocket half of the mock is hand-rolled RFC 6455, so it gets the same
	// treatment as the REST half: prove it enforces what it claims to before any
	// C++ is developed against it.
	console.log("field set websocket");
	{
		const { server, fieldset, base } = await listen("ok");
		try {
			const { body } = await getToken(base);
			const token = body.access_token;

			const refused = await openWebSocket(base, { token, apiKey: "WRONG-KEY" });
			check("upgrade with a bad signature is refused", refused.status === 401,
			      `got ${refused.status}`);
			refused.socket.destroy();

			const missing = await openWebSocket(base, { token, path: "/api/fieldsets/99" });
			check("upgrade to an unknown field set is 404", missing.status === 404,
			      `got ${missing.status}`);
			missing.socket.destroy();

			const ws = await openWebSocket(base, { token });
			check("signed upgrade is accepted", ws.status === 101, `got ${ws.status}`);
			check("Sec-WebSocket-Accept is the RFC 6455 digest", ws.accept === ws.expectedAccept,
			      `${ws.accept} != ${ws.expectedAccept}`);

			const event = nextFrame(ws.socket);
			fieldset.emit({ type: "matchStarted", fieldID: 1 });
			const received = await event;
			check("emitted events reach the client",
			      received !== null && JSON.parse(received.payload).type === "matchStarted",
			      JSON.stringify(received));

			// A masked command is accepted, recorded, and answered with the
			// broadcast real TM sends.
			const echo = nextFrame(ws.socket);
			ws.socket.write(textFrame(JSON.stringify({ cmd: "setAudienceDisplay", display: "RANKINGS" })));
			const answer = await echo;
			check("masked command is accepted", fieldset.commands.length === 1,
			      JSON.stringify(fieldset.commands));
			check("setAudienceDisplay is echoed to everyone",
			      answer !== null && JSON.parse(answer.payload).display === "RANKINGS",
			      JSON.stringify(answer));

			ws.socket.destroy();

			// RFC 6455 requires client frames to be masked. The decoder rejecting
			// an unmasked one is what makes this mock a real check on our client
			// rather than a rubber stamp.
			const sloppy = await openWebSocket(base, { token });
			check("second upgrade accepted", sloppy.status === 101, `got ${sloppy.status}`);
			const closed = waitForClose(sloppy.socket);
			sloppy.socket.write(textFrame(JSON.stringify({ cmd: "start" }), { masked: false }));
			check("unmasked client frame drops the connection", await closed);
			check("and the command is not recorded", fieldset.commands.length === 1,
			      JSON.stringify(fieldset.commands));
			sloppy.socket.destroy();
		} finally {
			fieldset.dropAll();
			server.close();
		}
	}
	{
		const { server, fieldset, base } = await listen("api-disabled");
		try {
			const { body } = await getToken(base);
			const refused = await openWebSocket(base, { token: body.access_token });
			check("websocket is absent when the Local TM API is off", refused.status === 404,
			      `got ${refused.status}`);
			refused.socket.destroy();
		} finally {
			fieldset.dropAll();
			server.close();
		}
	}

	console.log(failures === 0 ? "\nall mock server tests passed" : `\n${failures} FAILED`);
	process.exit(failures === 0 ? 0 : 1);
}

run().catch((err) => {
	console.error(err);
	process.exit(1);
});
