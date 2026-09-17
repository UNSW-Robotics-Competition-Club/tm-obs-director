# Parity with `tm-client`

[`@unsw-robotics-competition-club/tm-client`][tm-client] is the canonical
implementation of the VEX TM Public API. This plugin is the C++ one its README
refers to. They cannot share code — tm-client is TypeScript/ESM with no native
binding, and this is an OBS plugin that has to stay a single DLL with no Node on
the venue laptop — so tm-client is treated as the **specification** and this file
records where the two agree, where they deliberately do not, and what was checked.

Checked against tm-client **0.1.0**. Re-check this file when that version moves.

[tm-client]: https://github.com/UNSW-Robotics-Competition-Club/tm-client

## Shared behaviour

| Behaviour | Status |
|---|---|
| Five-line canonical signing string, trailing newline included | Same |
| `x-tm-date` / `x-tm-signature` / `Authorization` header set | Same |
| Cerberus token refresh margin (5 minutes) | Same |
| Reconnect ladder starting at 1s, capped at 30s | Same |
| `fieldSets` envelope key with a capital S on `/api/fieldsets` | Same |
| 503 and 404 from `/api/*` both mean the Local TM API is off | Same |
| A timeout is an assignment with an empty match **or** round `TIMEOUT` | Same |
| A 401 drops the cached bearer | Same (see below) |

## Bugs tm-client fixes, and whether this had them

tm-client's changelog names two bugs in the deleted upstream `vex-tm-client`.

**A 401 left a rejected bearer cached.** This had it too, and worse: the method
to drop the token existed and was documented, and was never once called, so a
bearer TM had already refused went on being sent until it expired. Fixed —
`getRaw` now invalidates on `InvalidSignature` and `TokenExpired`.

**`ensureBearer()` had no single-flight guard.** Not reachable here. Every call
into the API client goes through one mutex in `Director`, because the field set
socket thread and the dock's worker thread share it, so two cold-start mints
cannot overlap. The guard exists for a different reason and covers this case.

tm-client also types `fieldID` as `number | null` because upstream's types
promised a number and the runtime checked for null anyway. This parses it with
`intField`, which returns 0 for a JSON null, and 0 is already "no field"
throughout the engine — safe by construction rather than by having noticed.

## Deliberate differences

**Queued commands are dropped on reconnect, not replayed.** tm-client keeps an
outbox across a reconnect. Every field set command is time-critical, and firing a
stale `start` at a match that already began is worse than losing it. Moot in
practice: this plugin only ever reads, and sends no commands at all.

**The error taxonomy is coarser in two places.** tm-client splits
`dwab_invalid_client` / `dwab_credentials_expired` / `cerberus_credentials_invalid`
where this has one `InvalidClientCredentials`, and splits `cerberus_upstream_error`
from `dwab_unreachable` where this has one `AuthHostUnreachable`. The split codes
have identical remedies — the operator can do nothing about any of them, and the
message says which it was. A code earns its place here by changing what someone
does about it.

**Conditional GET is plumbed but unused.** `getRaw` accepts an
`If-Modified-Since` and reports `Last-Modified`, and nothing passes one. The
plugin makes a handful of REST calls per event — the field set list, the field
list — and reads everything else off the websocket, so there is nothing to poll
and nothing to cache. tm-client is a general client and needs it.

Because nothing here sends `If-Modified-Since`, the second half of tm-client's
401 fix — invalidating that URL's cached entry so a stale body cannot come back
via a 304 issued against a dead token — has nothing to invalidate. Worth
remembering if conditional GET is ever switched on.

## Not applicable

Rankings (this reads no ranking data), the websocket transport abstraction (this
uses libcurl's, and cannot run in a browser), and async WebCrypto signing (this
signs with its own SHA-256, checked against FIPS 180-4 and RFC 4231 vectors).
