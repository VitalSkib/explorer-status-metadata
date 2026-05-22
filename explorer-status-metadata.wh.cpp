// ==WindhawkMod==
// @id              explorer-status-metadata
// @name            Explorer Status Bar Metadata
// @description     Appends rich metadata (dimensions, date, type, duration, bitrate, fps) to Explorer's status bar. Zero polling — purely reactive, path-keyed cache.
// @version         1.3.0
// @author          VitalS
// @github          https://github.com/VitalSkib
// @include         explorer.exe
// @compilerOptions -lgdi32 -luser32 -lole32 -loleaut32 -luuid -lpropsys
// ==/WindhawkMod==

// ==WindhawkModSettings==
/*
- allowNetworkDrives: false
  $name: Enable metadata on network drives (UNC paths)
  $description: >
    WARNING: May cause Explorer to freeze for several seconds if the network
    is slow or unavailable. The metadata lookup is synchronous on the UI thread.
    Only enable this if you are on a fast, reliable local network.

- allowRemovableDrives: true
  $name: Enable metadata on removable drives (USB, SD cards)
  $description: >
    Metadata lookup on slow removable media (e.g. old USB drives) may cause
    a brief UI stutter. Fast USB 3.x drives are generally fine.

- cacheEvictCount: 10
  $name: Cache eviction count
  $description: >
    When the cache reaches 1000 entries, this many of the oldest entries
    are removed. Range: 1-50. Lower values preserve more history;
    higher values free more memory at once.
*/
// ==/WindhawkModSettings==

// ==WindhawkModReadme==
/*
# Explorer Status Bar Metadata — v1.3.0

Appends file metadata to the Windows Explorer status bar when a single file
is selected: dimensions, modification date, file type, duration, bitrate, fps.

## How it works
- No timers, no polling. Metadata is computed only when the selected file changes.
- Path-keyed LRU-style cache (1000 entries). Same file = instant return, zero COM calls.
- Hooks `DrawTextW` and `GetTextExtentPoint32W` in the Explorer UI thread.
- Custom binary parsers for formats the Windows Property System does not index:
  SVG, HDR, EXR, TIFF/TX (LE+BE), TGA, WebM/MKV (EBML).

## Settings
- **Network drives** — disabled by default. Can freeze Explorer on slow networks.
- **Removable drives** — enabled by default. Disable if you use slow USB media.
- **Cache evict count** — how many old entries to drop when the 1000-entry limit is hit.

## Changelog
### v1.3.0
- Initial release
*/
// ==/WindhawkModReadme==

#include <initguid.h>
#include <windows.h>
#include <shlobj.h>
#include <shlguid.h>
#include <propkey.h>
#include <propsys.h>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <vector>

// ─── Undocumented message to obtain IShellBrowser from a window ──────────────
#define CWM_GETISHELLBROWSER (WM_USER + 7)

// ─── Hook function pointer types ────────────────────────────────────────────
typedef int  (WINAPI *DrawTextW_t)            (HDC, LPCWSTR, int, LPRECT, UINT);
typedef BOOL (WINAPI *GetTextExtentPoint32W_t)(HDC, LPCWSTR, int, LPSIZE);

DrawTextW_t             g_origDTW  = nullptr;
GetTextExtentPoint32W_t g_origGTEP = nullptr;

// ─── Settings (read once at init, immutable afterwards) ──────────────────────
static bool   g_allowNetworkDrives   = false;
static bool   g_allowRemovableDrives = true;
static size_t g_cacheEvictCount      = 10;

// ─── Metadata cache ──────────────────────────────────────────────────────────
// unordered_map for O(1) lookup + vector to track insertion order for eviction.
static CRITICAL_SECTION g_cs;
static std::unordered_map<std::wstring, std::wstring> g_metaCache;
static std::vector<std::wstring>                      g_cacheOrder; // insertion order
static const size_t MAX_CACHE_SIZE = 1000;

// ─── Binary structures for TIFF/TX parser ────────────────────────────────────
#pragma pack(push, 1)
struct TiffHeader { WORD magic; WORD version; DWORD ifdOffset; };
struct TiffTag    { WORD tagId; WORD tagType; DWORD count; DWORD valueOffset; };
#pragma pack(pop)

// ═══════════════════════════════════════════════════════════════════════════════
// BLOCK 1: Custom format header parsers
//   Covers formats the Windows Property System does not index natively:
//   WebM/MKV (EBML), TIFF/TX (LE+BE), SVG, HDR, EXR, TGA.
// ═══════════════════════════════════════════════════════════════════════════════

struct CustomMeta {
    int    width  = 0;
    int    height = 0;
    double fps    = 0.0;
};

static CustomMeta ParseCustomFormatMetadata(const std::wstring& filePath, const std::wstring& ext)
{
    CustomMeta meta;

    HANDLE hFile = CreateFileW(
        filePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (hFile == INVALID_HANDLE_VALUE) {
        Wh_Log(L"[Parser] Cannot open file: %ls (GLE=%u)", filePath.c_str(), GetLastError());
        return meta;
    }

    DWORD bytesRead = 0;

    // ── WebM / MKV (EBML/Matroska) ───────────────────────────────────────────
    // Flat streaming parse: containers (Segment, Info, Tracks, TrackEntry, Video)
    // are entered by letting the loop continue into their bodies without skipping.
    // Leaf elements (PixelWidth, PixelHeight, DefaultDuration) are read directly.
    if (ext == L"webm" || ext == L"mkv") {
        unsigned char buf[16384];
        if (ReadFile(hFile, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 16) {
            const unsigned char* p   = buf;
            const unsigned char* end = buf + bytesRead;

            // Reads a variable-length EBML ID (1–4 bytes, leading 1-bit signals length)
            auto readID = [&]() -> uint32_t {
                if (p >= end) return 0;
                uint8_t first = *p;
                if (first == 0) return 0;
                int     len  = 1;
                uint8_t mask = 0x80;
                while (!(first & mask) && len <= 4) { mask >>= 1; len++; }
                if (p + len > end) return 0;
                uint32_t val = 0;
                for (int i = 0; i < len; ++i) val = (val << 8) | *p++;
                return val;
            };

            // Reads a variable-length EBML data size (1–8 bytes).
            // Returns (uint64_t)-1 for "unknown size" (all data bits set).
            auto readSize = [&]() -> uint64_t {
                if (p >= end) return 0;
                uint8_t first = *p;
                if (first == 0) return 0;
                int     len  = 1;
                uint8_t mask = 0x80;
                while (!(first & mask) && len <= 8) { mask >>= 1; len++; }
                if (p + len > end) return 0;
                uint64_t val         = (*p++) & ~mask;
                for (int i = 1; i < len; ++i) val = (val << 8) | *p++;
                uint64_t unknownSize = (1ULL << (7 * len)) - 1;
                if (val == unknownSize) return (uint64_t)-1;
                return val;
            };

            // Reads an unsigned integer from the next `size` bytes (big-endian)
            auto readUint = [&](uint64_t size) -> uint64_t {
                if (size > 8 || p + size > end) return 0;
                uint64_t val = 0;
                for (uint64_t i = 0; i < size; ++i) val = (val << 8) | *p++;
                return val;
            };

            while (p < end) {
                uint32_t id   = readID();
                if (id == 0) break;
                uint64_t size = readSize();

                // Handle unknown-size and truncated-in-buffer containers
                if (size == (uint64_t)-1) {
                    if (id == 0x18538067 /*Segment*/) size = (uint64_t)(end - p);
                    else break;
                } else if (p + size > end) {
                    // Container body extends past our read buffer — clamp and enter anyway
                    if (id == 0x18538067 /*Segment*/  || id == 0x1549A966 /*Info*/    ||
                        id == 0x1654AE6B /*Tracks*/   || id == 0xAE /*TrackEntry*/    ||
                        id == 0xE0       /*Video*/)
                        size = (uint64_t)(end - p);
                    else
                        break;
                }

                if (id == 0x18538067 || id == 0x1549A966 || id == 0x1654AE6B ||
                    id == 0xAE        || id == 0xE0) {
                    // Container: do not skip, let the loop parse the body directly
                    continue;
                } else if (id == 0xB0) {           // PixelWidth
                    meta.width  = (int)readUint(size);
                } else if (id == 0xBA) {           // PixelHeight
                    meta.height = (int)readUint(size);
                } else if (id == 0x23E383) {       // DefaultDuration (nanoseconds)
                    uint64_t defDur = readUint(size);
                    if (defDur > 0) meta.fps = 1000000000.0 / (double)defDur;
                } else {
                    // Guard: size == 0 means empty element body.
                    // Without this break, p would not advance → infinite loop.
                    if (size == 0) break;
                    p += size;
                }

                if (meta.width > 0 && meta.height > 0 && meta.fps > 0.0) break;
            }
        }
    }
    // ── TIFF / TX ─────────────────────────────────────────────────────────────
    // Supports both Little-Endian (II, magic=42) and Big-Endian (MM, magic=42 BE).
    else if (ext == L"tx" || ext == L"tif" || ext == L"tiff") {
        TiffHeader head;
        if (ReadFile(hFile, &head, sizeof(head), &bytesRead, nullptr)
            && bytesRead == sizeof(head))
        {
            bool isLE = (head.magic == 0x4949 && head.version == 42);
            bool isBE = (head.magic == 0x4D4D && head.version == 0x2A00);

            if (isLE || isBE) {
                DWORD ifdOffset = isLE ? head.ifdOffset : _byteswap_ulong(head.ifdOffset);
                if (SetFilePointer(hFile, ifdOffset, nullptr, FILE_BEGIN)
                    != INVALID_SET_FILE_POINTER)
                {
                    WORD numTagsRaw = 0;
                    ReadFile(hFile, &numTagsRaw, sizeof(numTagsRaw), &bytesRead, nullptr);
                    WORD numTags = isLE ? numTagsRaw : _byteswap_ushort(numTagsRaw);

                    int width = 0, height = 0;
                    for (WORD i = 0; i < numTags && !(width && height); ++i) {
                        TiffTag tag;
                        if (!ReadFile(hFile, &tag, sizeof(tag), &bytesRead, nullptr)) break;

                        WORD  tagId   = isLE ? tag.tagId       : _byteswap_ushort(tag.tagId);
                        WORD  tagType = isLE ? tag.tagType     : _byteswap_ushort(tag.tagType);
                        DWORD valOff  = isLE ? tag.valueOffset : _byteswap_ulong(tag.valueOffset);

                        if      (tagId == 0x0100) width  = (tagType == 3) ? (int)(valOff & 0xFFFF) : (int)valOff;
                        else if (tagId == 0x0101) height = (tagType == 3) ? (int)(valOff & 0xFFFF) : (int)valOff;
                    }
                    if (width > 0 && height > 0) { meta.width = width; meta.height = height; }
                }
            }
        }
    }
    // ── SVG ───────────────────────────────────────────────────────────────────
    // Priority: viewBox (logical canvas size). Fallback: explicit width=/height=.
    // Search is scoped to start at the <svg tag to avoid false matches on nested
    // elements that appear earlier in the file (e.g. <symbol width=...>).
    else if (ext == L"svg") {
        char buf[2048] = {0};
        if (ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            std::string content(buf, bytesRead);
            size_t svgTagPos = content.find("<svg");
            if (svgTagPos == std::string::npos) svgTagPos = 0;

            size_t vbPos = content.find("viewBox=", svgTagPos);
            if (vbPos != std::string::npos) {
                size_t q1 = content.find('"', vbPos);
                if (q1 != std::string::npos) {
                    size_t q2 = content.find('"', q1 + 1);
                    if (q2 != std::string::npos) {
                        float x, y, w, h;
                        if (sscanf_s(content.substr(q1 + 1, q2 - q1 - 1).c_str(),
                                     "%f %f %f %f", &x, &y, &w, &h) == 4) {
                            meta.width  = (int)w;
                            meta.height = (int)h;
                        }
                    }
                }
            }
            if (meta.width == 0) {
                size_t wPos = content.find("width=",  svgTagPos);
                size_t hPos = content.find("height=", svgTagPos);
                if (wPos != std::string::npos && hPos != std::string::npos) {
                    int w = 0, h = 0;
                    if (sscanf_s(content.c_str() + wPos,  "width=\"%d",  &w) == 1 &&
                        sscanf_s(content.c_str() + hPos, "height=\"%d", &h) == 1) {
                        meta.width = w; meta.height = h;
                    }
                }
            }
        }
    }
    // ── HDR (Radiance RGBE) ───────────────────────────────────────────────────
    // Resolution line format: "-Y <height> +X <width>"
    else if (ext == L"hdr") {
        char buf[1024] = {0};
        if (ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0) {
            std::string content(buf, bytesRead);
            size_t marker = content.find("-Y");
            if (marker != std::string::npos) {
                size_t lineEnd = content.find('\n', marker);
                if (lineEnd != std::string::npos) {
                    int w = 0, h = 0;
                    if (sscanf_s(content.substr(marker, lineEnd - marker).c_str(),
                                 "-Y %d +X %d", &h, &w) == 2) {
                        meta.width = w; meta.height = h;
                    }
                }
            }
        }
    }
    // ── EXR (OpenEXR) ─────────────────────────────────────────────────────────
    // Scans attribute list for "dataWindow" (type box2i: xMin,yMin,xMax,yMax).
    // Width  = xMax - xMin + 1
    // Height = yMax - yMin + 1
    // Uses memcpy to avoid undefined behaviour from unaligned int* cast.
    else if (ext == L"exr") {
        unsigned char head[512] = {0};
        if (ReadFile(hFile, head, sizeof(head), &bytesRead, nullptr) && bytesRead >= 4
            && head[0] == 0x76 && head[1] == 0x2f && head[2] == 0x31 && head[3] == 0x01)
        {
            for (DWORD i = 4; i + 32 < bytesRead; ++i) {
                if (memcmp(head + i, "dataWindow", 10) == 0) {
                    size_t off = i + 11; // skip "dataWindow\0"
                    // Layout: type "box2i\0" (6) + size DWORD (4) + xMin,yMin,xMax,yMax (16)
                    // Need: off + 26 bytes within the buffer
                    if (off + 26 <= (size_t)bytesRead
                        && memcmp(head + off, "box2i", 5) == 0)
                    {
                        int box[4];
                        memcpy(box, head + off + 10, sizeof(box)); // safe unaligned read
                        int w = box[2] - box[0] + 1;
                        int h = box[3] - box[1] + 1;
                        if (w > 0 && h > 0 && w < 65535 && h < 65535) {
                            meta.width = w; meta.height = h;
                        }
                    }
                    break;
                }
            }
        }
    }
    // ── TGA (Truevision TARGA) ────────────────────────────────────────────────
    // Fixed 18-byte header. Width/height at bytes 12–15 (little-endian WORD each).
    // Supported image types: 1=CM, 2=RGB, 3=BW (raw) and 9,10,11 (RLE variants).
    else if (ext == L"tga") {
        unsigned char hdr[18] = {0};
        if (ReadFile(hFile, hdr, sizeof(hdr), &bytesRead, nullptr)
            && bytesRead == sizeof(hdr))
        {
            BYTE imgType = hdr[2];
            if (imgType == 1 || imgType == 2  || imgType == 3  ||
                imgType == 9 || imgType == 10 || imgType == 11)
            {
                int w = (int)(hdr[12] | (hdr[13] << 8));
                int h = (int)(hdr[14] | (hdr[15] << 8));
                if (w > 0 && h > 0 && w < 65535 && h < 65535) {
                    meta.width = w; meta.height = h;
                }
            }
        }
    }

    CloseHandle(hFile);
    return meta;
}

// ═══════════════════════════════════════════════════════════════════════════════
// BLOCK 2: Utility functions
// ═══════════════════════════════════════════════════════════════════════════════

// Returns true if the file at `path` is safe to parse synchronously on the
// Explorer UI thread (i.e. the read will complete in < ~5 ms).
// Controlled by Windhawk settings for network and removable drives.
// Reparse points (junctions, symlinks) are always skipped — they may point
// to slow or unavailable targets.
static bool IsSafeToParse(const std::wstring& path)
{
    if (path.empty() || path.length() < 3) return false;

    // UNC network paths (\\server\share\...)
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        if (!g_allowNetworkDrives) {
            Wh_Log(L"[Safety] Skipping network path: %ls", path.c_str());
            return false;
        }
        // Network paths pass the drive-type and attribute checks below
        // only if the setting is enabled.
        goto check_attributes;
    }

    {
        wchar_t root[4] = { path[0], L':', L'\\', L'\0' };
        UINT driveType  = GetDriveTypeW(root);

        if (driveType == DRIVE_REMOVABLE && !g_allowRemovableDrives) {
            Wh_Log(L"[Safety] Skipping removable drive path: %ls", path.c_str());
            return false;
        }
        if (driveType != DRIVE_FIXED    &&
            driveType != DRIVE_REMOVABLE &&
            driveType != DRIVE_REMOTE)   // DRIVE_REMOTE handled above via UNC check
        {
            Wh_Log(L"[Safety] Skipping unsupported drive type %u: %ls", driveType, path.c_str());
            return false;
        }
    }

check_attributes:
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return false;
    if (attr & FILE_ATTRIBUTE_OFFLINE) {
        Wh_Log(L"[Safety] Skipping offline file: %ls", path.c_str());
        return false;
    }
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
        // Reparse points (symlinks, junctions) may point anywhere — always skip.
        return false;
    }

    return true;
}

// Detects the Explorer status-bar "N item(s) selected" string without
// allocating heap memory. Called on every DrawTextW / GetTextExtentPoint32W
// invocation, so must be as cheap as possible.
static bool IsSelectionStatus(const wchar_t* s, UINT len)
{
    if (!s || len < 2) return false;
    UINT checkLen = (len < 512u) ? len : 512u;

    for (UINT i = 0; i + 7 < checkLen; ++i) {
        wchar_t c = s[i];
        // English: "selected" / "Selected"
        if ((c == L's' || c == L'S') && wcsncmp(s + i + 1, L"elected", 7) == 0)
            return true;
        // Russian: "выбр" (U+0432 U+044B U+0431 U+0440) / "Выбр" (U+0412 ...)
        if ((c == L'\u0432' || c == L'\u0412') && i + 3 < checkLen &&
            s[i+1] == L'\u044b' && s[i+2] == L'\u0431' && s[i+3] == L'\u0440')
            return true;
    }
    return false;
}

// Strips Unicode LTR/RTL directional marks (U+200E, U+200F) that Windows
// appends to formatted date strings.
static std::wstring CleanPropString(LPCWSTR src)
{
    if (!src) return L"";
    std::wstring s(src);
    s.erase(std::remove(s.begin(), s.end(), L'\x200e'), s.end());
    s.erase(std::remove(s.begin(), s.end(), L'\x200f'), s.end());
    return s;
}

// ═══════════════════════════════════════════════════════════════════════════════
// BLOCK 3: Selection path discovery via CWM_GETISHELLBROWSER
// ═══════════════════════════════════════════════════════════════════════════════

// Extracts the filesystem path of the single selected item from a shell browser.
// Returns empty string if no item or multiple items are selected.
// GetSelection(FALSE, ...) — FALSE prevents the folder itself from being
// returned as a "selection" when nothing is actually selected.
static std::wstring ExtractPathFromBrowser(IShellBrowser* pSB)
{
    if (!pSB) return L"";
    IShellView* pSV = nullptr;
    if (FAILED(pSB->QueryActiveShellView(&pSV)) || !pSV) return L"";

    std::wstring path;
    IFolderView2* pFV2 = nullptr;
    if (SUCCEEDED(pSV->QueryInterface(IID_IFolderView2, (void**)&pFV2)) && pFV2) {
        IShellItemArray* pIA = nullptr;
        if (SUCCEEDED(pFV2->GetSelection(FALSE, &pIA)) && pIA) {
            DWORD cnt = 0;
            pIA->GetCount(&cnt);
            if (cnt == 1) {
                IShellItem* pItem = nullptr;
                if (SUCCEEDED(pIA->GetItemAt(0, &pItem)) && pItem) {
                    LPWSTR ps = nullptr;
                    if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &ps)) && ps) {
                        path = ps;
                        CoTaskMemFree(ps);
                    }
                    pItem->Release();
                }
            }
            pIA->Release();
        }
        pFV2->Release();
    }
    pSV->Release();
    return path;
}

// Finds the path of the currently selected file in the active Explorer window.
// Strategy 1: Walk the focus window hierarchy upward, asking each window for
//             its IShellBrowser via the undocumented CWM_GETISHELLBROWSER message.
//             SendMessageW to own windows on the same thread dispatches directly
//             to the window procedure — no queue, no blocking.
// Strategy 2: Fallback — enumerate ShellTabWindowClass children of the root
//             window when the focus has moved outside the file list.
static std::wstring GetSelectionPath(HDC /*hdc*/, HWND& outExplorerRoot)
{
    outExplorerRoot = NULL;
    HWND hFocus = NULL;
    GUITHREADINFO gti = { sizeof(GUITHREADINFO) };
    if (GetGUIThreadInfo(GetCurrentThreadId(), &gti))
        hFocus = gti.hwndFocus ? gti.hwndFocus : gti.hwndActive;
    if (!hFocus) hFocus = GetForegroundWindow();

    for (HWND h = hFocus; h; h = GetParent(h)) {
        auto* pSB = (IShellBrowser*)SendMessageW(h, CWM_GETISHELLBROWSER, 0, 0);
        if (pSB) {
            std::wstring path = ExtractPathFromBrowser(pSB);
            if (!path.empty()) {
                outExplorerRoot = GetAncestor(h, GA_ROOT);
                return path;
            }
        }
    }

    HWND hRoot = hFocus ? GetAncestor(hFocus, GA_ROOT) : NULL;
    if (hRoot) {
        HWND hTab = NULL;
        while ((hTab = FindWindowExW(hRoot, hTab, L"ShellTabWindowClass", NULL)) != NULL) {
            auto* pSB = (IShellBrowser*)SendMessageW(hTab, CWM_GETISHELLBROWSER, 0, 0);
            if (pSB) {
                std::wstring path = ExtractPathFromBrowser(pSB);
                if (!path.empty()) {
                    outExplorerRoot = hRoot;
                    return path;
                }
            }
        }
    }
    return L"";
}

// ═══════════════════════════════════════════════════════════════════════════════
// BLOCK 4: Metadata suffix builder
// ═══════════════════════════════════════════════════════════════════════════════

// Builds the status-bar suffix string for `filePath`.
// Called at most once per unique path (results are cached).
// Metadata priority for dimensions:
//   1. PKEY_Image_Dimensions  (Windows indexer — covers JPEG, PNG, BMP, …)
//   2. PKEY_Video_FrameWidth/Height (Windows indexer — covers MP4, AVI, …)
//   3. Custom binary parser   (SVG, HDR, EXR, TIFF/TX, TGA, WebM/MKV)
// Date: PSFormatForDisplayAlloc — correctly applies system timezone and DST,
//       avoiding the ±1 hour error from manual FileTimeToLocalFileTime chains.
// Bitrate: video property → audio property → file-size / duration estimate.
// FPS: PKEY_Video_FrameRate → EBML DefaultDuration fallback.
static std::wstring BuildMetadataSuffix(const std::wstring& filePath)
{
    IShellItem2* pItem2 = nullptr;
    if (FAILED(SHCreateItemFromParsingName(
            filePath.c_str(), nullptr, IID_PPV_ARGS(&pItem2))) || !pItem2)
    {
        Wh_Log(L"[Build] SHCreateItemFromParsingName failed for: %ls", filePath.c_str());
        return L"";
    }

    // Determine extension for custom parser dispatch
    std::wstring ext;
    size_t dot = filePath.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        ext = filePath.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    }

    // Run custom parser for formats not covered by the Windows Property System
    CustomMeta customMeta;
    if (ext == L"webm" || ext == L"mkv"  ||
        ext == L"tx"   || ext == L"tif"  || ext == L"tiff" ||
        ext == L"svg"  || ext == L"hdr"  || ext == L"exr"  || ext == L"tga")
    {
        customMeta = ParseCustomFormatMetadata(filePath, ext);
    }

    std::wstring dimStr, dateStr, typeStr;

    // 1. Dimensions
    LPWSTR pszDim = nullptr;
    if (SUCCEEDED(pItem2->GetString(PKEY_Image_Dimensions, &pszDim)) && pszDim) {
        dimStr = CleanPropString(pszDim);
        CoTaskMemFree(pszDim);
    }
    if (dimStr.empty()) {
        ULONG fw = 0, fh = 0;
        if (SUCCEEDED(pItem2->GetUInt32(PKEY_Video_FrameWidth,  &fw)) && fw > 0 &&
            SUCCEEDED(pItem2->GetUInt32(PKEY_Video_FrameHeight, &fh)) && fh > 0) {
            wchar_t buf[64];
            swprintf_s(buf, L"%u x %u", fw, fh);
            dimStr = buf;
        }
    }
    if (dimStr.empty() && customMeta.width > 0 && customMeta.height > 0) {
        wchar_t buf[64];
        swprintf_s(buf, L"%d x %d", customMeta.width, customMeta.height);
        dimStr = buf;
    }

    // 2. Modification date
    // PSFormatForDisplayAlloc handles timezone and DST conversion correctly.
    PROPVARIANT propVar;
    PropVariantInit(&propVar);
    if (SUCCEEDED(pItem2->GetProperty(PKEY_DateModified, &propVar))) {
        LPWSTR pszDate = nullptr;
        if (SUCCEEDED(PSFormatForDisplayAlloc(
                PKEY_DateModified, propVar, PDFF_DEFAULT, &pszDate)) && pszDate) {
            dateStr = CleanPropString(pszDate);
            CoTaskMemFree(pszDate);
        }
        PropVariantClear(&propVar);
    }

    // 3. File type description
    LPWSTR pszType = nullptr;
    if (SUCCEEDED(pItem2->GetString(PKEY_ItemTypeText, &pszType)) && pszType) {
        typeStr = CleanPropString(pszType);
        CoTaskMemFree(pszType);
    }

    // 4. Duration and derived media properties
    ULONGLONG duration100ns = 0;
    bool hasDuration = SUCCEEDED(pItem2->GetUInt64(PKEY_Media_Duration, &duration100ns))
                       && duration100ns > 0;

    std::wstring sfx;
    if (!dimStr.empty())  sfx += L"  \u2502  " + dimStr;
    if (!dateStr.empty()) sfx += L"  \u2502  " + dateStr;
    if (!typeStr.empty()) sfx += L"  \u2502  " + typeStr;

    if (hasDuration) {
        ULONG totalSec = (ULONG)(duration100ns / 10000000ULL);
        ULONG hh = totalSec / 3600;
        ULONG mm = (totalSec % 3600) / 60;
        ULONG ss = totalSec % 60;
        wchar_t durBuf[64];
        if (hh > 0) swprintf_s(durBuf, L"%02u:%02u:%02u", hh, mm, ss);
        else        swprintf_s(durBuf, L"%02u:%02u", mm, ss);
        sfx += L"  \u2502  " + std::wstring(durBuf);

        // Bitrate: video property → audio property → estimate from file size
        ULONG bitrateBps = 0;
        if (FAILED(pItem2->GetUInt32(PKEY_Video_TotalBitrate, &bitrateBps)) || !bitrateBps)
            pItem2->GetUInt32(PKEY_Audio_EncodingBitrate, &bitrateBps);
        if (bitrateBps == 0) {
            ULONGLONG fileSize = 0;
            if (SUCCEEDED(pItem2->GetUInt64(PKEY_Size, &fileSize)) && fileSize > 0) {
                double durationSec = (double)duration100ns / 10000000.0;
                if (durationSec > 0)
                    bitrateBps = (ULONG)((fileSize * 8.0) / durationSec);
            }
        }
        if (bitrateBps > 0) {
            wchar_t bitBuf[64];
            if (bitrateBps >= 1000000)
                swprintf_s(bitBuf, L"%.1f Mbps", (double)bitrateBps / 1000000.0);
            else
                swprintf_s(bitBuf, L"%u kbps", bitrateBps / 1000);
            sfx += L"  \u2502  " + std::wstring(bitBuf);
        }

        // FPS: Windows property system → EBML DefaultDuration fallback
        ULONG fps1000 = 0;
        if (!SUCCEEDED(pItem2->GetUInt32(PKEY_Video_FrameRate, &fps1000)) || !fps1000) {
            if (customMeta.fps > 0.0)
                fps1000 = (ULONG)(customMeta.fps * 1000.0 + 0.5);
        }
        if (fps1000 > 0) {
            wchar_t fpsBuf[64];
            if (fps1000 % 1000 == 0) swprintf_s(fpsBuf, L"%u fps", fps1000 / 1000);
            else                     swprintf_s(fpsBuf, L"%.2f fps", (double)fps1000 / 1000.0);
            sfx += L"  \u2502  " + std::wstring(fpsBuf);
        }
    }

    pItem2->Release();
    return sfx;
}

// ═══════════════════════════════════════════════════════════════════════════════
// BLOCK 5: Path-keyed cache with LRU-style eviction
// ═══════════════════════════════════════════════════════════════════════════════

// Removes the N oldest cache entries (by insertion order).
// Must be called with g_cs held.
static void EvictOldestEntries(size_t n)
{
    n = std::min(n, g_cacheOrder.size());
    for (size_t i = 0; i < n; ++i)
        g_metaCache.erase(g_cacheOrder[i]);
    g_cacheOrder.erase(g_cacheOrder.begin(), g_cacheOrder.begin() + (ptrdiff_t)n);
    Wh_Log(L"[Cache] Evicted %zu entries, remaining: %zu", n, g_metaCache.size());
}

// Main entry point called from both GDI hooks.
// Cache hit  → returns immediately with zero COM calls.
// Cache miss → calls BuildMetadataSuffix, stores result, returns.
// g_insideMetadataBuild guards against re-entrancy in the unlikely event that
// BuildMetadataSuffix or something it calls triggers a DrawTextW/GTEP redraw.
static std::wstring GetSuffixForStatusBar(HDC hdc)
{
    HWND hwndExplorerRoot = NULL;
    std::wstring path = GetSelectionPath(hdc, hwndExplorerRoot);
    if (path.empty()) return L"";

    thread_local bool g_insideMetadataBuild = false;
    if (g_insideMetadataBuild) return L"";

    if (!IsSafeToParse(path)) return L"";

    EnterCriticalSection(&g_cs);
    auto it = g_metaCache.find(path);
    if (it != g_metaCache.end()) {
        std::wstring result = it->second;
        LeaveCriticalSection(&g_cs);
        return result;
    }

    // Cache miss — evict if full before computing new entry
    if (g_metaCache.size() >= MAX_CACHE_SIZE)
        EvictOldestEntries(g_cacheEvictCount);

    LeaveCriticalSection(&g_cs);

    Wh_Log(L"[Cache] Miss — computing metadata for: %ls", path.c_str());

    g_insideMetadataBuild = true;
    std::wstring suffix = BuildMetadataSuffix(path);
    g_insideMetadataBuild = false;

    EnterCriticalSection(&g_cs);
    g_metaCache[path]  = suffix;
    g_cacheOrder.push_back(path);
    LeaveCriticalSection(&g_cs);

    return suffix;
}

// ═══════════════════════════════════════════════════════════════════════════════
// BLOCK 6: GDI hooks
// ═══════════════════════════════════════════════════════════════════════════════

// Both hooks follow the same pattern:
//  1. Detect "N items selected" status-bar text via IsSelectionStatus.
//  2. Retrieve (or compute) the metadata suffix.
//  3. Concatenate and forward to the original GDI function.
// thread_local string buffers avoid per-call heap allocations.

int WINAPI Hook_DTW(HDC hdc, LPCWSTR s, int cch, LPRECT rc, UINT fmt)
{
    UINT len = (cch < 0) ? (s ? (UINT)wcslen(s) : 0u) : (UINT)cch;
    if (!IsSelectionStatus(s, len))
        return g_origDTW(hdc, s, cch, rc, fmt);

    std::wstring sfx = GetSuffixForStatusBar(hdc);
    if (!sfx.empty()) {
        thread_local std::wstring t_full;
        t_full.assign(s, len);
        t_full.append(sfx);
        return g_origDTW(hdc, t_full.c_str(), (int)t_full.size(), rc, fmt);
    }
    return g_origDTW(hdc, s, cch, rc, fmt);
}

BOOL WINAPI Hook_GTEP(HDC hdc, LPCWSTR s, int cch, LPSIZE sz)
{
    UINT len = (cch < 0) ? (s ? (UINT)wcslen(s) : 0u) : (UINT)cch;
    if (!IsSelectionStatus(s, len))
        return g_origGTEP(hdc, s, cch, sz);

    std::wstring sfx = GetSuffixForStatusBar(hdc);
    if (!sfx.empty()) {
        thread_local std::wstring t_full;
        t_full.assign(s, len);
        t_full.append(sfx);
        return g_origGTEP(hdc, t_full.c_str(), (int)t_full.size(), sz);
    }
    return g_origGTEP(hdc, s, cch, sz);
}

// ═══════════════════════════════════════════════════════════════════════════════
// BLOCK 7: Mod init / uninit
// ═══════════════════════════════════════════════════════════════════════════════

BOOL Wh_ModInit()
{
    Wh_Log(L"[Init] Explorer Status Bar Metadata v1.3.0 starting");

    // Read settings (Windhawk guarantees these are available during Wh_ModInit)
    g_allowNetworkDrives   = Wh_GetIntSetting(L"allowNetworkDrives")   != 0;
    g_allowRemovableDrives = Wh_GetIntSetting(L"allowRemovableDrives") != 0;
    int evictRaw           = Wh_GetIntSetting(L"cacheEvictCount");
    g_cacheEvictCount      = (evictRaw >= 1 && evictRaw <= 50) ? (size_t)evictRaw : 10;

    Wh_Log(L"[Init] Settings: networkDrives=%d removableDrives=%d evictCount=%zu",
           (int)g_allowNetworkDrives, (int)g_allowRemovableDrives, g_cacheEvictCount);

    InitializeCriticalSection(&g_cs);

    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");

    if (!u32 || !gdi) {
        Wh_Log(L"[Init] FATAL: user32.dll or gdi32.dll not found");
        return FALSE;
    }

    void* pfnDTW  = reinterpret_cast<void*>(GetProcAddress(u32, "DrawTextW"));
    void* pfnGTEP = reinterpret_cast<void*>(GetProcAddress(gdi, "GetTextExtentPoint32W"));

    if (!pfnDTW || !pfnGTEP) {
        Wh_Log(L"[Init] FATAL: could not resolve DrawTextW or GetTextExtentPoint32W");
        return FALSE;
    }

    Wh_SetFunctionHook(pfnDTW,  (void*)Hook_DTW,  (void**)&g_origDTW);
    Wh_SetFunctionHook(pfnGTEP, (void*)Hook_GTEP, (void**)&g_origGTEP);

    Wh_Log(L"[Init] Hooks installed. DTW=%p GTEP=%p", pfnDTW, pfnGTEP);
    return TRUE;
}

void Wh_ModUninit()
{
    Wh_Log(L"[Uninit] Clearing cache (%zu entries) and shutting down",
           g_metaCache.size());

    EnterCriticalSection(&g_cs);
    g_metaCache.clear();
    g_cacheOrder.clear();
    LeaveCriticalSection(&g_cs);

    DeleteCriticalSection(&g_cs);
    Wh_Log(L"[Uninit] Done");
}
