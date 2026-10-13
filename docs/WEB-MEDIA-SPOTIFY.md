# Web-only media control experiment (v3.4.1 fork)

Implementation branch: feature/web-media-spotify-control. **No legacy CAN/VAN forwarding, radio handling, or Wi-Fi parameters are modified.** Existing audio stays on RD45 AUX.

## What's implemented

- Rising-edge counters for seek_up and seek_down parsed from VAN 0x9C4. Counters live in CarState and increment at the same input parse point that already feeds CAN 0x21F.
- GET http://192.168.100.1/api/media returns next_count, previous_count, seek_up, seek_down, and uptime in milliseconds. No write endpoint is exposed.
- GET http://192.168.100.1/media is a self-contained web dashboard: poll counters, detect rising-edge event deltas, optional direction reversal, simulated events, diagnostics, status and disconnection handling.
- Spotify OAuth **Authorization Code + PKCE/S256** with a pure JS SHA-256 implementation because Safari on the HTTP bridge IP cannot rely on crypto.subtle. Tokens are stored in sessionStorage of the browser **not on the bridge**. No client secret is required. Full callback URL + OAuth state are verified before exchange. Refresh token logic is included.
- Spotify player information: title, artist, album, album art, progress, active device and playback status. Commands: next, previous, play/pause, seek, volume, shuffle, repeat, discover Spotify Connect devices, transfer playback. Spotify errors and rate limiting surface in diagnostics.
- Physical AUX and existing display are untouched.

## Requirements and installation

1. Make a Spotify Developer application. Development mode may restrict users/devices and require Spotify Premium. Enter this account in the app's allowed-user list if required.
2. Host the static file docs/spotify-callback.html on your own **HTTPS** URL (e.g. enable GitHub Pages, or place it on your existing HTTPS web server). It must serve as a real HTTPS page; simply keeping it in the repository does not host it.
3. Register the **exact** hosted HTTPS callback URL with the Spotify developer app as a Redirect URI. Spotify does not allow http://192.168.100.1 as an OAuth redirect. No credentials should be added to the callback.
4. Build/flash firmware for the matching PCB from branch feature/web-media-spotify-control using the original repository's PlatformIO/ESP-IDF procedure. Keep a recovery image of the working firmware.
5. Connect iPhone to **PSA VAN-CAN Bridge** Wi-Fi, open http://192.168.100.1/media and check that counters increase once per physical press. If direction is inverted, tick “Swap seek directions”.
6. Enter the Spotify Client ID (never a secret), enter the HTTPS callback URL and tap Authorize. Finish Spotify authorization, copy the **full callback URL** from the hosted page, return to http://192.168.100.1/media and paste it into the callback field. Tap Complete authorization.
7. With a currently active Spotify playback device, verify manual Next, Previous and metadata first; then enable stalk forwarding. Use foreground Safari. To prevent loss, avoid switching Safari to background.
8. The iPhone must have **working Internet routing to Spotify in addition to local access to bridge Wi-Fi**. A SoftAP-only bridge has no Internet. iOS cellular fallback while on a Wi-Fi network without Internet is device/configuration dependent; if Spotify requests time out, use a network configuration that provides access to both.

Spotify API rate limits and Premium restrictions apply. Web page running in Safari can be suspended on background or phone lock and does not provide system-wide media key injection. For locked-screen media control use a future Bluetooth AMS/HID or native accessory path, not a web-only page.

## Test plan

- Start with engine off/ignition as required for power; monitor /api/media while pressing seek up/down. Count one per press, no repetition while held; repeated short presses increment separately.
- Refresh page during idle; no phantom Spotify command on new-page counter baseline.
- Press a few times while page disconnected; reconnect: stale events are not replayed.
- Verify Spotify current song and playlist controls with a real Premium account.
- Verify Spotify token refresh after the access token expires; session tokens do not persist after the browser session.
- Test no Internet and token revocation: existing VAN/CAN and AUX must continue functioning, errors should be visible only in media dashboard.
- Monitor radio/VAN/CAN bus for regressions at Wi-Fi polling ~2.5 requests/second; profile for CPU/heap and VAN frame losses on target.
- Do not use this page while actively driving. Initial test parked only.

## Explicitly not implemented

**EMF-C text injection/CD-changer emulation** is not safe to claim from unverified message numbers. Before enabling TX, capture the actual RD45 ↔ EMF-C CAN frames for AUX, CD and Bluetooth, identify producer of candidate text frames, ensure single transmitter and validate display arbitration. Feature shall be opt-in with a hard disable and never substitute radio source without proof. This feature does not attempt that and has no CAN transmit code.

**No production/vehicle verification was possible in this environment.** These changes require local PlatformIO compilation against this fork's matching ESP-IDF toolchain, flashing to hardware and testing with iPhone + RD45. Web API behavior requires actual Spotify access and reachable Internet.

## Security notes

The older fork has a fixed Wi-Fi passphrase in WebServer.hpp. This feature deliberately does not change that unrelated setting. Use a private environment and secure the AP before trusting the HTTP page with Spotify refresh tokens. Browser sessionStorage is still accessible to JavaScript on this HTTP origin and to someone who can interact with the unlocked iPhone. Authorization codes are one-use and PKCE-protected but should not be published. A hosted callback must not load third-party analytics.
