# Communication protocol – firmware 3.11

The controller and the app talk over HTTP/1.1 on port 80 of the home network. The app is the page at `/`; it polls `GET /api/status` every 2 s and sends actions as `POST` requests. Alerts go from the controller to an ntfy server over HTTPS. Nothing else leaves the board except NTP and mDNS.

## Transport and access

| Item | Rule |
|---|---|
| Address | `http://reusewater.local` (mDNS) or the IP shown on the screen |
| Sign-in (3.11) | `POST /api/login` sets the cookie `gwsid` (`HttpOnly; SameSite=Strict`, `Max-Age=34560000` = 400 days with `remember=1`, a browser-session cookie with `remember=0`). Its value is `<role a/v><remember 1/0><16 hex nonce>.<32 hex HMAC>`, signed with HMAC-SHA256 under the 32-byte key `authkey` in flash; the signature covers the role's password (`AuthToken.h`). A password change invalidates that role's cookies; `POST /api/logout` with `all=1` makes a new key. `GET /` renews a remembered cookie. Without a valid cookie or Basic header, `/api/*` answers `401` `Sign in to continue.` **without** `WWW-Authenticate`, so browsers do not show their own password box; `GET /` is public |
| Authentication | HTTP Basic (still accepted, for tools). User `admin` with the dashboard password (8–32 characters): everything. User `viewer` with the viewer password (3.5; off while none is set): only `GET /`, `GET /api/status` and `GET /api/history` (without `settings`); every other request answers `403` *The viewer user can only look. Sign in as admin to make changes.* |
| Wrong passwords | 5 within a minute from one IP address → that address gets `429` for 1 minute (per device from 3.0) |
| Actions | Every `POST` also needs the header `X-GW: 1`, else `403`. Other websites cannot send it, so they cannot press buttons with a remembered password |
| Caching | All API replies carry `Cache-Control: no-store` |
| Page | `/` is sent with `Content-Encoding: gzip` (3.0) |
| Encryption | None on the LAN (plain HTTP). Keep the controller on the home network; do not forward ports |

## Endpoints

| Method | Path | Body (form fields) | Success | Errors |
|---|---|---|---|---|
| GET | `/` | — | page | 401, 429 |
| GET | `/api/status` | — | status JSON (below) | 401, 429 |
| GET | `/api/settings` | — | settings JSON (no passwords; `viewerOn` says whether the viewer user is on, 3.5) | 401, 403 (viewer), 429 |
| POST | `/api/settings` | any of: `washPeriodS`, `washOpenS`, `drain2Min`, `edgeS`, `stepAlertMin`, `manualWashMaxMin`, `tankMaxAgeH`, `minFillLitres`, `flowOnLpm`, `saveMinLpm` (0.5–10, 3.4), `buzzerOn` (0/1), `allowRemoteStart` (0/1), `language` (`es` / `en`, 3.1), `screenFont` (0–3: Hyperlegible, Plain sans, Terminal bold, Schoolbook bold; 3.6), `ntfyServer`, `ntfyTopic`, `timeZone`, `webPassword`, `otaPassword`, `viewerOn` (0 turns the viewer off and forgets its password; 1 needs a password, 3.5), `viewerPassword` (8–32, not the dashboard password, 3.5) | the saved settings JSON | 400 (message says which field), 403 (viewer), 500 (flash write failed) |
| POST | `/api/start` | optional `save`: `1` *Reuse and save* (the default), `0` *Reuse only*, step 4 sends the rinse water to the drain (3.7) | `{"ok":true}` = request accepted | 400 (`save` not 0 or 1), 403 (turned off in Settings, or viewer), 409 (not Ready) |
| POST | `/api/stop` | — | `{"ok":true}` = request accepted | 409 `No cycle is running.` (3.0) |
| POST | `/api/wash` | — | `{"ok":true}` = started | 409 (cycle running, or tank already full) |
| POST | `/api/wash/stop` | — | `{"ok":true}` | — |
| POST | `/api/test-alert` | — | `{"ok":true}` = queued | 400 (no topic) |
| POST | `/api/restart` | — | `{"ok":true}`, then restart | 409 (cycle running) |
| GET | `/api/history` | — | history JSON + `settings` (`null` for the viewer, 3.5) | — |
| POST | `/api/history/restore` | history fields, `force=1` to replace | `{"ok":true}` | 400 (invalid), 409 (history exists) |
| GET | `/api/events` | — | array of `{epoch, uptimeS, text}`, newest first, up to 50 | 403 (viewer) |
| POST | `/api/interrupted/dismiss` | — | `{"ok":true}` | — |
| POST | `/api/login` | `user`, `password`, `remember` (`1`/`0`, default `1`); needs `X-GW: 1` | `{"ok":true,"role":"admin"}` or `viewer`, plus `Set-Cookie` (3.11) | 401 `Wrong user name or password.`, 403 (no `X-GW`), 429 (5 wrong in a minute from this device) |
| POST | `/api/logout` | optional `all=1` (admin only): sign out every other device; needs `X-GW: 1` | `{"ok":true}`; clears the cookie, or with `all=1` sends a new one for this device (3.11) | 401/403 (`all=1` without admin) |
| POST | `/api/interrupted/resume` | — | `{"ok":true}` = request accepted; the cycle continues at the start of the saved step on the next `loop()` pass (3.10) | 403 (*Start from the dashboard* off, or viewer), 409 `The cycle can no longer be resumed.` (no complete snapshot, more than 30 min after power-up, snapshot over 4 h old, not Ready, tank wash running, dismissed, or a cycle started) |

Errors are JSON: `{"error":"<sentence for the user>"}`, always in English; the app translates them (`web/es.json`). Settings values outside their ranges are clamped (README, *Settings*), not refused, except the rules that return 400.

### Accepted is not done

`/api/start` and `/api/stop` only set a request flag; the cycle acts on it at the start of the next `loop()` pass. The app therefore:

1. keeps the button in *Starting…* / *Stopping…* after `200`,
2. shows *Cycle started* / *Cycle stopped* only when a later status shows `state` ≠ 0 / `state` = 0,
3. after 10 s without that, shows an error and re-enables the buttons.

The tank wash and the test alert follow the same rule (`manualWash` in the status; `alert.seq` / `alert.ok`).

### Duplicate and conflicting commands

- Start while not Ready → `409`; a second Start before the first is applied is harmless (the request flag is a boolean).
- Stop while Ready → `409` (3.0). STOP on the controller always works and needs no network.
- Requests are handled one at a time in `loop()`, so two phones cannot interleave an action.

## Status JSON (`GET /api/status`)

| Field | Type | Meaning |
|---|---|---|
| `fw` | string | Firmware version, `3.2.0` |
| `lang` | `es` / `en` | Language of the screen, alerts and the app (3.1). `title`, `message` and `boot` come in this language |
| `state`, `stateName` | int, string | 0 Ready, 1 Starting, 2 Fill (wash), 3 Drain (soapy), 4 Rinse, 5 Save |
| `title`, `message` | string | Screen header and the longer status sentence |
| `tank` | `low` / `medium` / `high` | Debounced level; `high` whenever the high float is wet |
| `tankFault` | bool | High float wet, low float dry for 3 s (3.0) |
| `wash` | bool | Tank-wash valve output on |
| `outputs` | int | Pins as written: 1 tap, 2 drain, 4 fill, 8 pump, 16 tank wash |
| `tapLpm`, `drainLpm` | number | 2 s average flow, L/min |
| `sensors` | object | `tapPulses`, `drainPulses`, `tapFlowing`, `drainFlowing`, `flowOnLpm` |
| `stepElapsedS`, `drain2LeftS`, `stuck` | | Step timing; `stuck` = step 1–3 past the alert time |
| `fill` | object | `litres` taken in this fill step, `complete` (guard set), `minLitres` |
| `typicalS` | [3] | Typical duration of steps 1–3 on this machine, s (0 = not learned) |
| `manualWash`, `manualWashLeftS`, `manualWashMaxMin`, `manualWashResult` | | Manual wash state; result `full` / `timeout` / `stopped` / `cycle` |
| `tankAgeH`, `tankMaxAgeH` | number | Age of the oldest tank water (−1 empty/unknown), limit |
| `cycle`, `month`, `prevMonth`, `total`, `last` | objects | Water statistics, litres (`tap`, `reused`, `saved`, `cycles`); `cycle.noSave` is true while a *Reuse only* cycle runs (3.7) |
| `history` | object | `source` (where the history was loaded from), `hasData` |
| `interrupted` | object | `active`, `step` (0 = before step 1, else 1–4), `epoch` of the last cycle cut short (time of its last snapshot, 0 if the clock was not set); 3.10: `noSave` (it was *Reuse only*), `resume` (`/api/interrupted/resume` would be accepted now, ignoring the Settings switch), `resumeLeftS` (seconds left to resume, 0 when `resume` is false) |
| `boot` | string | Last restart reason |
| `updatesAllowed` | bool | Wireless updates accepted now (Ready, no tank wash) |
| `defaultPasswords` | bool | A password is still `greywater` (the app then asks for new ones); always `false` for the viewer (3.5) |
| `role` | `admin` / `viewer` | Who signed in (3.5). The app hides the controls and the Service and Settings tabs for `viewer` |
| `allowRemoteStart`, `alerts` | bool | Settings in effect |
| `screen` | bool | OLED answered at start-up (3.0) |
| `loop` | object | `maxMs` since start-up, `lastMinMaxMs` in the last full minute (3.0) |
| `heap` | object | `free`, `min` bytes (3.0) |
| `alert` | object | `seq` (results received), `ok`, `code`: HTTP status; −100 no Wi-Fi for 5 min; −101 bad server address; other negative values (HTTPClient −1 … −11): could not connect (3.0; −2 / −3 until 3.2.0) |
| `wifi`, `time`, `uptimeS` | | Signal (dBm), IP, local time `YYYY-MM-DD HH:MM`, seconds since start-up |

Fields are only added between versions, never renamed, so an older page keeps working with newer firmware.

## Timeouts and reconnection

| Where | Value |
|---|---|
| App, status request | 5 s, then counted as a failure; 2 failures → *Offline* badge; no update for 6 s → *Updating slowly* |
| App, action request | 8 s |
| App, confirmation of an action | 10 s |
| App, test-alert result | 120 s (75 s until 3.2.0) |
| Controller, Wi-Fi reconnect | every 60 s while disconnected (saved network) |
| Controller, alert sending | 10 s each for connect, TLS handshake and answer. No Wi-Fi, no answer, a timeout, 408, 429 or 5xx: tried again after 5 s, 10 s, 20 s … at most every 5 min, for up to 30 min (test alert: 30 s); up to 6 alerts wait at once, each on its own schedule, the oldest is given up when a 7th arrives; queue of 6, extra alerts are dropped and logged (3.3.1) |
| Controller, Wi-Fi setup page | closes after 3 min |

## Alerts (controller → ntfy)

From 3.1: `POST <ntfyServer>/` (the server root) with `Content-Type: application/json` and the body
`{"topic":"<topic>","title":"<title>","message":"<text>","priority":1-5,"tags":["<tag>"],"click":"http://<controller IP>/"}` (ntfy "publish as JSON"; `click` from 3.3). The body is built by `Ntfy.h`. The topic must be 1–63 characters from `A-Z a-z 0-9 - _` (checked by `POST /api/settings` from 3.3); the server address is stored without spaces, trailing slashes or a pasted topic. JSON carries UTF-8, so Spanish accents arrive intact; up to 3.0 the title went in an HTTP header, which only carries ASCII. HTTPS without certificate checking (alerts carry no secrets; the topic name is the only secret, so use a long random one). Results come back to the event log: *Alert delivered* (with *try n* after a retry), *Alert not sent (no Wi-Fi)*, *Alert not sent (bad server address)*, *Alert failed (code n, n tries)*.

## Wireless updates (ArduinoOTA)

Host name `reusewater`, update password from Settings (applied at once from 3.0). Accepted only while `updatesAllowed` is true. On start: watchdog paused, all outputs off, history saved, screen shows `UPDATING`.
