# RD45 AUX + Spotify: standalone ESP32 bridge control (3.9.2)

This branch adds **actual background Spotify playback control** to the customized
\`feature/3.9.2-wifi-steering-media\` version. It does not change CAN/VAN
message generation, RT scheduling, audio, or the RD45 source.

## Runtime

\`COM2000 SEEK -> VAN 0x9C4 -> existing rising-edge counters ->
SpotifyBridge FreeRTOS task (only while RD45 reports AUX) -> Spotify Web API ->
Spotify Connect on iPhone -> FiiO USB DAC -> RD45 AUX\`.

The task runs at low priority, polls the existing atomic counters every 200 ms,
and only makes HTTPS calls for new button events. An expired Spotify access
token is refreshed on the ESP32 itself. Browser Safari does **not** need to
remain open and Waze can be foreground on the same phone. Your phone still
needs an Internet connection and an active Spotify Connect playback device.
Waze's voice guidance can use the same iPhone audio output; test Waze setting
"Play sound via: Device default".

The actual RD45 input is passively observed from AEE2004 comfort CAN frame
`0x165` sent by the radio. Byte 2, bits 4..6, encodes the source; byte 0,
bit 7, indicates the radio is enabled. Only AUX 1 (4) or AUX 2 (5) permits
Spotify commands. Radio-off, CD, tuner, USB, Bluetooth, unknown source,
or status older than 1.5 seconds suppresses Spotify control. The radio's
original COM2000 controls (VAN 0x9C4 => CAN 0x21F) are never modified.

The read-only `/api/spotify/status` endpoint also reports
`radio_source` (0 means unknown/stale) and `aux_active`.
**Hardware capture (2026-10-22):** this RD45 emitted 0x165
`C0 80 40 00` on AUX1, `C0 80 10 00` on FM and
`C0 86 60 00` on USB. The source mapping has been observed on
hardware; the hold-to-seek behavior itself still needs an in-car test.

No playlist contents or playback audio are transferred through the bridge.
Short COM2000 SEEK presses still skip next/previous tracks, but are now
confirmed on button release rather than press. Holding either direction for at
least 700 ms seeks within the playing track when released: every second held
corresponds to 10 seconds of seek, capped at 120 seconds. A held direction
switches with the existing 'Swap seek directions' setting. This is one-shot
seeking, not continuous audible fast-forward/rewind. A stale or missing release
is discarded; an old gesture is never replayed after network reconnection.
Both operations require the RD45 to report a fresh AUX1/AUX2 source.

The previous browser
\`/media\` buttons and player information remain available when opened.

## One-time setup

1. **Wi-Fi:** Keep phone Personal Hotspot enabled. Connect to the bridge's
   existing AP and, in configuration, set \`STA_WIFI_SSID\` and
   \`STA_WIFI_PASSWORD\` to the hotspot network name/password. Save, then
   reboot bridge. **Do not commit these credentials to git, source files or
   tests.** If hotspot isn't detected during boot the firmware falls back to
   the bridge AP. Connected STA automatically reconnects on later disconnects.
2. **Find bridge:** In STA mode, \`192.168.100.1\` is not its address. Try
   \`http://psavancanbridge.local/media\`. If mDNS does not resolve from the
   iPhone hotspot host, consult serial log for the STA DHCP address, or
   connect from another device on the same hotspot network. Direct local
   access from the phone hosting the hotspot is iOS-dependent and **has not
   been tested**.
3. **Authorize Spotify:** In \`/media\`, enter public Spotify Client ID and
   exact registered HTTPS callback URL. Use Spotify Premium and Spotify
   Developer app allowlisting as applicable. Finish the existing manual
   OAuth PKCE callback step. Tokens initially remain in browser sessionStorage.
4. **Provision:** Tap **Install authorization on bridge**, then **Enable
   autonomous SEEK**. The refresh token is delivered once over the private
   local HTTP Wi-Fi link and stored in the bridge's NVS; Spotify HTTPS uses
   ESP-IDF certificate validation. The access token is renewed automatically.
5. **Play Spotify:** Start Spotify playback normally on the iPhone, route
   its audio through FiiO DAC / RD45 AUX. Keep Waze in foreground if wanted.
   Press the COM2000 seek controls to change tracks. The existing **Swap seek
   directions** checkbox is applied when you tap Enable autonomous SEEK.
6. **Check:** View \`/media\` background status. \`GET /api/spotify/status\`
   reports configured/enabled/online, last action and HTTP response code,
   **never** tokens. Use Disable or Erase when needed.

**Privacy:** The provisioning page is HTTP, not end-to-end encrypted between
browser and bridge. Use only a private trusted hotspot. Persisted credentials
use ordinary NVS, **not guaranteed to be encrypted** without flash/NVS
encryption enabled. Someone with physical flash access may extract them.
The endpoint deliberately refuses provisioning when bridge is in SoftAP mode.
This is a personal prototype, not production-quality secret storage.

## Implementation/API

- \`GET /api/spotify/status\`: read-only state including actual audio source and AUX gate.
- \`POST /api/spotify/provision\`: \`{"client_id":"...","refresh_token":"..."}\`.
  Credentials are written to NVS; the feature starts disabled.
- \`POST /api/spotify/enable\`: \`{"enabled":true}\` or \`false\`.
- \`POST /api/spotify/clear\`: clears the persistent permission and disables.
- The radio signal is generated by the **existing** bridge mapping. No injected
  CAN frames and no added monitor/diagnostic mode are used.

The browser automatically unchecks and disables its old stalk forwarding
toggle if autonomous mode is enabled, preventing duplicate Spotify skips.
For startup/offline conditions, all observed counters are consumed even when
network or authorization is unavailable: old events are **not** replayed.
At most four accumulated edges per polling interval are processed; larger
backlogs are discarded. Rate limits/HTTP errors lead to backoff and appear
in read-only status. TLS verification requires successful SNTP time sync
(\`pool.ntp.org\`).

## Validation needed before daily use

- `node tests/web-media-smoke.cjs` for the web control logic.
- `g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/spotify-seek-gesture-smoke.cpp -o /tmp/seek-test && /tmp/seek-test`
  for release, duration, queue overflow, clock wrap, and stale-event handling.
- \`g++ -std=c++17 -Wall -Wextra -Werror -Isrc tests/radio-audio-source-smoke.cpp -o /tmp/radio-test && /tmp/radio-test\` for the pure source decoder.
- \`pio run -e esp32c6_v16\` using the matching custom PlatformIO/ESP-IDF
  environment. This change has **not** been compiled on the specific firmware
  toolchain or flashed to the bridge in this chat.
- Test on bench with Spotify Premium authorization and Wi-Fi; confirm no
  duplicate skip, correct direction, token refresh after 1 hour, reconnect
  after hotspot disappears/reappears, and CAN/VAN framing/timing unaffected.
- Some iPhone hotspot setups do not allow the host to browse the tethered
  ESP32 locally; if so, provisioning from another client connected to that
  hotspot is necessary. Internet access to Spotify from the ESP32 is separately
  required.

**Known limitation:** This design uses Spotify Connect's cloud control
endpoints. Even when the Spotify app is already playing locally on iPhone,
commands may have network delay and can fail while the phone or bridge has no
Internet. This cannot provide the instantaneous guarantees of a local
Bluetooth AVRCP/HID media key.


## OEM EMF-C text probe (experimental, BENCH ONLY)

An off-by-default section in `/media` can preview the verified USB OEM
metadata format `0x63` over CAN `0x2E3`. It provides a captured Michael
Jackson test payload or the already-fetched Spotify artist/title; no extra
Spotify polling or persisted enable flag is introduced.

The **Send one OEM 0x63 test** control requires both a checkbox and a
second browser confirmation of isolated CAN bench wiring. The firmware
also insists on *fresh AUX1 from 0x165* and at least 30 seconds of
`0x2E3` RX silence. These are **software interlocks, not proof that no
other CAN node can transmit 0x2E3**. Do not use TX on the shared road-car
CAN bus. Merely enabling the firmware or opening the /media page does
not transmit any OEM display frames.

The bridge emits a bounded, ISO-TP single or first frame; for multi-frame
payloads it waits for real physical `0x15F` Flow Control (0x30, BS=0),
then sends CFs with the indicated separation time (minimum 10 ms). No
CAN ID spoofing, no modification of `0x165`, no changes to the standard
bridge message scheduler, and no recurring metadata TX. On unexpected
flow control, absent AUX1, observed `0x2E3` activity or a timeout the
operation aborts. The HTTP response reports only the transport
result, **not whether the EMF-C drew anything**.

After transmitting, use the existing Destination CAN monitor for
`0x2E3`, `0x15F`, and `0x165` and record a photograph of the physical
EMF-C. Its existing network/direction filter may require separate
captures for Incoming and Outgoing. For a conclusive experiment:
1. Confirm actual AUX1, with no competing transmitter on the same CAN ID.
2. First send the known captured OEM sample (not Spotify).
3. Note Flow Control and the actual EMF-C image.
4. Only then test Spotify track metadata with the same transport.
The OEM display glyph mapping for Unicode has not been established;
non-ASCII UTF-8 characters are conservatively replaced with '?'.

CI runs an independent native ISO-TP encoder test and the complete
ESP32-C6 build. The *actual* `firmware.bin` must fit **both** 0x140000-byte
OTA slots from `partitions.csv` with at least 4096 bytes remaining, or
the workflow fails before artifact publication.
