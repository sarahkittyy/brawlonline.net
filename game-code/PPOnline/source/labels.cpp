// Labels drawn with the game's own font into the game's own textures (docs/design/rooms.md #2:
// "Labels are textures rendered from the game's own font ... No AI art").
//
// The font is Brawl's bold 32-pixel font (an RFNT, "RFNU" once loaded, in the MeleeFont heap: 32x32 cells, I4 glyph
// sheets, the heavy face of the character select's name plates), found once by scanning that
// heap. A label is laid out like the game's own button labels (rooms.md, the RE notes in
// docs/game-code.md): one or two lines, as large as the box allows, squeezed sideways (down to
// 0.6) before it is made smaller; each glyph is sampled bilinearly from its sheet into a coverage
// mask the size of the texture, the mask is grown by a round brush for the rim, and the result
// is written into the texture's own pixels in its own format and tiling:
//   RGB5A3 (the WITH FRIENDS buttons' labels MenMainWifi21/22/23): black letters, a white rim,
//          transparent elsewhere, as the game's labels are drawn;
//   I4     (the character select's "Seeking" MenSelchrWifi03): white letters (intensity is
//          colour and alpha there);
//   CMPR   (the panel states' "Selecting Character..." WifiInfWait05_0): white letters, a dark
//          rim, transparent elsewhere, in CMPR's 3-colour mode (black, white, grey, clear).
// Nothing is allocated for long: two masks from the Network heap, freed at once.
#include <OS/OSCache.h>
#include <gf/gf_heap_manager.h>
#include <memory.h>
#include <string.h>
#include "labels.h"

namespace Labels {

    static bool isPtr(u32 p) { return (p >= 0x80000000 && p < 0x81800000) || (p >= 0x90000000 && p < 0x94000000); }

    struct Font {
        u8* rfnt;
        u8* finf;
        u8* sheets;        // sheet images
        u32 sheetSize;
        u16 sheetNum, sheetFmt, rows, lines, sheetW, sheetH;
        u8 cellW, cellH;
        u16 alt;           // the glyph for unknown characters
        u32 cwdh, cmap;    // first CWDH / CMAP blocks (absolute)
    };
    static Font s_font;
    static bool s_fontTried = false;

    // g_HeapInfos (0x80494958): {name, pool, size, arena} x 0x48; MeleeFont is heap 0x30.
    static bool findFont()
    {
        if (s_font.rfnt) return true;
        u32* info = (u32*)(0x80494958 + 16 * 0x30);
        u32 start = info[1], size = info[2];
        if (!isPtr(start) || size < 0x100 || size > 0x200000) return false;
        for (u32 a = start; a + 0x60 < start + size; a += 4) {
            u32 magic = *(u32*)a;   // RFNU once the game has loaded it (RFNT on disc)
            if ((magic != 0x52464E55 && magic != 0x52464E54) || *(u16*)(a + 4) != 0xFEFF) continue;
            u8* finf = (u8*)a + 0x10;
            if (*(u32*)finf != 0x46494E46 /* FINF */) continue;
            u32 glyph = *(u32*)(finf + 0x10);
            if (!isPtr(glyph)) continue;
            u8* tglp = (u8*)(glyph - 8);
            if (*(u32*)tglp != 0x54474C50 /* TGLP */) continue;
            // the bold 32-pixel font, I4 sheets
            if (tglp[8] != 32 || tglp[9] != 32 || *(u16*)(tglp + 0x12) != 0) continue;
            Font f;
            f.rfnt = (u8*)a;
            f.finf = finf;
            f.cellW = tglp[8];
            f.cellH = tglp[9];
            f.sheetSize = *(u32*)(tglp + 0xC);
            f.sheetNum = *(u16*)(tglp + 0x10);
            f.sheetFmt = *(u16*)(tglp + 0x12);
            f.rows = *(u16*)(tglp + 0x14);
            f.lines = *(u16*)(tglp + 0x16);
            f.sheetW = *(u16*)(tglp + 0x18);
            f.sheetH = *(u16*)(tglp + 0x1A);
            f.sheets = *(u8**)(tglp + 0x1C);
            f.alt = *(u16*)(finf + 0xA);
            f.cwdh = *(u32*)(finf + 0x14);
            f.cmap = *(u32*)(finf + 0x18);
            if (!isPtr((u32)f.sheets) || !isPtr(f.cwdh) || !isPtr(f.cmap) || !f.rows || !f.lines) continue;
            s_font = f;
            return true;
        }
        return false;
    }

    static int glyphIndex(u16 code)
    {
        u32 p = s_font.cmap;
        for (int guard = 0; isPtr(p) && guard < 64; guard++) {
            u8* b = (u8*)p;
            u16 lo = *(u16*)b, hi = *(u16*)(b + 2), method = *(u16*)(b + 4);
            if (code >= lo && code <= hi) {
                if (method == 0) return *(u16*)(b + 0xC) + (code - lo);
                if (method == 1) return *(u16*)(b + 0xC + 2 * (code - lo));
                if (method == 2) {
                    u16 n = *(u16*)(b + 0xC);
                    u16* t = (u16*)(b + 0xE);
                    for (u16 i = 0; i < n; i++) {
                        if (t[2 * i] == code) return t[2 * i + 1];
                    }
                }
            }
            p = *(u32*)(b + 8);
        }
        return s_font.alt;
    }

    struct Width {
        s8 left;
        u8 glyph;
        s8 advance;
    };

    static Width glyphWidth(int idx)
    {
        u32 p = s_font.cwdh;
        for (int guard = 0; isPtr(p) && guard < 64; guard++) {
            u8* b = (u8*)p;
            u16 lo = *(u16*)b, hi = *(u16*)(b + 2);
            if (idx >= lo && idx <= hi) {
                u8* e = b + 8 + 3 * (idx - lo);
                Width w = {(s8)e[0], e[1], (s8)e[2]};
                return w;
            }
            p = *(u32*)(b + 4);
        }
        Width w = {(s8)s_font.finf[0xC], s_font.finf[0xD], (s8)s_font.finf[0xE]};
        return w;
    }

    // An I4 sheet's texel (0-255) at (x, y): 8x8 blocks, two texels a byte, high nibble first.
    static int texel(int sheet, int x, int y)
    {
        if (x < 0 || y < 0 || x >= s_font.sheetW || y >= s_font.sheetH) return 0;
        const u8* d = s_font.sheets + sheet * s_font.sheetSize;
        u32 off = ((y >> 3) * (s_font.sheetW >> 3) + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1);
        u8 v = d[off];
        return (x & 1) ? (v & 15) * 17 : (v >> 4) * 17;
    }

    static const int TRACK = -1;   // tighter than the font's own advance, as the game's labels
    static const int LINE_GAP = 2;
    static const int MAX_LINES = 2;
    static const int MAX_CHARS = 24;

    // Draw the coverage of `text` (lines split at '\n') into mask[w*h], fitted into the box.
    static void layout(u8* mask, int w, int h, const char* text, int bx0, int by0, int bx1, int by1)
    {
        int nl = 1;
        int idx[MAX_LINES][MAX_CHARS];
        Width wd[MAX_LINES][MAX_CHARS];
        int len[MAX_LINES] = {0, 0};
        int width[MAX_LINES] = {0, 0};
        for (const char* p = text; *p; p++) {
            if (*p == '\n') {
                if (nl < MAX_LINES) nl++;
                continue;
            }
            int l = nl - 1;
            if (len[l] >= MAX_CHARS) continue;
            int g = glyphIndex((u8)*p);
            idx[l][len[l]] = g;
            wd[l][len[l]] = glyphWidth(g);
            width[l] += wd[l][len[l]].advance + (len[l] ? TRACK : 0);
            len[l]++;
        }
        int maxW = 1;
        for (int l = 0; l < nl; l++) {
            if (width[l] > maxW) maxW = width[l];
        }
        float bw = (float)(bx1 - bx0), bh = (float)(by1 - by0);
        float th = (float)(s_font.cellH * nl + LINE_GAP * (nl - 1));
        float sy = bh / th, sx = sy;
        if (maxW * sx > bw) {
            sx = bw / maxW;
            if (sx < sy * 0.6f) {
                sx = sy * 0.6f;
                if (maxW * sx > bw) {
                    sy *= bw / (maxW * sx);
                    sx = bw / maxW;
                }
            }
        }
        float top = by0 + (bh - th * sy) * 0.5f;
        int perSheet = s_font.rows * s_font.lines;
        for (int l = 0; l < nl; l++) {
            float lx = bx0 + (bw - width[l] * sx) * 0.5f;
            float ly = top + l * (s_font.cellH + LINE_GAP) * sy;
            float pen = 0;
            for (int i = 0; i < len[l]; i++) {
                int g = idx[l][i];
                int sheet = g / perSheet, cell = g % perSheet;
                int cx = (cell % s_font.rows) * (s_font.cellW + 1);
                int cy = (cell / s_font.rows) * (s_font.cellH + 1);
                const Width& gw = wd[l][i];
                float gx0 = lx + (pen + gw.left) * sx;
                int X0 = (int)gx0 - 1, X1 = (int)(gx0 + gw.glyph * sx) + 2;
                int Y0 = (int)ly - 1, Y1 = (int)(ly + s_font.cellH * sy) + 2;
                for (int Y = Y0; Y < Y1; Y++) {
                    if (Y < 0 || Y >= h) continue;
                    float v = (Y + 0.5f - ly) / sy - 0.5f;
                    int iv = (int)(v + 8.0f) - 8;
                    float dv = v - iv;
                    for (int X = X0; X < X1; X++) {
                        if (X < 0 || X >= w) continue;
                        float u = (X + 0.5f - gx0) / sx - 0.5f;
                        int iu = (int)(u + 8.0f) - 8;
                        float du = u - iu;
                        int t00 = 0, t10 = 0, t01 = 0, t11 = 0;
                        if (iu >= 0 && iu < gw.glyph && iv >= 0 && iv < s_font.cellH) t00 = texel(sheet, cx + iu, cy + iv);
                        if (iu + 1 < gw.glyph && iu + 1 >= 0 && iv >= 0 && iv < s_font.cellH) t10 = texel(sheet, cx + iu + 1, cy + iv);
                        if (iu >= 0 && iu < gw.glyph && iv + 1 < s_font.cellH && iv + 1 >= 0) t01 = texel(sheet, cx + iu, cy + iv + 1);
                        if (iu + 1 < gw.glyph && iu + 1 >= 0 && iv + 1 < s_font.cellH && iv + 1 >= 0) t11 = texel(sheet, cx + iu + 1, cy + iv + 1);
                        float s = t00 * (1 - du) * (1 - dv) + t10 * du * (1 - dv) + t01 * (1 - du) * dv + t11 * du * dv;
                        int c = (int)s;
                        if (c > 255) c = 255;
                        u8& m = mask[Y * w + X];
                        if (c > m) m = (u8)c;
                    }
                }
                pen += gw.advance + TRACK;
            }
        }
    }

    // The rim: the mask grown by a round brush of radius `r` (pixels).
    static void grow(const u8* in, u8* out, int w, int h, int r)
    {
        int r2 = (r * 2 + 1) * (r * 2 + 1);   // (r + 0.5)^2 * 4
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                int m = 0;
                for (int dy = -r; dy <= r; dy++) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    for (int dx = -r; dx <= r; dx++) {
                        if (4 * (dx * dx + dy * dy) > r2) continue;
                        int xx = x + dx;
                        if (xx < 0 || xx >= w) continue;
                        int v = in[yy * w + xx];
                        if (v > m) m = v;
                    }
                }
                out[y * w + x] = (u8)m;
            }
        }
    }

    static bool renderImpl(u8* tex0, const char* text, int x0, int y0, int x1, int y1, int rim, int boxOnly);

    bool render(u8* tex0, const char* text, int x0, int y0, int x1, int y1, int rim)
    {
        return renderImpl(tex0, text, x0, y0, x1, y1, rim, 0);
    }

    bool renderInBox(u8* tex0, const char* text, int x0, int y0, int x1, int y1, int rim)
    {
        return renderImpl(tex0, text, x0, y0, x1, y1, rim, 1);
    }

    static bool renderImpl(u8* tex0, const char* text, int x0, int y0, int x1, int y1, int rim, int boxOnly)
    {
        if (!isPtr((u32)tex0) || *(u32*)tex0 != 0x54455830 /* TEX0 */) return false;
        if (!findFont()) return false;
        int w = *(u16*)(tex0 + 0x1C), h = *(u16*)(tex0 + 0x1E);
        u32 fmt = *(u32*)(tex0 + 0x20);
        u8* data = tex0 + *(u32*)(tex0 + 0x10);
        if (w <= 0 || h <= 0 || w > 512 || h > 256) return false;
        if (fmt != 5 && fmt != 0 && fmt != 14) return false;
        // Textures are stored in whole blocks (RGB5A3 4x4, I4 and CMPR 8x8): a size that is no
        // multiple of the block (the 88x20 "Seeking") has padding blocks at the right and the bottom.
        const int bw = fmt == 5 ? 4 : 8, bh = fmt == 5 ? 4 : 8;
        const int bpr = (w + bw - 1) / bw;   // blocks per row
        u32 n = (u32)(w * h);
        u8* mask = (u8*)gfHeapManager::alloc(Heaps::Network, n * 2);
        if (!mask) return false;
        u8* rimMask = mask + n;
        memset(mask, 0, n);
        layout(mask, w, h, text, x0, y0, x1, y1);
        if (fmt == 5) {
            grow(mask, rimMask, w, h, rim);
            // RGB5A3, 4x4 blocks of u16: opaque 1rrrrrgggggbbbbb, else 0aaarrrrggggbbbb.
            u16* out = (u16*)data;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    int m = mask[y * w + x], o = rimMask[y * w + x] * 2;
                    if (o > 255) o = 255;
                    int a = m > o ? m : o;
                    int c = 255 - m;            // black letters on a white rim
                    u16 v;
                    if (a >= 0xF0) {
                        int c5 = c >> 3;
                        v = (u16)(0x8000 | (c5 << 10) | (c5 << 5) | c5);
                    } else {
                        int c4 = c >> 4;
                        v = (u16)(((a >> 5) << 12) | (c4 << 8) | (c4 << 4) | c4);
                    }
                    u32 blk = (u32)((y >> 2) * bpr + (x >> 2));
                    out[blk * 16 + (y & 3) * 4 + (x & 3)] = v;
                }
            }
            DCFlushRange(data, (u32)(bpr * ((h + bh - 1) / bh) * 32));
        } else if (fmt == 14) {
            // CMPR, 8x8 blocks of four 4x4 sub-blocks {u16 c0, u16 c1, u32 2-bit indices}. With
            // c0 = black <= c1 = white the palette is black, white, grey, transparent.
            grow(mask, rimMask, w, h, rim);
            const int bpc = (h + bh - 1) / bh;
            for (int by = 0; by < bpc; by++) {
                for (int bx = 0; bx < bpr; bx++) {
                    u8* blk = data + (by * bpr + bx) * 32;
                    for (int sb = 0; sb < 4; sb++) {
                        int sx = bx * 8 + (sb & 1) * 4, sy = by * 8 + (sb >> 1) * 4;
                        // In a box (a button's own art around it): only the sub-blocks inside it,
                        // white letters on a black outline as Brawl's button labels.
                        if (boxOnly && (sx < x0 || sy < y0 || sx + 4 > x1 || sy + 4 > y1)) continue;
                        u32 letter = boxOnly ? 1 : 0, outline = boxOnly ? 0 : 1;
                        u32 idx = 0;
                        for (int yy = 0; yy < 4; yy++) {
                            for (int xx = 0; xx < 4; xx++) {
                                int x = sx + xx, y = sy + yy;
                                u32 k = 3;
                                if (x < w && y < h) {
                                    int m = mask[y * w + x], o = rimMask[y * w + x];
                                    if (m >= 0xA0) k = letter;
                                    else if (m >= 0x50) k = 2;
                                    else if (o >= 0x40 || m >= 0x20) k = outline;
                                }
                                idx = (idx << 2) | k;
                            }
                        }
                        u8* e = blk + sb * 8;
                        *(u16*)e = 0x0000;
                        *(u16*)(e + 2) = 0xFFFF;
                        *(u32*)(e + 4) = idx;
                    }
                }
            }
            DCFlushRange(data, (u32)(bpr * bpc * 32));
        } else {
            // I4, 8x8 blocks, two texels a byte.
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x += 2) {
                    int a = mask[y * w + x] >> 4, b = x + 1 < w ? mask[y * w + x + 1] >> 4 : 0;
                    u32 blk = (u32)((y >> 3) * bpr + (x >> 3));
                    data[blk * 32 + (y & 7) * 4 + ((x & 7) >> 1)] = (u8)((a << 4) | b);
                }
            }
            DCFlushRange(data, (u32)(bpr * ((h + bh - 1) / bh) * 32));
        }
        free(mask);
        return true;
    }

    int rebindTexture(u8* mdl0, const char* name, u8* tex0)
    {
        if (!isPtr((u32)mdl0) || *(u32*)mdl0 != 0x4D444C30 /* MDL0 */) return 0;
        s32 off = *(s32*)(mdl0 + 0x10 + 9 * 4);
        if (off <= 0) return 0;
        u8* dic = mdl0 + off;
        u32 count = *(u32*)(dic + 4);
        int n = 0;
        for (u32 i = 1; i <= count && i < 256; i++) {
            u8* e = dic + 8 + 16 * i;
            if (strcmp((const char*)(dic + *(s32*)(e + 8)), name) != 0) continue;
            u8* link = dic + *(s32*)(e + 12);
            u32 pairs = *(u32*)link;   // {material, texture info} offsets from the link
            for (u32 k = 0; k < pairs && k < 16; k++) {
                u8* info = link + *(s32*)(link + 8 + 8 * k);
                *(u8**)(info + 8) = tex0;
                n++;
            }
        }
        return n;
    }

    // An I4 texel (0-15) of a w-wide image: 8x8 blocks, two texels a byte, high nibble first.
    static int i4Get(const u8* data, int w, int x, int y)
    {
        int bpr = (w + 7) / 8;
        u8 b = data[((y >> 3) * bpr + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
        return (x & 1) ? (b & 15) : (b >> 4);
    }

    static void i4Set(u8* data, int w, int x, int y, int v)
    {
        int bpr = (w + 7) / 8;
        u8* b = &data[((y >> 3) * bpr + (x >> 3)) * 32 + (y & 7) * 4 + ((x & 7) >> 1)];
        *b = (x & 1) ? (u8)((*b & 0xF0) | v) : (u8)((*b & 0x0F) | (v << 4));
    }

    bool mirroredIcon(u8* dst, const u8* src)
    {
        if (*(u32*)dst != 0x54455830 || *(u32*)src != 0x54455830) return false;
        if (*(u32*)(dst + 0x20) != 0 || *(u32*)(src + 0x20) != 0) return false;   // I4 only
        int dw = *(u16*)(dst + 0x1C), dh = *(u16*)(dst + 0x1E);
        int sw = *(u16*)(src + 0x1C), sh = *(u16*)(src + 0x1E);
        u8* dd = dst + *(u32*)(dst + 0x10);
        const u8* sd = src + *(u32*)(src + 0x10);
        int fw = sw * 2;   // the whole icon: the half and its mirror image
        // Tilted 30 degrees (mouth to the upper left), as the spectator button draws it, and
        // fitted into the texture: the tilted icon's bounding box, scaled, centred.
        const float C = 0.8660254f, S = 0.5f;
        // The drawn texels' extent once tilted (the half-icon has empty margins).
        float scx = fw * 0.5f, scy = sh * 0.5f;
        float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
        for (int v = 0; v < sh; v++) {
            for (int u = 0; u < fw; u++) {
                if (i4Get(sd, sw, u < sw ? u : fw - 1 - u, v) < 4) continue;
                float ox = u + 0.5f - scx, oy = v + 0.5f - scy;
                float rx = ox * C + oy * S, ry = -ox * S + oy * C;   // forward of the mapping below
                if (rx < minx) minx = rx;
                if (rx > maxx) maxx = rx;
                if (ry < miny) miny = ry;
                if (ry > maxy) maxy = ry;
            }
        }
        if (maxx <= minx || maxy <= miny) return false;
        float scale = (dw - 4) / (maxx - minx);
        if ((dh - 4) / (maxy - miny) < scale) scale = (dh - 4) / (maxy - miny);
        // centre the extent: the texture centre maps to the extent's centre
        float cx = dw * 0.5f - (minx + maxx) * 0.5f * scale, cy = dh * 0.5f - (miny + maxy) * 0.5f * scale;
        u32 bytes = (u32)(((dw + 7) / 8) * ((dh + 7) / 8) * 32);
        memset(dd, 0, bytes);
        for (int y = 0; y < dh; y++) {
            for (int x = 0; x < dw; x++) {
                int sum = 0;
                for (int sub = 0; sub < 4; sub++) {   // 2x2 samples a texel
                    float px = (x + 0.25f + 0.5f * (sub & 1) - cx) / scale;
                    float py = (y + 0.25f + 0.5f * (sub >> 1) - cy) / scale;
                    // rotate back (the icon is turned counterclockwise on screen, y down)
                    float u = px * C - py * S + scx, v = px * S + py * C + scy;
                    int iu = (int)u, iv = (int)v;
                    if (u < 0 || v < 0 || iu >= fw || iv >= sh) continue;
                    int su = iu < sw ? iu : fw - 1 - iu;
                    sum += i4Get(sd, sw, su, iv);
                }
                i4Set(dd, dw, x, y, (sum + 2) / 4);
            }
        }
        // One texel bolder (the icon is drawn small in the circle, as the spiral): a 3x3 max,
        // from a copy of the rows above and the current one.
        u8 rows[3][128];
        for (int y = 0; y < dh && dw <= 128; y++) {
            for (int x = 0; x < dw; x++) {
                rows[0][x] = y > 0 ? rows[1][x] : 0;
                rows[1][x] = (u8)i4Get(dd, dw, x, y);
            }
            for (int x = 0; x < dw; x++) rows[2][x] = y + 1 < dh ? (u8)i4Get(dd, dw, x, y + 1) : 0;
            for (int x = 0; x < dw; x++) {
                int m = 0;
                for (int r = 0; r < 3; r++) {
                    for (int k = x - 1; k <= x + 1; k++) {
                        if (k >= 0 && k < dw && rows[r][k] > m) m = rows[r][k];
                    }
                }
                i4Set(dd, dw, x, y, m);
            }
        }
        DCFlushRange(dd, bytes);
        return true;
    }

    // The TEX0 bound to `name` in a model's materials: MDL0 section 9 (texture links, a ResDic)
    // -> the link's first {material, texture info} pair -> the texture info's bound TEX0 (+8).
    u8* boundTexture(u8* mdl0, const char* name)
    {
        if (!isPtr((u32)mdl0) || *(u32*)mdl0 != 0x4D444C30 /* MDL0 */) return NULL;
        s32 off = *(s32*)(mdl0 + 0x10 + 9 * 4);
        if (off <= 0) return NULL;
        u8* dic = mdl0 + off;
        u32 count = *(u32*)(dic + 4);
        for (u32 i = 1; i <= count && i < 256; i++) {
            u8* e = dic + 8 + 16 * i;
            const char* nm = (const char*)(dic + *(s32*)(e + 8));
            if (strcmp(nm, name) != 0) continue;
            u8* link = dic + *(s32*)(e + 12);
            if (*(u32*)link == 0) return NULL;
            u8* info = link + *(s32*)(link + 8);
            u8* tex = *(u8**)(info + 8);
            return isPtr((u32)tex) && *(u32*)tex == 0x54455830 ? tex : NULL;
        }
        return NULL;
    }
}
