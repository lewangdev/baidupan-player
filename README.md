<p align="right">
  <strong>English</strong> · <a href="README.zh_CN.md">简体中文</a>
</p>

<h1 align="center">Baidupan Pocket Player</h1>

<p align="center">
  <strong>Your Baidu Netdisk music library, on a pocket badge.</strong><br>
  Streaming MP3/M4A/AAC/WAV player firmware for the FoloToy AI Passport (ESP32-C3, no PSRAM).
</p>

---

## Features

- **Stream straight from Baidu Netdisk** — no local storage, no phone app. Browse folders or list
  every MP3, M4A, AAC and WAV file in your Netdisk in name order.
- **Scan to sign in** — the screen shows a QR code; confirm on your phone with your Baidu account.
  The grant is stored on the device and refreshed automatically.
- **Web Wi-Fi setup** — on first boot the device opens the `BaiduPlayer-XXXX` hotspot. Scan the QR
  code on screen, pick your network in the captive web page, done.
- **Player that keeps going** — auto-advance through the folder and onto the next page, resume
  interrupted downloads with HTTP Range, volume remembered across reboots.
- **Chinese file names** — the built-in font covers all 7,445 GB2312 characters.
- **Three-button UI** designed for the 240×320 round-corner screen; the screen sleeps after 30 s
  while music keeps playing.

## Quick start

1. **Get Baidu credentials.** Create an app on the
   [Baidu Netdisk open platform](https://pan.baidu.com/union/console), then copy
   `main/bp_baidu_keys.example.h` to `main/bp_baidu_keys.h` and fill in the AppKey and SecretKey.
   That file is Git-ignored — never commit it.
2. **Build and flash** with ESP-IDF 5.5.3:

   ```bash
   idf.py set-target esp32c3
   idf.py build
   idf.py -p <PORT> flash          # first install
   idf.py -p <PORT> app-flash      # later updates keep Wi-Fi and authorization
   ```

   The full gate (`./tools/validate.sh`) runs repository checks, host tests and a verified merged image.
3. **Connect Wi-Fi.** Join the `BaiduPlayer-XXXX` hotspot by scanning the screen; the setup page
   opens (or visit `http://192.168.4.1`). Only 2.4 GHz networks are supported.
4. **Bind your Netdisk and play.** Once Wi-Fi connects, the binding page opens by itself. Scan its QR
   code with the **Baidu Netdisk app** and confirm; *All audio* then opens, so press OK on any track.

> Firmware built from your own credentials embeds your SecretKey. Do not publish your `build/` output.

## Buttons

| Screen | Up / Down | OK | Long OK | Long Up / Down |
| --- | --- | --- | --- | --- |
| Player (start screen) | Volume ±10 | Play / pause (opens *All audio* if nothing is loaded) | Open Settings | Previous / next track |
| Settings menu | Select | Open | Back to the player | — |
| List | Select | Open folder / play / page | Parent folder / Settings | Move 5 rows |
| Player skin | Classic or Tape | Apply (saved) | Back to Settings | — |
| Brightness | Level 1–5 (default 3, saved) | — | Back to Settings | — |
| Wi-Fi | — | Start setup hotspot | Back to Settings | — |

The Settings menu holds *All audio*, *Browse*, *Player skin*, *Brightness*, *Wi-Fi*, *Netdisk account* and
*About*. The **Tape** skin shows two spinning reels (the emptier reel spins faster), black and white
waveforms on the tape, and a fish cut in two whose swaying head and tail are joined by live
sound-wave bars; the **Classic** skin is the progress ring.

## Supported audio

| Format | Details |
| --- | --- |
| MP3 | MPEG-1/2 Layer III, CBR and VBR, any sample rate (Helix decoder) |
| M4A | AAC-LC in an MP4 container (Helix AAC); HE-AAC plays its AAC-LC core. The index must be at the start of the file |
| AAC | ADTS AAC-LC |
| WAV | 16-bit PCM, mono or stereo, 8–48 kHz |

Stereo is downmixed for the single speaker. Browsing shows only folders and playable files; FLAC,
OGG and other files are hidden. The format is detected from the file content, so an M4A file
named `.mp3` still plays.
Seeking is not supported. Browsing while a track plays pauses it automatically and releases the
download connection (the device cannot hold two TLS connections); press OK on the player to resume.

## How it works

```text
Baidu OAuth (device code) ─► xpan list / categorylist ─► filemetas dlink
                                                             │
control task ─► fetch task (HTTPS, 302, Range resume) ─► 20 KB stream buffer
                                                             │
                              decode task (Helix MP3 / AAC / WAV) ─► mono PCM ─► ES8311 / I2S
```

| Module | Role |
| --- | --- |
| [`main/bp_baidu.c`](main/bp_baidu.c) | Device-code authorization, locked single-use token refresh, folder and audio lists, download links |
| [`main/bp_player.c`](main/bp_player.c) | Streaming pipeline, playlist, auto-advance, per-stage heap logging |
| [`main/bp_ui.c`](main/bp_ui.c) | LVGL pages: player, list and settings stay resident; rarely used pages are built on entry and freed on exit |
| [`main/bp_ui_reel.c`](main/bp_ui_reel.c) | Tape skin: pre-rendered reel and fish frames in flash, waveforms driven by the live audio level |
| [`main/bp_wifi.cc`](main/bp_wifi.cc) | Station reconnect and SoftAP captive-portal provisioning |
| [`main/bp_media.c`](main/bp_media.c) | Pure logic with host tests: formats, content sniffing, MP3 frame checks, ID3v2, WAV headers, paths |
| [`main/bp_mp4.c`](main/bp_mp4.c) | Streaming M4A header parser; mdat is then decoded as consecutive raw AAC blocks without a frame-size table |

The ESP32-C3 has about 180 KB of heap for everything. Measured during playback: ~106 KB free
before a track, ~52 KB after the CDN TLS connection, ~20 KB minimum while decoding, and full
recovery after each track. See [docs/baidupan-pocket-player.md](docs/baidupan-pocket-player.md) for details,
serial console commands and limits.

## Credits

- Built on [FoloToy AI Passport](https://github.com/FoloToy/ai-passport) (board support, build
  and validation tooling). Its template documentation remains in [docs/README.md](docs/README.md).
- Baidu Netdisk integration follows
  [netdisk-recording-badge](https://github.com/openbrt/netdisk-recording-badge).
- [esp-wifi-connect](https://github.com/78/esp-wifi-connect) for Wi-Fi and the captive portal,
  [libhelix-mp3](https://components.espressif.com/components/chmorgan/esp-libhelix-mp3) for MP3, the Helix AAC decoder (via [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio), RPSL) for AAC,
  and Source Han Sans SC (SIL OFL) for Chinese text.

Released under the [MIT License](LICENSE).
