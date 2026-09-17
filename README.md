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

1. In Tournament Manager: **Tools → Options → Web Publishing → Enable Local TM
   API**, and copy the API key. TM regenerates it every event.
2. In OBS: open the **TM Director** dock, enter TM's address and that key, press
   **Test connection**, pick your field set.
3. Map each field to a scene and press **Save**.

**[docs/operator-guide.md](docs/operator-guide.md)** is the full version: how the
scene fallback works, what delay and hold actually do, how to read the Live panel
mid-event, and a table of every failure with who can fix it. That is the one to
hand to whoever is running the stream.

## Two things worth knowing before an event

**Signing in needs the internet.** TM itself is LAN-only, but the bearer token
comes from the club's sign-in service, which does not run at the venue. A laptop with
no uplink can reach TM and still not be able to automate anything. Check this
during setup, not during the first match.

**The TM API key is stored in plain text** in the plugin's config file
(`%APPDATA%\obs-studio\plugin_config\tm-obs-director\tm-director.json`). It is
per-event and only useful on the venue LAN, which is why this is acceptable — but
it is not a secret this file protects.

## Building

Requires CMake 3.28+, and on Windows Visual Studio 2022 (Build Tools are enough).
Dependencies are fetched by the project itself; nothing needs installing first.

Put the sign-in key in a file named `cerberus-key.txt` in the project root — one
line, nothing else. Then:

```bash
cmake --preset windows-x64
cmake --build --preset windows-x64
ctest --preset windows-x64
bash build-aux/deploy-windows.sh
```

The key is issued **per build**, is never committed, and `.gitignore` already
excludes the file. Configure prints only its length and where it came from, never
the key itself, so building in a shared terminal or a recorded session does not
leak it. `-DTMD_CERBERUS_KEY=` and the `TMD_CERBERUS_KEY` environment variable
also work and take precedence, but both put the key somewhere that gets echoed —
a command line, shell history, a CI log — so the file is the better default.

A build without a key compiles and runs and says plainly that it cannot sign in,
rather than failing at a venue as a 401 indistinguishable from a revoked key.

Anyone holding the built DLL can read the key out of it. That is inherent to
baking one in, and is why Cerberus issues keys per build and can revoke them —
but it means a build is as shareable as the key inside it.

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

The TM client follows [`tm-client`][tm-client], the canonical implementation of
this API, as a specification — the two cannot share code, since one is
TypeScript and this has to stay a single DLL with no Node on the venue laptop.
[docs/tm-client-parity.md](docs/tm-client-parity.md) records what matches, what
deliberately does not, and which version was checked.

[tm-client]: https://github.com/UNSW-Robotics-Competition-Club/tm-client

`src/tm` and `src/engine` are deliberately free of libobs and Qt. That is what
makes the awkward parts — signing, reconnection, and the timing rules — testable
without a running OBS.

## Licence

GPL-2.0-or-later. See [LICENSE](LICENSE).
