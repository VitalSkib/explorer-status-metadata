# Explorer Status Bar Metadata

A [Windhawk](https://windhawk.net/) mod for Windows Explorer that appends rich file metadata to the status bar when a single file is selected.

![Explorer status bar with metadata](screenshot.png)

## What it shows

| Field | Example | Notes |
|---|---|---|
| Dimensions | `1920 x 1080` | Images and video |
| Date modified | `15.05.2026 14:32` | System locale & timezone |
| Duration | `01:24:35` | Video / audio |
| Bitrate | `8.4 Mbps` | Video → audio → estimated |
| Frame rate | `23.98 fps` | Video |
| File type | `PNG Image` | Windows description (always aligned to the right edge) |

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
| PSD / PSB | Adobe Photoshop File (Dimensions extracted from fixed 26-byte header) |
| WebM / MKV | EBML/Matroska — dimensions + fps from DefaultDuration |

## Design

- **Zero polling.** Metadata is computed only when the selected file changes.
- **Robust & Language-Independent.** Hooks `PSFormatForDisplayAlloc` (the exact property formatting function Explorer calls), eliminating text-matching heuristics and false positives.
- **Path-keyed cache** (up to 1000 entries). Same file = instant return, zero COM calls.
- **True LRU eviction.** Cache hits dynamically move entries to the back of the queue, protecting frequently accessed files from being evicted.
- **Thread-safe.** Securely handles cross-thread messaging between shell worker threads and UI processes using strict Process ID guards.
- **Live Settings.** Changes made in the Windhawk UI (e.g., toggling network drives) clear the cache and apply instantly without requiring an Explorer restart.

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
2. Paste the contents of the `.cpp` file.
3. Click **Compile & Apply**

## Requirements

- Windows 10 or 11
- [Windhawk](https://windhawk.net/) v1.4 or later

## License

MIT © 2026 VitalS
