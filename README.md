# TM Director

An OBS Studio plugin that switches scenes from VEX Tournament Manager.

TM already broadcasts what a stream operator is watching for: a match queued to a
field, a field activated, a timer started, a timer stopped. TM Director subscribes
to that feed and maps each of those moments to a scene, so the switching that an
operator otherwise does by hand happens on its own.

- **Switches on** `fieldMatchAssigned`, `fieldActivated`, `matchStarted`,
  `matchStopped` — one field set, any number of fields.
- **Per field**: one scene, with a per-trigger override where the field's camera
  is not the whole answer.
- **Timing**: a delay before switching, and a minimum hold afterwards, so a
  stream does not lurch through four scenes in the seconds around a match start.
- **Automation on/off**, prominently, so an operator can take manual control
  mid-match without dismantling anything.

## Setting it up at an event

1. **In Tournament Manager**, open *Tools → Options → Web Publishing* and tick
   **Enable Local TM API**. Copy the API key shown there.

   > TM generates a **new key for every event**. A key that worked last weekend
   > will not work this one, and TM reports the mismatch as a plain 401.

2. **In OBS**, open the **TM Director** dock.
3. Enter TM's address (`http://127.0.0.1` when OBS runs on the TM machine, or the
   TM machine's LAN address otherwise) and paste the API key.
4. Press **Test connection**. On success the field set list fills in; pick the one
   this stream covers.
5. Map each field to a scene. Leave a field on *(no switch)* to exclude it.
6. Press **Save**. The status line and activity log show what the plugin is doing.

### When something is wrong

Every failure names its own fix in the dock rather than reporting a generic
"connection failed", because the four things that go wrong need four different
responses:

| What it says | What to do |
|---|---|
| The API key was rejected | Re-copy it from *this* event's Web Publishing options |
| The API is switched off | Tick Enable Local TM API in TM |
| Could not reach TM | Check the address, the port, and that both machines are on the same network |
| The clock is too far out | Turn on automatic time sync — TM signs requests with a timestamp |
| Could not reach sign-in | This laptop has no internet route. TM is local, but the token is not |
| This build's key was rejected | The plugin build needs replacing; nothing typed in the dock will help |

## Two things worth knowing before an event

**Signing in needs the internet.** TM itself is LAN-only, but the bearer token
comes from SRA's sign-in service, which does not run at the venue. A laptop with
no uplink can reach TM and still not be able to automate anything. Check this
during setup, not during the first match.

**The TM API key is stored in plain text** in the plugin's config file
(`%APPDATA%\obs-studio\plugin_config\obs-tm-director\tm-director.json`). It is
per-event and only useful on the venue LAN, which is why this is acceptable — but
it is not a secret this file protects.

## Building

Requires CMake 3.28+, and on Windows Visual Studio 2022 (Build Tools are enough).
Dependencies are fetched by the project itself; nothing needs installing first.

```bash
cmake --preset windows-x64 -DTMD_CERBERUS_KEY=ctm_...
cmake --build --preset windows-x64
ctest --preset windows-x64
bash build-aux/deploy-windows.sh
```

`TMD_CERBERUS_KEY` is the sign-in key, issued **per build** and never committed.
A build without one compiles and runs and says plainly that it cannot sign in,
rather than failing at a venue as a 401 indistinguishable from a revoked key.

> On Windows, OBS loads plugins from `%ProgramData%\obs-studio\plugins`, **not**
> from `%APPDATA%`. A plugin in the wrong one is not rejected — it is never
> looked at. `build-aux/deploy-windows.sh` puts it in the right place.
>
> OBS logs every module load failure at debug level, so a plugin it refuses
> leaves no trace in an ordinary log. Run `obs64.exe --verbose` to see why.

## Testing without Tournament Manager

A mock TM server ships with the tests. It enforces the real signing rules, so a
client that satisfies it satisfies real TM, and it can replay a whole match on a
timer:

```bash
node test/mock-tm-server.mjs --port 8080 --cycle 20
```

Point the dock at `http://127.0.0.1:8080` with the API key the mock prints on
startup. `--scenario` forces a specific failure — run
`node test/mock-tm-server.mjs --help` for the list — which is how the dock's
error messages are checked without having to break a real event.

## Layout

| Path | What lives there |
|---|---|
| `src/tm/` | The TM Public API client: request signing, HTTP, the field set websocket, event decoding |
| `src/engine/` | The decisions: inferred match state, scene rules, switch timing |
| `src/ui/` | The only code that touches libobs or Qt |
| `test/` | Unit tests, the mock TM server, and the runners that pair them |

`src/tm` and `src/engine` are deliberately free of libobs and Qt. That is what
makes the awkward parts — signing, reconnection, and the timing rules — testable
without a running OBS.

## Licence

GPL-2.0-or-later. See [LICENSE](LICENSE).
