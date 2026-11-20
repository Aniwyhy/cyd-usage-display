# CYD Usage Display

Firmware for a 4" "Cheap Yellow Display" (ESP32-32E, ST7796 480×320, CH340 USB) that shows AI plan usage limits on its own over WiFi. No computer needed once it's set up.

| Branch | What it shows |
| --- | --- |
| `main` | Claude only: 5-hour and weekly limits as two large ring gauges |
| `split-screen` | Claude on the left, ChatGPT (Codex limits) on the right |

## Build and flash

```bash
pio run -t upload --upload-port /dev/cu.usbserial-10
```

The upload speed is set to 460800 because the CH340 fails at 921600.

## Setup

1. **WiFi:** on first boot the board opens a hotspot called `Claude-Usage`. Scan the QR code on screen; its password is the 8 digits shown under it. Then pick your network on the page that opens (http://192.168.4.1).
2. **Claude:** open the board's address (shown on screen, e.g. `http://192.168.0.219`) and use **Sign in with Claude**, then paste the code back. The login only asks for the `user:profile` scope, so the board can read usage but can't send prompts. It doesn't affect your Claude Code login.
3. **ChatGPT** (`split-screen`): the right half shows a code and a QR code to `auth.openai.com/codex/device`. The account owner signs in there and enters the code. Device-code sign-in may need to be turned on in ChatGPT's security settings.

**BOOT button:** tap to refresh now (or to get a new ChatGPT code); hold for 5 s to forget WiFi and both sign-ins.

## Notes

- Usage comes from the same internal endpoints Claude Code's `/usage` and Codex's `/status` use (`api.anthropic.com/api/oauth/usage` and `chatgpt.com/backend-api/wham/usage`). These aren't official APIs and may change.
- ChatGPT exposes no meter for ordinary chat messages; the numbers on the right are the Codex limits that come with the ChatGPT plan.
- Tokens are stored only in the board's flash (NVS). Nothing secret is in this repo.
- The timezone is set in `src/main.cpp` (`TZ_INFO`, Europe/Madrid).
