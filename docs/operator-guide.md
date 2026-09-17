# TM Director — operator guide

For whoever is running the stream. Everything here happens in the **TM Director**
dock inside OBS.

---

## What it does

Tournament Manager broadcasts four moments that matter to a stream:

| TM event | In the dock | When it happens |
|---|---|---|
| `fieldMatchAssigned` | **Queued** | A match is put on deck for a field |
| `fieldActivated` | **Activated** | That field becomes the one on the audience display |
| `matchStarted` | **Started** | The match timer starts |
| `matchStopped` | **Stopped** | The timer stops — at the end, or on an abort |

You map each to a scene. TM Director switches OBS when they arrive.

It only ever **switches scenes**. It never starts or stops your stream or
recording, and it never sends anything back to TM — it cannot start, abort or
queue a match by accident.

---

## Before the event

1. **Install the plugin.** Copy it to `C:\ProgramData\obs-studio\plugins\` (not
   `%APPDATA%` — OBS does not look there on Windows).
2. **Check the laptop has internet.** TM is on the local network, but signing in
   is not. A venue with no uplink can reach TM perfectly and still not be able to
   automate anything. Test this on setup day, not on the first match.
3. **Build your scenes** in OBS, one per camera or layout you intend to cut to.

---

## Setting up at the event

### In Tournament Manager

Open **Tools → Options → Web Publishing** and tick **Enable Local TM API**. Copy
the API key it shows.

> TM generates a **new key for every event**. Last weekend's key will be
> rejected, and the failure looks like a plain authentication error.

### In OBS

1. Open the **TM Director** dock (View → Docks if it is not visible).
2. **Address** — `http://127.0.0.1` if OBS is on the TM machine, otherwise the TM
   machine's address on the venue network.
3. **API key** — paste the key from above.
4. **Test connection.** On success the **Field set** list fills in. Pick the one
   this stream covers.
5. The fields for that set appear below as collapsible blocks.
6. Map your scenes (see next section), then press **Save**.

The **Live** section should now show `Link: Connected`.

---

## Mapping scenes

Each field is one block:

```
▼ Main Field
    Default scene   [ Field 1 Cam   ]
    Queued          [ (use default) ]
    Activated       [ (use default) ]
    Started         [ Field 1 Match ]
    Stopped         [ Replay        ]
    Delay [default]   Hold [default]
```

**Default scene** is the fallback. Any trigger set to *(use default)* uses it.

That is the whole hierarchy:

- Trigger has its own scene → that scene is used.
- Trigger is *(use default)* → the field's **Default scene** is used.
- Both empty → **nothing happens for that trigger.**

The last case is the one that catches people. A trigger showing *(use default)*
when **Default scene** is *(no switch)* does nothing at all, silently. If a
trigger seems to be ignored, check the default above it first.

The common venue setup is one camera per field: set **Default scene** once, leave
all four triggers on *(use default)*, and add an override only where the camera is
not the whole answer — match graphics on **Started**, a replay on **Stopped**.

Set a field's **Default scene** to *(no switch)* to exclude that field entirely
without deleting its settings.

---

## Timing: delay and hold

A match fires several events within seconds. Without timing controls a stream
lurches through four scenes around every match start.

**Delay** — wait this long before switching. Use it to line a cut up with what the
viewer is about to see rather than with the instant TM sent the event.

**Hold** — having switched, stay put at least this long.

Two rules worth knowing, because they are what stops the obvious problems:

- **A newer event replaces one still waiting out its delay.** If a match starts
  while the "queued" switch is still pending, you get the match — not the on-deck
  shot followed a second later by the match.
- **A hold delays a switch, it never cancels it.** A match starting one second
  after its field was activated still reaches the match scene; it just waits out
  the hold first.

Per-field **Delay** and **Hold** show *default* when they follow the **Default
delay** and **Default hold** at the top. Set them to override just that field.

---

## Running the event

The **Live** section is how you tell working from stuck:

- **Link** — `Connected` is what you want. `Reconnecting` with a reason means it
  is retrying on its own; it backs off and keeps trying, so a brief network blip
  recovers without intervention.
- **Match** — what TM says is on the field, e.g. `Q12 - Running on field 1`.
- **Last switch** — the scene the automation most recently cut to.
- **The log** — every event received and what was done about it, including
  `- no rule` when nothing was mapped.

**Automation on** is the switch to reach for when you need to take manual control
mid-match. It stops all switching and changes none of your mapping, so you can
turn it straight back on.

**Collapse all** folds every field down to its name, for events with more fields
than fit on screen.

The same link state also goes to OBS's own log, so it can be read after the event
from `%APPDATA%\obs-studio\logs\`.

---

## When something is wrong

Each failure names its own fix. The distinction matters because most of these are
not fixable by the person standing at the laptop:

| The dock says | What is actually wrong | Who fixes it |
|---|---|---|
| The TM API key was rejected | Wrong or stale key — it is regenerated per event | You: re-copy it |
| Tournament Manager's API is switched off | Enable Local TM API was never ticked | You, or the Event Partner |
| Could not reach Tournament Manager | Wrong address or port, or a different network | You |
| The clock is too far out | The two machines disagree on the time; TM signs requests with a timestamp | You: turn on automatic time sync |
| Could not reach the sign-in service | This laptop has no internet route | The venue's network |
| This build's sign-in key was rejected | The plugin build needs replacing | SRA — nothing in the dock will help |
| This plugin build is too old | Same | SRA |
| Sign-in failed on the SRA side | SRA's own credential expired or was rejected | SRA — run manually meanwhile |

A scene named in your mapping that no longer exists in OBS is kept in the picker
and marked **(missing)**, and a switch to it is logged as doing nothing rather
than failing quietly. Rename a scene and the mapping will say so.

---

## Known limits

These are properties of TM's API, not things left unfinished:

- **Timeouts cannot be started from here.** TM reports that one is running but
  offers no command to start or stop one.
- **A stop is a stop.** TM sends the same event whether a match ended normally or
  was aborted, so **Stopped** fires for both.
- **Skills runs are not split** into driver and programming. TM reports a skills
  run without saying which.
- **No match timer.** TM never sends the clock, so the dock cannot show a
  countdown.
- **One field set per OBS instance.** An event streaming two divisions separately
  needs an OBS instance per stream.
- **The audience display is not a trigger.** It changes whenever anyone touches
  it, including in response to our own switching, so wiring it to scenes would
  risk a loop.

---

## Testing without Tournament Manager

A mock TM server ships with the plugin's source and can replay a whole match on a
timer, which is the cheapest way to rehearse a layout:

```bash
node test/mock-tm-server.mjs --port 8080 --cycle 20
```

Point the dock at `http://127.0.0.1:8080` using the API key the mock prints when
it starts. It enforces the real signing rules, so anything that works against it
works against TM.

`node test/mock-tm-server.mjs --help` lists scenarios that force a specific
failure, if you want to see what each error above looks like before meeting it at
an event.

---

## Where things are kept

| What | Where |
|---|---|
| The plugin | `C:\ProgramData\obs-studio\plugins\obs-tm-director\` |
| Your settings | `%APPDATA%\obs-studio\plugin_config\obs-tm-director\tm-director.json` |
| OBS's log | `%APPDATA%\obs-studio\logs\` |

The settings file holds the TM API key in plain text. It is per-event and only
useful on the venue network, but it is not a secret that file protects.
