# Communication protocol – firmware 3.1

The controller and the app talk over HTTP/1.1 on port 80 of the home network. The app is the page at `/`; it polls `GET /api/status` every 2 s and sends actions as `POST` requests. Alerts go from the controller to an ntfy server over HTTPS. Nothing else leaves the board except NTP and mDNS.

## Transport and access

| Item | Rule |
|---|---|
| Address | `http://reusewater.local` (mDNS) or the IP shown on the screen |
| Authentication | HTTP Basic, user `admin`, the dashboard password (8–32 characters) |
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
| GET | `/api/settings` | — | settings JSON (no passwords) | 401, 429 |
| POST | `/api/settings` | any of: `washPeriodS`, `washOpenS`, `drain2Min`, `edgeS`, `stepAlertMin`, `manualWashMaxMin`, `tankMaxAgeH`, `minFillLitres`, `flowOnLpm`, `buzzerOn` (0/1), `allowRemoteStart` (0/1), `language` (`es` / `en`, 3.1), `ntfyServer`, `ntfyTopic`, `timeZone`, `webPassword`, `otaPassword` | the saved settings JSON | 400 (message says which field), 500 (flash write failed) |
| POST | `/api/start` | — | `{"ok":true}` = request accepted | 403 (turned off in Settings), 409 (not Ready) |
| POST | `/api/stop` | — | `{"ok":true}` = request accepted | 409 `No cycle is running.` (3.0) |
| POST | `/api/wash` | — | `{"ok":true}` = started | 409 (cycle running, or tank already full) |
| POST | `/api/wash/stop` | — | `{"ok":true}` | — |
| POST | `/api/test-alert` | — | `{"ok":true}` = queued | 400 (no topic) |
| POST | `/api/restart` | — | `{"ok":true}`, then restart | 409 (cycle running) |
| GET | `/api/history` | — | history JSON + `settings` | — |
| POST | `/api/history/restore` | history fields, `force=1` to replace | `{"ok":true}` | 400 (invalid), 409 (history exists) |
| GET | `/api/events` | — | array of `{epoch, uptimeS, text}`, newest first, up to 50 | — |
| POST | `/api/interrupted/dismiss` | — | `{"ok":true}` | — |

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
| `fw` | string | Firmware version, `3.1.0` |
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
| `cycle`, `month`, `prevMonth`, `total`, `last` | objects | Water statistics, litres (`tap`, `reused`, `saved`, `cycles`) |
| `history` | object | `source` (where the history was loaded from), `hasData` |
| `interrupted` | object | `active`, `step` (0 = before step 1, else 1–4), `epoch` of the last cycle cut short |
| `boot` | string | Last restart reason |
| `updatesAllowed` | bool | Wireless updates accepted now (Ready, no tank wash) |
| `defaultPasswords` | bool | A password is still `greywater` (the app then asks for new ones) |
| `allowRemoteStart`, `alerts` | bool | Settings in effect |
| `screen` | bool | OLED answered at start-up (3.0) |
| `loop` | object | `maxMs` since start-up, `lastMinMaxMs` in the last full minute (3.0) |
| `heap` | object | `free`, `min` bytes (3.0) |
| `alert` | object | `seq` (results received), `ok`, `code`: HTTP status; −2 no Wi-Fi for 60 s; −3 bad server address; other negative values: could not connect (3.0) |
| `wifi`, `time`, `uptimeS` | | Signal (dBm), IP, local time `YYYY-MM-DD HH:MM`, seconds since start-up |

Fields are only added between versions, never renamed, so an older page keeps working with newer firmware.

## Timeouts and reconnection

| Where | Value |
|---|---|
| App, status request | 5 s, then counted as a failure; 2 failures → *Offline* badge; no update for 6 s → *Updating slowly* |
| App, action request | 8 s |
| App, confirmation of an action | 10 s |
| App, test-alert result | 75 s |
| Controller, Wi-Fi reconnect | every 60 s while disconnected (saved network) |
| Controller, alert sending | waits up to 60 s for Wi-Fi, 8 s HTTP timeout; queue of 6, extra alerts are dropped and logged |
| Controller, Wi-Fi setup page | closes after 3 min |

## Alerts (controller → ntfy)

From 3.1: `POST <ntfyServer>/` (the server root) with `Content-Type: application/json` and the body
`{"topic":"<topic>","title":"<title>","message":"<text>","priority":1-5,"tags":["<tag>"]}` (ntfy "publish as JSON"). JSON carries UTF-8, so Spanish accents arrive intact; up to 3.0 the title went in an HTTP header, which only carries ASCII. HTTPS without certificate checking (alerts carry no secrets; the topic name is the only secret, so use a long random one). Results come back to the event log: *Alert delivered*, *Alert not sent (no Wi-Fi)*, *Alert failed (code n)*.

## Wireless updates (ArduinoOTA)

Host name `reusewater`, update password from Settings (applied at once from 3.0). Accepted only while `updatesAllowed` is true. On start: watchdog paused, all outputs off, history saved, screen shows `UPDATING`.
