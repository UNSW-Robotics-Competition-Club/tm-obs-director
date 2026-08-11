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
import { createHmac, timingSafeEqual } from "node:crypto";
import { sign } from "./tm-sign.mjs";

// ---------------------------------------------------------------------------
// Fixed credentials. Test fixtures, not secrets: the real ones are per-event
// and never live in this repo.
// ---------------------------------------------------------------------------

export const CLIENT_ID = "mock-client-id";
export const CLIENT_SECRET = "mock-client-secret";
export const API_KEY = "TESTAPIKEY0123456789";

const TOKEN_TTL_SECONDS = 7200; // real TM tokens last ~2h
const MAX_CLOCK_SKEW_SECONDS = 300;

// ---------------------------------------------------------------------------
// Scenarios: force a specific failure so the client's error mapping is testable.
// ---------------------------------------------------------------------------

const SCENARIOS = {
	ok: "Everything works.",
	"api-disabled": "Event Partner never ticked Enable Local TM API: /api/* is 404.",
	"bad-signature": "Every signature is rejected, as with a wrong TM API key.",
	"token-expired": "Tokens are issued already expired, forcing a refresh path.",
	unauthorized: "The OAuth token endpoint rejects the client credentials.",
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

		// --- OAuth token endpoint ---------------------------------------------
		if (url.pathname === "/oauth2/token") {
			if (req.method !== "POST") {
				trace(405);
				return sendJson(res, 405, { error: "method_not_allowed" });
			}
			const params = new URLSearchParams(await readBody(req));

			if (params.get("grant_type") !== "client_credentials") {
				trace(400, "grant_type");
				return sendJson(res, 400, { error: "unsupported_grant_type" });
			}
			if (
				scenario === "unauthorized" ||
				params.get("client_id") !== CLIENT_ID ||
				params.get("client_secret") !== CLIENT_SECRET
			) {
				trace(401, "bad client credentials");
				return sendJson(res, 401, { error: "invalid_client" });
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
		// Before that the whole namespace is simply absent -- which is why an
		// unauthenticated 404 here must not be reported as an auth failure.
		if (scenario === "api-disabled") {
			trace(404, "Local TM API not enabled");
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

	return { server, requests };
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

function parseArgs(argv) {
	const args = { port: 8080, scenario: "ok" };
	for (let i = 0; i < argv.length; i++) {
		if (argv[i] === "--port") args.port = Number(argv[++i]);
		else if (argv[i] === "--scenario") args.scenario = argv[++i];
		else if (argv[i] === "--help") args.help = true;
	}
	return args;
}

const isMain = process.argv[1] && import.meta.url.endsWith(process.argv[1].replace(/\\/g, "/").split("/").pop());

if (isMain) {
	const args = parseArgs(process.argv.slice(2));

	if (args.help) {
		console.log("Usage: node test/mock-tm-server.mjs [--port N] [--scenario NAME]\n\nScenarios:");
		for (const [name, description] of Object.entries(SCENARIOS)) {
			console.log(`  ${name.padEnd(14)} ${description}`);
		}
		process.exit(0);
	}

	const { server } = createMockServer({ scenario: args.scenario });
	server.listen(args.port, () => {
		console.log(`Mock TM API on http://127.0.0.1:${args.port}  scenario=${args.scenario}`);
		console.log(`  client id     ${CLIENT_ID}`);
		console.log(`  client secret ${CLIENT_SECRET}`);
		console.log(`  TM API key    ${API_KEY}`);
	});
}
