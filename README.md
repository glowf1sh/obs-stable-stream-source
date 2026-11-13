# Stable Stream Source for OBS Studio

An OBS Studio source for network streams (SRT, RTMP, RIST, UDP, HLS and anything else FFmpeg can open) that **keeps audio and video in sync after interruptions**.

*Deutsche Kurzfassung weiter unten.*

> **Status:** early development. The first release is being tested on OBS Studio 31.0.4 (Windows). Behaviour described below is the design goal of version 0.1.

## Why this plugin exists

If you bring a live stream from the road into OBS (IRL streaming over mobile networks, a remote camera, a contribution feed), the connection will drop from time to time: a dead spot, the phone switching networks, a call coming in, the encoder restarting. With the built-in **Media Source**, the picture comes back after such a gap, but **audio and video are often out of sync** afterwards, typically by 0.2 to 0.5 seconds, sometimes more. The offset stays until you restart the source by hand.

This is a known limitation of OBS ([obs-studio#12724](https://github.com/obsproject/obs-studio/issues/12724)) and it is not caused by the sender or the server: the stream itself is still in sync.

## What causes it

When the stream continues after a gap, its timestamps jump: the sender starts a new session with new timestamps, or the timestamps continue with a gap.

The Media Source passes the stream timestamps on to OBS. OBS handles the jump differently for audio and video:

- **Audio:** if the timestamp jumps by more than 2 seconds, OBS re-anchors the audio to the current clock, at the moment the first audio after the gap arrives.
- **Video:** OBS keeps timing the video relative to its old anchor.

From then on audio and video have two different reference points. The difference between them depends on how long decoding and buffering took at that moment, which is the offset you see. Restarting the source sets both anchors together again, which is why that helps.

## What this plugin does differently

Stable Stream Source decodes the stream itself and does the timing **before** OBS sees the frames:

1. Stream time is mapped to the computer clock with **one mapping shared by audio and video**.
2. Every frame is handed to OBS at its mapped time, with a timestamp close to the current clock. A small playout buffer (default 250 ms) absorbs network jitter.
3. When the stream timestamps jump (gap, new session, reconnect), the mapping is re-anchored **for audio and video at the same moment**. Their relative timing is taken from the stream, so they stay in sync.

OBS never sees a timestamp jump, so its own re-anchoring of the audio is never triggered. There is no restart of the source, no black frame and no need for scene switching tools to "refresh" the source.

The plugin reconnects automatically after the connection is lost.

## Settings

| Setting | Meaning |
|---|---|
| Stream URL | Any address FFmpeg can open, e.g. `srt://host:port?streamid=...&latency=1000000`, `rtmp://...`, `rist://...`, `udp://...`, `https://.../index.m3u8` |
| Input format | Optional, e.g. `mpegts`. Leave empty for auto detection. |
| Playout buffer | Extra delay in milliseconds to absorb network jitter (default 250 ms). |
| Reconnect delay | Seconds to wait before reconnecting after the stream was lost. |
| Disconnect while the source is not visible | Saves bandwidth when the source is not used in the active scene. |
| Show nothing while disconnected | Clears the picture when the connection is lost (otherwise the last frame stays). |
| YUV color range | Auto, partial or full. |

For SRT, set the latency in the URL (in microseconds, e.g. `latency=1000000` for 1 s) so lost packets can be retransmitted. Additional network buffering in OBS is not needed.

## Compatibility

- OBS Studio 31.x (built against 31.0.4) on Windows (x64) and macOS. Linux builds use the system FFmpeg.
- Uses the FFmpeg libraries shipped with OBS, no extra installation needed.
- Software decoding in this version.

## Installation

Download the package for your platform from the [Releases](https://github.com/glowf1sh/obs-stable-stream-source/releases) page and install it like any other OBS plugin. Then add a source of type **Stable Stream Source** to your scene.

## Building

The project is based on the official [OBS plugin template](https://github.com/obsproject/obs-plugintemplate). Builds for Windows, macOS and Linux run automatically on GitHub Actions. For local builds see the template documentation.

## License

GPL-2.0-or-later, see [LICENSE](LICENSE). Copyright © 2026 Glowfish GmbH & Co. KG.

---

## Deutsch: Kurzfassung

**Stable Stream Source** ist eine OBS-Quelle für Netzwerk-Streams (SRT, RTMP, RIST, UDP, HLS), die **Bild und Ton nach Unterbrechungen synchron hält**.

**Das Problem:** Reißt die Verbindung kurz ab, etwa durch ein Funkloch, einen Netzwechsel oder einen Anruf auf dem Handy, laufen in der normalen OBS-Medienquelle danach Bild und Ton oft um 0,2 bis 0,5 Sekunden auseinander. Das bleibt so, bis man die Quelle von Hand neu startet. Ursache: OBS richtet nach einem Zeitsprung im Stream den Ton neu an der aktuellen Uhrzeit aus, das Bild aber nicht.

**Die Lösung:** Diese Quelle dekodiert den Stream selbst und ordnet Bild und Ton über **eine gemeinsame Zuordnung** der Computeruhr zu. Springen die Zeitstempel, wird diese Zuordnung für beide gleichzeitig neu gesetzt. OBS bekommt nie einen Zeitsprung zu sehen, Bild und Ton bleiben zusammen. Kein Neustart der Quelle, kein schwarzes Bild, automatische Wiederverbindung.

**Einrichtung:** Plugin installieren, in der Szene eine Quelle vom Typ **Stabile Stream-Quelle** hinzufügen und die Stream-Adresse eintragen. Bei SRT die Latenz in der Adresse angeben, zum Beispiel `&latency=1000000` für 1 Sekunde.
