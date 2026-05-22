# Explorer Status Bar Metadata

A [Windhawk](https://windhawk.net/) mod for Windows Explorer that appends rich file metadata to the status bar when a single file is selected.

![Explorer status bar with metadata](screenshot.png)

## What it shows

| Field | Example | Notes |
|---|---|---|
| Dimensions | `1920 x 1080` | Images and video |
| Date modified | `15.05.2026 14:32` | System locale & timezone |
| File type | `PNG Image` | Windows description |
| Duration | `01:24:35` | Video / audio |
| Bitrate | `8.4 Mbps` | Video → audio → estimated |
| Frame rate | `23.98 fps` | Video |

## Supported formats

**Via Windows Property System** (indexed): JPEG, PNG, BMP, GIF, HEIC, MP4, MKV, AVI, MOV, MP3, FLAC, and most other common formats.

**Via custom binary parser** (not indexed by Windows):

| Format | Notes |
|---|---|
| SVG | viewBox priority, width/height fallback |
| HDR | Radiance RGBE |
| EXR | OpenEXR dataWindow |
| TIFF / TX | Little-endian and big-endian |
| TGA | Truevision TARGA (types 1–3, 9–11) |
| WebM / MKV | EBML/Matroska — dimensions + fps from DefaultDuration |

## Design

- **Zero polling.** Metadata is computed only when the selected file changes.
- **Path-keyed cache** (up to 1000 entries). Same file = instant return, zero COM calls.
- **LRU-style eviction.** When the cache is full, only the N oldest entries are removed (configurable).
- **UI-thread safe.** Hooks `DrawTextW` and `GetTextExtentPoint32W` synchronously; both calls hit the cache after the first.
- **No heap allocation** in the hot path — `IsSelectionStatus` uses direct pointer scan; GDI hook buffers are `thread_local`.

## Settings

Configurable in the Windhawk UI:

| Setting | Default | Description |
|---|---|---|
| Enable metadata on network drives | Off | ⚠️ Can freeze Explorer on slow networks |
| Enable metadata on removable drives | On | Disable for slow USB/SD media |
| Cache eviction count | 10 | Entries removed when cache hits 1000 |

## Installation

### Via Windhawk (recommended)
Search for **"Explorer Status Bar Metadata"** in the Windhawk mod catalog and click Install.

### Manual (load unpublished mod)
1. Open Windhawk → click the **+** button → **Load unpublished mod**
2. Paste the contents of `explorer-status-metadata.wh.cpp`
3. Click **Compile & Apply**

## Requirements

- Windows 10 or 11
- [Windhawk](https://windhawk.net/) v1.4 or later

## License

MIT © 2026 VitalS
