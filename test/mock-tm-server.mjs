// Mock VEX Tournament Manager Public API server.
//
// Serves the endpoints documented in the TM Public API Guide v1.2 so the plugin
// can be developed and tested without TM, without a LAN, and without approved
// API credentials. It enforces the real auth rules rather than waving them
// through: a client that passes here should pass against real TM.
//
// It validates incoming signatures with test/tm-sign.mjs -- the same reference
// implementation that generates our C++ golden vectors -- so the mock and the
// unit tests cannot drift apart.
//
// Run:  node test/mock-tm-server.mjs [--port 8080] [--scenario <name>]
//
// Scenarios exist to reproduce the failure modes the dock's Test Connection
// button must tell apart. See SCENARIOS below.

import { createServer } from "node:http";
import { createHash, createHmac, timingSafeEqual } from "node:crypto";
import { sign } from "./tm-sign.mjs";

// ---------------------------------------------------------------------------
// Fixed credentials. Test fixtures, not secrets: the real ones are per-event
// and never live in this repo.
// ---------------------------------------------------------------------------

export const CLIENT_SECRET = "mock-client-secret";
export const API_KEY = "TESTAPIKEY0123456789";

// The bearer now comes from Cerberus rather than straight from DWAB, so this
// mock stands in for Cerberus's /v1/token as well as for TM itself. A well
// formed key: ctm_<16 hex>_<48 hex>.
export const CERBERUS_KEY = "ctm_0123456789abcdef_" + "0123456789abcdef".repeat(3);

// Cerberus refuses builds below a floor, which is why the client sends its
// version at all. Anything below this is answered with upgrade_required.
export const MIN_BUILD = "0.1.0";

const TOKEN_TTL_SECONDS = 7200; // real TM tokens last ~2h
const MAX_CLOCK_SKEW_SECONDS = 300;

// ---------------------------------------------------------------------------
// Scenarios: force a specific failure so the client's error mapping is testable.
// ---------------------------------------------------------------------------

const SCENARIOS = {
	ok: "Everything works.",
	"api-disabled": "Event Partner never ticked Enable Local TM API: /api/* is 503, as real TM answers.",
	"api-disabled-404": "The same thing, but answered 404. Older TM builds, and a defensive second shape.",
	"bad-signature": "Every signature is rejected, as with a wrong TM API key.",
	"token-expired": "Tokens are issued already expired, forcing a refresh path.",
	"cerberus-invalid-key": "Sign-in rejects this build's client key (unknown, malformed, or revoked).",
	"cerberus-upgrade-required": "Sign-in refuses this build as older than the supported minimum.",
	"cerberus-rate-limited": "Sign-in is rate limiting this key.",
	"cerberus-credentials-invalid":
		"Sign-in reached DWAB and DWAB rejected SRA's own credential. Not the operator's fault, " +
		"and deliberately not a 401.",
	"cerberus-upstream": "Sign-in could not reach DWAB at all.",
	"clock-skew":
		"The server's clock reads an hour ahead, so requests are rejected the way real TM " +
		"rejects a client whose clock has drifted.",
};

/// How far ahead the clock-skew scenario pretends the server's clock is.
const SKEW_SCENARIO_OFFSET_SECONDS = 3600;

// ---------------------------------------------------------------------------
// Payloads, verbatim in shape from the API guide.
// ---------------------------------------------------------------------------

const EVENT = { event: { name: "SRA Mock Scrimmage", code: "RE-V5RC-25-0000" } };

const DIVISIONS = { divisions: [{ name: "Division 1", id: 1 }, { name: "Division 2", id: 2 }] };

const FIELD_SETS = { fieldSets: [{ id: 1, name: "Match Field Set" }, { id: 2, name: "Skills Field Set" }] };

const FIELDS = {
	1: { fields: [{ id: 1, name: "Field 1" }, { id: 2, name: "Field 2" }] },
	2: { fields: [{ id: 3, name: "Skills 1" }] },
};

const TEAMS = {
	teams: [
		{ number: "1234A", name: "Mock Robotics A", school: "SRA", city: "Sydney", region: "NSW",
		  country: "Australia", ageGroup: "High School", divId: 1, checkedIn: true },
		{ number: "1234B", name: "Mock Robotics B", school: "SRA", city: "Sydney", region: "NSW",
		  country: "Australia", ageGroup: "High School", divId: 1, checkedIn: true },
		{ number: "5678C", name: "Test Team C", school: "Other", city: "Melbourne", region: "VIC",
		  country: "Australia", ageGroup: "High School", divId: 1, checkedIn: false },
		{ number: "5678D", name: "Test Team D", school: "Other", city: "Melbourne", region: "VIC",
		  country: "Australia", ageGroup: "High School", divId: 1, checkedIn: true },
	],
};

function match(round, number, state, red, blue, score) {
	return {
		finalScore: score ?? [0, 0],
		winningAlliance: score ? (score[0] > score[1] ? 0 : score[0] < score[1] ? 1 : -1) : -1,
		matchInfo: {
			timeScheduled: 1786418040 + number * 360,
			state,
			alliances: [
				{ teams: red.map((number) => ({ number })) },
				{ teams: blue.map((number) => ({ number })) },
			],
			matchTuple: { session: 0, division: 1, round, instance: 1, match: number },
		},
	};
}

const MATCHES = {
	1: {
		matches: [
			match("QUAL", 1, "SCORED", ["1234A", "1234B"], ["5678C", "5678D"], [42, 31]),
			match("QUAL", 2, "SCORED", ["5678C", "1234A"], ["1234B", "5678D"], [15, 88]),
			match("QUAL", 3, "UNPLAYED", ["1234A", "5678D"], ["1234B", "5678C"], null),
		],
	},
	2: { matches: [] },
};

const RANKINGS = {
	rankings: [
		{ rank: 1, tied: false, number: "1234A", wins: 2, losses: 0, ties: 0, wp: 4, ap: 12, sp: 31,
		  avgPoints: 42.0, totalPoints: 84, highScore: 42, numMatches: 2 },
		{ rank: 2, tied: false, number: "5678D", wins: 1, losses: 1, ties: 0, wp: 2, ap: 8, sp: 20,
		  avgPoints: 30.0, totalPoints: 60, highScore: 88, numMatches: 2 },
	],
};

const SKILLS = {
	skillsRankings: [
		{ rank: 1, tie: false, number: "1234A", totalScore: 55, progHighScore: 25, driverHighScore: 30,
		  progAttempts: 3, driverAttempts: 3 },
		{ rank: 2, tie: false, number: "5678C", totalScore: 40, progHighScore: 18, driverHighScore: 22,
		  progAttempts: 3, driverAttempts: 2 },
	],
};

// Fixed so If-Modified-Since is deterministic across runs.
const LAST_MODIFIED = new Date("2026-08-11T03:00:00Z");

// ---------------------------------------------------------------------------
// Auth
// ---------------------------------------------------------------------------

const issuedTokens = new Map(); // token -> expiry epoch seconds

function issueToken(scenario) {
	const token = createHmac("sha256", CLIENT_SECRET)
		.update(`${Date.now()}:${Math.random()}`)
		.digest("hex");
	const ttl = scenario === "token-expired" ? -1 : TOKEN_TTL_SECONDS;
	issuedTokens.set(token, Math.floor(Date.now() / 1000) + ttl);
	return { token, ttl };
}

function constantTimeEquals(a, b) {
	const ab = Buffer.from(a, "utf8");
	const bb = Buffer.from(b, "utf8");
	if (ab.length !== bb.length) return false;
	return timingSafeEqual(ab, bb);
}

// Returns null when authorised, or {status, code, message} describing exactly
// which auth layer rejected the request. The distinct codes are the whole point:
// the dock has to tell a wrong API key from a skewed clock from a disabled API.
function authorise(req, scenario) {
	const auth = req.headers["authorization"] ?? "";
	const tmDate = req.headers["x-tm-date"];
	const signature = req.headers["x-tm-signature"];

	if (!auth.startsWith("Bearer ")) {
		return { status: 401, code: "missing_token", message: "Authorization: Bearer <token> required" };
	}
	const token = auth.slice("Bearer ".length);

	const expiry = issuedTokens.get(token);
	if (expiry === undefined) {
		return { status: 401, code: "unknown_token", message: "Token was not issued by this server" };
	}
	if (expiry < Math.floor(Date.now() / 1000)) {
		return { status: 401, code: "expired_token", message: "Access token has expired" };
	}

	if (!tmDate) return { status: 401, code: "missing_date", message: "x-tm-date header required" };
	if (!signature) return { status: 401, code: "missing_signature", message: "x-tm-signature header required" };

	const parsed = Date.parse(tmDate);
	if (Number.isNaN(parsed)) {
		return { status: 401, code: "malformed_date", message: "x-tm-date is not an HTTP date" };
	}
	const skew = Math.abs(Date.now() - parsed) / 1000;
	if (skew > MAX_CLOCK_SKEW_SECONDS) {
		return {
			status: 401,
			code: "clock_skew",
			message: `x-tm-date is ${Math.round(skew)}s from server time (max ${MAX_CLOCK_SKEW_SECONDS}s)`,
		};
	}

	if (scenario === "bad-signature") {
		return { status: 401, code: "bad_signature", message: "Signature rejected (forced by scenario)" };
	}

	// Rebuild the canonical string exactly as the client should have. The Host
	// header is what the client signed, so it -- not our listen address -- is
	// what must go into the URL we reconstruct.
	const host = req.headers["host"] ?? "";
	const expected = sign({
		method: req.method,
		url: `http://${host}${req.url}`,
		token,
		tmDate,
		apiKey: API_KEY,
	});

	if (!constantTimeEquals(expected, signature)) {
		return {
			status: 401,
			code: "bad_signature",
			message: "x-tm-signature does not match; check the TM API key",
		};
	}

	return null;
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

function routeApi(pathname) {
	if (pathname === "/api/event") return EVENT;
	if (pathname === "/api/divisions") return DIVISIONS;
	if (pathname === "/api/fieldsets") return FIELD_SETS;
	if (pathname === "/api/teams") return TEAMS;
	if (pathname === "/api/skills") return SKILLS;

	let m;
	if ((m = pathname.match(/^\/api\/fieldsets\/(\d+)\/fields$/))) return FIELDS[m[1]] ?? null;
	if ((m = pathname.match(/^\/api\/teams\/(\d+)$/))) {
		return { teams: TEAMS.teams.filter((t) => t.divId === Number(m[1])) };
	}
	if ((m = pathname.match(/^\/api\/matches\/(\d+)$/))) return MATCHES[m[1]] ?? null;
	if ((m = pathname.match(/^\/api\/rankings\/(\d+)\/(\w+)$/))) return MATCHES[m[1]] ? RANKINGS : null;

	return null;
}

// ---------------------------------------------------------------------------
// WebSocket
//
// Node ships a WebSocket *client* but no server, and the whole point of this
// mock is to have no dependencies, so the handshake and RFC 6455 framing are
// done by hand below. It is less code than it looks: TM's messages are small
// text frames, and the only wire subtleties that matter are the accept-key
// digest and unmasking client frames.
// ---------------------------------------------------------------------------

const WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

const OPCODE = { CONTINUATION: 0x0, TEXT: 0x1, BINARY: 0x2, CLOSE: 0x8, PING: 0x9, PONG: 0xa };

/// Guards the frame decoder against a client claiming an absurd payload length.
const MAX_FRAME_BYTES = 1 << 20;

function websocketAccept(key) {
	return createHash("sha1").update(key + WS_GUID).digest("base64");
}

/// Encodes one complete server-to-server frame. Server frames are never masked.
function encodeFrame(payload, opcode = OPCODE.TEXT) {
	const data = Buffer.isBuffer(payload) ? payload : Buffer.from(payload, "utf8");

	let header;
	if (data.length < 126) {
		header = Buffer.alloc(2);
		header[1] = data.length;
	} else if (data.length < 65536) {
		header = Buffer.alloc(4);
		header[1] = 126;
		header.writeUInt16BE(data.length, 2);
	} else {
		header = Buffer.alloc(10);
		header[1] = 127;
		header.writeBigUInt64BE(BigInt(data.length), 2);
	}
	header[0] = 0x80 | opcode; // FIN

	return Buffer.concat([header, data]);
}

/// Incremental frame decoder. Feeds whole messages to `handlers.message`.
///
/// It enforces the rule that client-to-server frames must be masked. That is not
/// pedantry: it is a free check that our libcurl client speaks the protocol
/// properly, and an unmasked frame would sail past a laxer decoder.
function createFrameDecoder(handlers) {
	let pending = Buffer.alloc(0);
	let message = Buffer.alloc(0);

	return function feed(chunk) {
		pending = Buffer.concat([pending, chunk]);

		for (;;) {
			if (pending.length < 2) return;

			const fin = (pending[0] & 0x80) !== 0;
			const opcode = pending[0] & 0x0f;
			const masked = (pending[1] & 0x80) !== 0;

			let length = pending[1] & 0x7f;
			let offset = 2;

			if (length === 126) {
				if (pending.length < offset + 2) return;
				length = pending.readUInt16BE(offset);
				offset += 2;
			} else if (length === 127) {
				if (pending.length < offset + 8) return;
				const wide = pending.readBigUInt64BE(offset);
				if (wide > BigInt(MAX_FRAME_BYTES)) return handlers.protocolError("frame too large");
				length = Number(wide);
				offset += 8;
			}

			if (length > MAX_FRAME_BYTES) return handlers.protocolError("frame too large");

			if (!masked) return handlers.protocolError("client frame was not masked");

			if (pending.length < offset + 4) return;
			const mask = pending.subarray(offset, offset + 4);
			offset += 4;

			if (pending.length < offset + length) return;

			const payload = Buffer.from(pending.subarray(offset, offset + length));
			for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];

			pending = pending.subarray(offset + length);

			if (opcode === OPCODE.CLOSE) return handlers.close();
			if (opcode === OPCODE.PING) {
				handlers.ping(payload);
				continue;
			}
			if (opcode === OPCODE.PONG) continue;

			message = Buffer.concat([message, payload]);
			if (fin) {
				const complete = message;
				message = Buffer.alloc(0);
				handlers.message(complete.toString("utf8"));
			}
		}
	};
}

function sendJson(res, status, body, extraHeaders = {}) {
	const payload = JSON.stringify(body);
	res.writeHead(status, {
		"Content-Type": "application/json",
		"Content-Length": Buffer.byteLength(payload),
		...extraHeaders,
	});
	res.end(payload);
}

async function readBody(req) {
	const chunks = [];
	for await (const chunk of req) chunks.push(chunk);
	return Buffer.concat(chunks).toString("utf8");
}

export function createMockServer({ scenario = "ok", log = true } = {}) {
	if (!(scenario in SCENARIOS)) {
		throw new Error(`Unknown scenario "${scenario}". Known: ${Object.keys(SCENARIOS).join(", ")}`);
	}

	const requests = []; // inspected by tests

	const server = createServer(async (req, res) => {
		const url = new URL(req.url, `http://${req.headers["host"] ?? "localhost"}`);
		requests.push({ method: req.method, path: url.pathname, headers: { ...req.headers } });

		const trace = (status, note) => {
			if (log) console.log(`  ${req.method} ${url.pathname} -> ${status}${note ? ` (${note})` : ""}`);
		};

		// --- Cerberus token endpoint ------------------------------------------
		if (url.pathname === "/v1/token") {
			if (req.method !== "POST") {
				trace(405);
				return sendJson(res, 405, { error: "bad_request", message: "POST only" });
			}

			// Ordering matters and mirrors Cerberus: the key is checked before the
			// build floor, so a revoked key never learns the minimum version.
			const auth = req.headers["authorization"] ?? "";
			const key = auth.startsWith("Bearer ") ? auth.slice(7) : "";
			if (scenario === "cerberus-invalid-key" || key !== CERBERUS_KEY) {
				trace(401, "bad client key");
				return sendJson(res, 401, {
					error: "invalid_client",
					message: "Invalid or revoked API key",
				});
			}

			const build = String(req.headers["x-cerberus-build"] ?? "").split("/")[0];
			if (scenario === "cerberus-upgrade-required" || !build) {
				trace(426, `build ${build || "(absent)"}`);
				return sendJson(res, 426, {
					error: "upgrade_required",
					message: "Client version below the supported minimum",
					minimum: MIN_BUILD,
				});
			}

			if (scenario === "cerberus-rate-limited") {
				trace(429);
				return sendJson(res, 429, { error: "rate_limited", message: "Too many requests" });
			}
			if (scenario === "cerberus-credentials-invalid") {
				trace(503);
				return sendJson(res, 503, {
					error: "credentials_invalid",
					message: "The DWAB developer credential was rejected.",
				});
			}
			if (scenario === "cerberus-upstream") {
				trace(502);
				return sendJson(res, 502, {
					error: "upstream_error",
					message: "Could not reach the DWAB authorization server",
				});
			}

			const { token, ttl } = issueToken(scenario);
			trace(200, `token ttl=${ttl}s`);
			return sendJson(res, 200, { access_token: token, token_type: "Bearer", expires_in: ttl });
		}

		// --- TM REST API ------------------------------------------------------
		if (!url.pathname.startsWith("/api/")) {
			trace(404);
			return sendJson(res, 404, { error: "not_found" });
		}

		// TM only exposes /api/* once the Event Partner enables the Local TM API.
		// Before that it answers 503 -- verified against the reference client,
		// which maps exactly that status to "the API is not enabled". This mock
		// previously answered 404 here, which is a shape real TM does not send,
		// and the client's own test agreed with the mock rather than with TM.
		//
		// Neither status is an auth failure, and reporting it as one sends an
		// operator to re-copy a key that was never the problem.
		if (scenario === "api-disabled") {
			trace(503, "Local TM API not enabled");
			return sendJson(res, 503, { error: "service_unavailable" });
		}
		if (scenario === "api-disabled-404") {
			trace(404, "Local TM API not enabled (404 shape)");
			return sendJson(res, 404, { error: "not_found" });
		}

		// A drifted server clock rejects our x-tm-date and, crucially, advertises
		// its own time in the Date header. That header is the only reliable way a
		// client can tell "your clock is wrong" from "your API key is wrong",
		// since both otherwise present as a bare 401.
		if (scenario === "clock-skew") {
			const serverNow = new Date(Date.now() + SKEW_SCENARIO_OFFSET_SECONDS * 1000);
			trace(401, "clock_skew");
			res.writeHead(401, {
				"Content-Type": "application/json",
				Date: serverNow.toUTCString(),
			});
			return res.end(JSON.stringify({ error: "clock_skew", message: "x-tm-date is too far from server time" }));
		}

		const denial = authorise(req, scenario);
		if (denial) {
			trace(denial.status, denial.code);
			return sendJson(res, denial.status, { error: denial.code, message: denial.message });
		}

		const body = routeApi(url.pathname);
		if (body === null) {
			trace(404, "no such resource");
			return sendJson(res, 404, { error: "not_found" });
		}

		// Conditional GET. The guide tells clients to poll at most once a minute
		// and to honour Last-Modified, so the mock rewards a correct client with
		// a 304 and punishes a lazy one with a full body.
		const ims = req.headers["if-modified-since"];
		if (ims && !Number.isNaN(Date.parse(ims)) && Date.parse(ims) >= LAST_MODIFIED.getTime()) {
			trace(304);
			res.writeHead(304, { "Last-Modified": LAST_MODIFIED.toUTCString() });
			return res.end();
		}

		trace(200);
		return sendJson(res, 200, body, { "Last-Modified": LAST_MODIFIED.toUTCString() });
	});

	// --- Field set websocket ------------------------------------------------
	//
	// ws://{server}/api/fieldsets/{id}, upgraded with the same three auth
	// headers as a REST call. The upgrade is authorised through exactly the same
	// authorise() used above, so a client that can sign a GET can open a socket
	// and a client that cannot, cannot.

	const clients = new Set();
	const commands = []; // every {cmd:...} the client sent, in order
	const connections = []; // one entry per accepted upgrade
	const clientWaiters = [];

	function broadcast(payload) {
		const frame = encodeFrame(typeof payload === "string" ? payload : JSON.stringify(payload));
		for (const client of clients) client.write(frame);
		return clients.size;
	}

	server.on("upgrade", (req, socket) => {
		const url = new URL(req.url, `http://${req.headers["host"] ?? "localhost"}`);
		requests.push({ method: req.method, path: url.pathname, headers: { ...req.headers }, upgrade: true });

		const trace = (status, note) => {
			if (log) console.log(`  UPGRADE ${url.pathname} -> ${status}${note ? ` (${note})` : ""}`);
		};

		// No response object on an upgrade, so the status line is written by hand.
		// end() rather than destroy(): destroying can discard the bytes we just
		// wrote, and then the client sees a dropped connection instead of the
		// status code that explains why it was refused.
		const refuse = (status, text, note) => {
			trace(status, note);
			socket.end(`HTTP/1.1 ${status} ${text}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
		};

		const match = url.pathname.match(/^\/api\/fieldsets\/(\d+)$/);
		if (!match) return refuse(404, "Not Found", "no such field set socket");

		const fieldSetId = Number(match[1]);
		if (!FIELDS[fieldSetId]) return refuse(404, "Not Found", "unknown field set");

		if (scenario === "api-disabled") return refuse(503, "Service Unavailable", "Local TM API not enabled");
		if (scenario === "api-disabled-404") return refuse(404, "Not Found", "Local TM API not enabled");
		if (scenario === "clock-skew") return refuse(401, "Unauthorized", "clock_skew");

		const denial = authorise(req, scenario);
		if (denial) return refuse(denial.status, "Unauthorized", denial.code);

		const key = req.headers["sec-websocket-key"];
		if (!key) return refuse(400, "Bad Request", "no Sec-WebSocket-Key");

		socket.write(
			"HTTP/1.1 101 Switching Protocols\r\n" +
				"Upgrade: websocket\r\n" +
				"Connection: Upgrade\r\n" +
				`Sec-WebSocket-Accept: ${websocketAccept(key)}\r\n\r\n`,
		);
		socket.setNoDelay(true);
		trace(101, `field set ${fieldSetId}`);

		clients.add(socket);
		connections.push({ fieldSetId, at: Date.now() });

		const feed = createFrameDecoder({
			message(text) {
				let parsed;
				try {
					parsed = JSON.parse(text);
				} catch {
					if (log) console.log(`  ws <- unparseable: ${text}`);
					return;
				}
				if (log) console.log(`  ws <- ${text}`);
				commands.push(parsed);

				// Real TM announces the change to every listener rather than
				// answering the sender privately, so the plugin learns about a
				// display change it did not make. Mirror that.
				if (parsed.cmd === "setAudienceDisplay" && parsed.display) {
					broadcast({ type: "audienceDisplayChanged", display: parsed.display });
				}
			},
			ping(payload) {
				socket.write(encodeFrame(payload, OPCODE.PONG));
			},
			close() {
				socket.write(encodeFrame(Buffer.alloc(0), OPCODE.CLOSE));
				socket.end();
			},
			protocolError(reason) {
				if (log) console.log(`  ws protocol error: ${reason}`);
				socket.destroy();
			},
		});

		socket.on("data", feed);
		socket.on("error", () => clients.delete(socket));
		socket.on("close", () => clients.delete(socket));

		for (const resolve of clientWaiters.splice(0)) resolve();
	});

	const fieldset = {
		get clientCount() {
			return clients.size;
		},

		/// Every command the client has sent, oldest first.
		commands,

		/// One entry per accepted upgrade. Length > 1 means the client reconnected.
		connections,

		/// Pushes one event to every connected client. Returns how many got it.
		emit(event) {
			return broadcast(event);
		},

		/// Yanks the TCP connection with no close frame — what a TM restart, a
		/// yanked network cable, or a sleeping laptop looks like. This is the
		/// case reconnect logic actually has to survive.
		dropAll() {
			for (const client of clients) client.destroy();
			clients.clear();
		},

		/// Polite shutdown with a close frame.
		closeAll() {
			for (const client of clients) {
				client.write(encodeFrame(Buffer.alloc(0), OPCODE.CLOSE));
				client.end();
			}
			clients.clear();
		},

		/// Resolves once a client is attached. Resolves immediately if one
		/// already is, so there is no race between connecting and waiting.
		waitForClient(timeoutMs = 10000) {
			if (clients.size > 0) return Promise.resolve();
			return new Promise((resolve, reject) => {
				const timer = setTimeout(
					() => reject(new Error(`no websocket client after ${timeoutMs}ms`)),
					timeoutMs,
				);
				clientWaiters.push(() => {
					clearTimeout(timer);
					resolve();
				});
			});
		},
	};

	return { server, requests, fieldset };
}

/// The event sequence one match produces, in order. Used by the --cycle CLI
/// option and by the socket integration test, so both exercise the same script.
export function matchCycle({ fieldId = 1, matchNumber = 1, round = "QUAL", division = 1 } = {}) {
	return [
		{
			type: "fieldMatchAssigned",
			fieldID: fieldId,
			match: { division, session: 0, round, match: matchNumber, instance: 1 },
		},
		{ type: "fieldActivated", fieldID: fieldId },
		{ type: "audienceDisplayChanged", display: "INTRO" },
		{ type: "matchStarted", fieldID: fieldId },
		{ type: "audienceDisplayChanged", display: "IN_MATCH" },
		{ type: "matchStopped", fieldID: fieldId },
		{ type: "audienceDisplayChanged", display: "RESULTS" },
	];
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

function parseArgs(argv) {
	const args = { port: 8080, scenario: "ok", cycle: 0 };
	for (let i = 0; i < argv.length; i++) {
		if (argv[i] === "--port") args.port = Number(argv[++i]);
		else if (argv[i] === "--scenario") args.scenario = argv[++i];
		else if (argv[i] === "--cycle") args.cycle = Number(argv[++i] ?? 20);
		else if (argv[i] === "--help") args.help = true;
	}
	return args;
}

const isMain = process.argv[1] && import.meta.url.endsWith(process.argv[1].replace(/\\/g, "/").split("/").pop());

if (isMain) {
	const args = parseArgs(process.argv.slice(2));

	if (args.help) {
		console.log(
			"Usage: node test/mock-tm-server.mjs [--port N] [--scenario NAME] [--cycle SECONDS]\n\n" +
				"  --cycle SECONDS  replay a full match event sequence every SECONDS, so OBS can\n" +
				"                   be driven by hand without a real event running\n\nScenarios:",
		);
		for (const [name, description] of Object.entries(SCENARIOS)) {
			console.log(`  ${name.padEnd(14)} ${description}`);
		}
		process.exit(0);
	}

	const { server, fieldset } = createMockServer({ scenario: args.scenario });
	server.listen(args.port, () => {
		console.log(`Mock TM API on http://127.0.0.1:${args.port}  scenario=${args.scenario}`);
		console.log(`  sign-in key   ${CERBERUS_KEY}`);
		console.log(`  TM API key    ${API_KEY}`);
		console.log(`  build floor   ${MIN_BUILD}`);
		console.log(`  field set ws  ws://127.0.0.1:${args.port}/api/fieldsets/1`);
	});

	if (args.cycle > 0) {
		let matchNumber = 1;
		const runCycle = async () => {
			if (fieldset.clientCount === 0) return;
			console.log(`  -- match cycle: QUAL ${matchNumber}`);
			for (const event of matchCycle({ matchNumber })) {
				fieldset.emit(event);
				await new Promise((resolve) => setTimeout(resolve, (args.cycle * 1000) / 8));
			}
			matchNumber++;
		};
		setInterval(runCycle, args.cycle * 1000);
	}
}
