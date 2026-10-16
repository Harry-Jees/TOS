// Hawk OS - image buffers, scaling, colour search, PNG/BMP writing, base64.
//
// PNG writing has two paths and no third-party libraries: Windows Imaging
// Component (built into Windows) compresses properly, and a hand-written
// encoder that emits stored (uncompressed) deflate blocks is the fallback.
#include "hawk.h"

#include <objbase.h>
#include <wincodec.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace hawk {

// ------------------------------------------------------------------- CRC32 --
static uint32_t crc_table[256];
static bool crc_ready = false;

static void crc_init() {
    if (crc_ready) return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = true;
}

static uint32_t crc32_of(const uint8_t* d, size_t n) {
    crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t adler32_of(const uint8_t* d, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + d[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void put32be(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x >> 24));
    v.push_back((uint8_t)(x >> 16));
    v.push_back((uint8_t)(x >> 8));
    v.push_back((uint8_t)x);
}

static void put_chunk(std::vector<uint8_t>& out, const char type[4],
                      const std::vector<uint8_t>& data) {
    put32be(out, (uint32_t)data.size());
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    uint32_t crc = crc32_of(out.data() + start, data.size() + 4);
    put32be(out, crc);
}

// --------------------------------------------------------------- encoding --
// A self-contained encoder: always available, valid everywhere, uncompressed.
static bool save_png_plain(const Image& img, const std::string& path) {
    if (img.empty()) return false;

    // filter byte 0 + RGBA (the buffer is BGRA, so swap R and B)
    std::vector<uint8_t> raw;
    raw.reserve((size_t)img.h * (1 + (size_t)img.w * 4));
    for (int y = 0; y < img.h; y++) {
        raw.push_back(0);
        const uint8_t* p = img.row(y);
        for (int x = 0; x < img.w; x++, p += 4) {
            raw.push_back(p[2]);
            raw.push_back(p[1]);
            raw.push_back(p[0]);
            raw.push_back(0xFF);
        }
    }

    std::vector<uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        size_t n = raw.size() - off;
        if (n > 65535) n = 65535;
        bool last = (off + n == raw.size());
        z.push_back(last ? 0x01 : 0x00);
        z.push_back((uint8_t)(n & 0xFF));
        z.push_back((uint8_t)(n >> 8));
        uint16_t inv = (uint16_t)(~n);
        z.push_back((uint8_t)(inv & 0xFF));
        z.push_back((uint8_t)(inv >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    }
    put32be(z, adler32_of(raw.data(), raw.size()));

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    put32be(ihdr, (uint32_t)img.w);
    put32be(ihdr, (uint32_t)img.h);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // truecolour with alpha
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    put_chunk(png, "IHDR", ihdr);
    put_chunk(png, "IDAT", z);
    put_chunk(png, "IEND", {});

    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t wrote = fwrite(png.data(), 1, png.size(), f);
    fclose(f);
    return wrote == png.size();
}

// The preferred path: WIC compresses, so a screenshot that would be 5 MB raw
// becomes a few hundred KB - which matters when one has to be sent to a model.
template <class T>
static void release(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

static bool save_png_wic(const Image& img, const std::string& path) {
    if (img.empty()) return false;
    // COM is per thread, and the agent runs captures on a worker thread.
    static thread_local bool com_ready = false;
    if (!com_ready) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        com_ready = true;
    }

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory))) ||
        !factory)
        return false;

    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    bool ok = false;
    std::wstring wpath = wide(path);
    do {
        if (FAILED(factory->CreateStream(&stream))) break;
        if (FAILED(stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE))) break;
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) break;
        if (FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) break;
        if (FAILED(encoder->CreateNewFrame(&frame, &props))) break;
        if (FAILED(frame->Initialize(props))) break;
        if (FAILED(frame->SetSize((UINT)img.w, (UINT)img.h))) break;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&fmt))) break;
        if (!IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA)) break;  // would need a conversion
        UINT stride = (UINT)img.w * 4;
        if (FAILED(frame->WritePixels((UINT)img.h, stride, (UINT)img.px.size(),
                                      const_cast<BYTE*>(img.px.data()))))
            break;
        if (FAILED(frame->Commit())) break;
        if (FAILED(encoder->Commit())) break;
        ok = true;
    } while (false);

    release(props);
    release(frame);
    release(encoder);
    release(stream);
    release(factory);
    if (!ok) DeleteFileW(wpath.c_str());  // never leave a half-written shot behind
    return ok;
}

bool save_png(const Image& img, const std::string& path) {
    // HAWK_NO_WIC forces the dependency-free encoder, which is handy when
    // checking that the fallback path still produces a readable file.
    static const bool no_wic = getenv("HAWK_NO_WIC") != nullptr;
    if (!no_wic && save_png_wic(img, path)) return true;
    return save_png_plain(img, path);
}

bool save_bmp(const Image& img, const std::string& path) {
    if (img.empty()) return false;
    const uint32_t header = 14 + 40;
    const uint32_t size = header + (uint32_t)img.px.size();
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    uint8_t fh[14] = {'B', 'M'};
    memcpy(fh + 2, &size, 4);
    uint32_t off = header;
    memcpy(fh + 10, &off, 4);
    fwrite(fh, 1, 14, f);
    uint8_t ih[40] = {0};
    int32_t w = img.w, h = -img.h;  // negative height = top-down rows
    memcpy(ih, &w, 4);
    memcpy(ih + 4, &h, 4);
    uint16_t planes = 1, bpp = 32;
    memcpy(ih + 12, &planes, 2);
    memcpy(ih + 14, &bpp, 2);
    uint32_t raw = (uint32_t)img.px.size();
    memcpy(ih + 20, &raw, 4);
    fwrite(ih, 1, 40, f);
    fwrite(img.px.data(), 1, img.px.size(), f);
    fclose(f);
    return true;
}

std::string b64_encode(const std::vector<uint8_t>& data) {
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back(tbl[v & 63]);
    }
    if (i + 1 == data.size()) {
        uint32_t v = data[i] << 16;
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out += "==";
    } else if (i + 2 == data.size()) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(tbl[(v >> 18) & 63]);
        out.push_back(tbl[(v >> 12) & 63]);
        out.push_back(tbl[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

// ------------------------------------------------------------- operations --
Image crop(const Image& src, int x, int y, int w, int h) {
    Image out;
    if (src.empty() || w <= 0 || h <= 0) return out;
    out.w = w;
    out.h = h;
    out.px.assign((size_t)w * h * 4, 0);
    for (int j = 0; j < h; j++) {
        int sy = y + j;
        if (sy < 0 || sy >= src.h) continue;
        for (int i = 0; i < w; i++) {
            int sx = x + i;
            if (sx < 0 || sx >= src.w) continue;
            memcpy(out.row(j) + (size_t)i * 4, src.row(sy) + (size_t)sx * 4, 4);
        }
    }
    return out;
}

Image scale_to_fit(const Image& src, int max_w, int max_h) {
    if (src.empty() || max_w <= 0 || max_h <= 0) return Image{};
    if (src.w <= max_w && src.h <= max_h) return src;

    double ratio = src.w / (double)src.h;
    int dw = max_w, dh = (int)(max_w / ratio);
    if (dh > max_h) {
        dh = max_h;
        dw = (int)(max_h * ratio);
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    Image out;
    out.w = dw;
    out.h = dh;
    out.px.assign((size_t)dw * dh * 4, 0);

    // box average when shrinking keeps text readable for the vision model
    double sx_step = src.w / (double)dw;
    double sy_step = src.h / (double)dh;
    for (int y = 0; y < dh; y++) {
        int y0 = (int)(y * sy_step), y1 = (int)((y + 1) * sy_step);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > src.h) y1 = src.h;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)(x * sx_step), x1 = (int)((x + 1) * sx_step);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > src.w) x1 = src.w;
            uint32_t b = 0, g = 0, r = 0, n = 0;
            for (int j = y0; j < y1; j++) {
                const uint8_t* p = src.row(j) + (size_t)x0 * 4;
                for (int i = x0; i < x1; i++, p += 4) {
                    b += p[0];
                    g += p[1];
                    r += p[2];
                    n++;
                }
            }
            if (!n) n = 1;
            uint8_t* q = out.row(y) + (size_t)x * 4;
            q[0] = (uint8_t)(b / n);
            q[1] = (uint8_t)(g / n);
            q[2] = (uint8_t)(r / n);
            q[3] = 0xFF;
        }
    }
    return out;
}

Image draw_marker(const Image& src, int x, int y) {
    Image out = src;
    if (out.empty()) return out;
    const uint32_t ink = 0x0020FF3C;  // green crosshair (BGR)
    for (int i = -8; i <= 8; i++) {
        out.set(x + i, y, ink);
        out.set(x, y + i, ink);
    }
    for (int i = -2; i <= 2; i++)
        for (int j = -2; j <= 2; j++)
            if (abs(i) + abs(j) >= 3) out.set(x + i, y + j, 0x00303030);
    return out;
}

std::string color_hex(uint32_t bgr) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02X%02X%02X", bgr & 0xFF, (bgr >> 8) & 0xFF, (bgr >> 16) & 0xFF);
    return buf;
}

uint32_t parse_color(const std::string& hex) {
    unsigned r = 0, g = 0, b = 0;
    std::string h = hex;
    if (!h.empty() && h[0] == '#') h = h.substr(1);
    if (h.size() == 3) {
        h = std::string(2, h[0]) + std::string(2, h[1]) + std::string(2, h[2]);
    }
    if (h.size() != 6) return 0xFFFFFFFFu;
    if (sscanf(h.c_str(), "%2x%2x%2x", &r, &g, &b) != 3) return 0xFFFFFFFFu;
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}

bool find_color(const Image& img, uint32_t bgr, int tolerance, Point* out) {
    if (img.empty()) return false;
    uint8_t tr = (uint8_t)(bgr & 0xFF), tg = (uint8_t)((bgr >> 8) & 0xFF), tb = (uint8_t)((bgr >> 16) & 0xFF);
    long best = -1;
    Point best_pt{};
    for (int y = 0; y < img.h; y++) {
        const uint8_t* p = img.row(y);
        for (int x = 0; x < img.w; x++, p += 4) {
            int d = abs((int)p[0] - tb) + abs((int)p[1] - tg) + abs((int)p[2] - tr);
            if (d == 0) {
                if (out) *out = Point{x, y};
                return true;
            }
            if (tolerance > 0 && d <= tolerance && (best < 0 || d < best)) {
                best = d;
                best_pt = Point{x, y};
            }
        }
    }
    if (best >= 0) {
        if (out) *out = best_pt;
        return true;
    }
    return false;
}

// ------------------------------------------------------ vision primitives --
// Local analysis of real pixels. No model, no network, no guessing: these are
// the functions that let a script see a screenshot well enough to act on it.

static int color_dist(uint32_t a, uint32_t b) {
    return abs((int)(a & 0xFF) - (int)(b & 0xFF)) +
           abs((int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF)) +
           abs((int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF));
}

// Mean colour of a box (clamped to the image), 0 when the box is empty.
static uint32_t box_mean(const Image& img, int x0, int y0, int x1, int y1) {
    long r = 0, g = 0, b = 0, n = 0;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > img.w) x1 = img.w;
    if (y1 > img.h) y1 = img.h;
    for (int y = y0; y < y1; y++) {
        const uint8_t* p = img.row(y) + (size_t)x0 * 4;
        for (int x = x0; x < x1; x++, p += 4) {
            b += p[0];
            g += p[1];
            r += p[2];
            n++;
        }
    }
    if (!n) return 0;
    return (uint32_t)(b / n) | ((uint32_t)(g / n) << 8) | ((uint32_t)(r / n) << 16);
}

std::vector<std::pair<uint32_t, int>> dominant_colors(const Image& img, int top_n) {
    std::vector<std::pair<uint32_t, int>> out;
    if (img.empty()) return out;
    // Quantise to 5 bits per channel (32768 buckets), counting a 2x2 sample.
    static long counts[32768];
    static long sr[32768], sg[32768], sb[32768];
    static bool cleared = false;
    if (!cleared) {
        memset(counts, 0, sizeof(counts));
        memset(sr, 0, sizeof(sr));
        memset(sg, 0, sizeof(sg));
        memset(sb, 0, sizeof(sb));
        cleared = true;
    } else {
        for (long* p : {counts, sr, sg, sb}) memset(p, 0, sizeof(long) * 32768);
    }
    for (int y = 0; y < img.h; y += 2) {
        const uint8_t* p = img.row(y);
        for (int x = 0; x < img.w; x += 2, p += 8) {
            uint32_t c = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
            int key = (((c >> 3) & 0x1F) << 10) | (((c >> 11) & 0x1F) << 5) | ((c >> 19) & 0x1F);
            counts[key]++;
            sb[key] += p[0];
            sg[key] += p[1];
            sr[key] += p[2];
        }
    }
    std::vector<std::pair<int, long>> by_count;
    for (int k = 0; k < 32768; k++)
        if (counts[k] > 0) by_count.push_back({k, counts[k]});
    std::sort(by_count.begin(), by_count.end(),
              [](const std::pair<int, long>& a, const std::pair<int, long>& b) {
                  return a.second > b.second;
              });
    for (size_t i = 0; i < by_count.size() && (int)out.size() < top_n; i++) {
        int k = by_count[i].first;
        long n = counts[k];
        uint32_t bgr = (uint32_t)(sb[k] / n) | ((uint32_t)(sg[k] / n) << 8) |
                       ((uint32_t)(sr[k] / n) << 16);
        out.push_back({bgr, (int)(by_count[i].second * 4)});  // counts were sampled 1-in-4
    }
    return out;
}

std::vector<UiElement> detect_elements(const Image& img, int max_elements) {
    std::vector<UiElement> out;
    if (img.empty() || img.w < 96 || img.h < 96 || max_elements <= 0) return out;

    // 1. Split the image into small cells and record each cell's mean colour
    //    and how uniform it is. UI chrome is made of flat regions; photographs
    //    and gradients are not, which is exactly the distinction we want.
    const int C = 6;  // cell size in pixels
    int gw = img.w / C, gh = img.h / C;
    if (gw < 10 || gh < 10) return out;
    std::vector<uint32_t> mean((size_t)gw * gh, 0);
    std::vector<uint8_t> flat((size_t)gw * gh, 0);
    for (int cy = 0; cy < gh; cy++) {
        for (int cx = 0; cx < gw; cx++) {
            int x0 = cx * C + 1, x1 = (cx + 1) * C - 1;
            int y0 = cy * C + 1, y1 = (cy + 1) * C - 1;
            uint32_t first = img.at(x0, y0);
            int worst = 0, n = 0;
            long r = 0, g = 0, b = 0;
            for (int y = y0; y < y1; y += 2) {
                const uint8_t* p = img.row(y) + (size_t)x0 * 4;
                for (int x = x0; x < x1; x += 2, p += 8) {
                    uint32_t c = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
                    b += p[0];
                    g += p[1];
                    r += p[2];
                    n++;
                    int d = color_dist(c, first);
                    if (d > worst) worst = d;
                }
            }
            if (!n) n = 1;
            size_t idx = (size_t)cy * gw + cx;
            mean[idx] = (uint32_t)(b / n) | ((uint32_t)(g / n) << 8) | ((uint32_t)(r / n) << 16);
            flat[idx] = (uint8_t)(worst > 255 ? 255 : worst);
        }
    }

    // 2. Background = the most common colour among the flat cells.
    std::vector<std::pair<uint32_t, int>> dom;
    {
        std::map<uint32_t, int> tally;
        for (size_t i = 0; i < mean.size(); i++)
            if (flat[i] <= 40) tally[mean[i] & 0xF8F8F8u]++;
        std::vector<std::pair<uint32_t, int>> v(tally.begin(), tally.end());
        std::sort(v.begin(), v.end(), [](const std::pair<uint32_t, int>& a,
                                         const std::pair<uint32_t, int>& b) {
            return a.second > b.second;
        });
        for (size_t i = 0; i < v.size() && i < 6; i++) dom.push_back(v[i]);
    }
    uint32_t bg = dom.empty() ? 0x00282828u : dom[0].first;

    // 3. Group neighbouring flat cells of similar colour into components.
    std::vector<int> label(mean.size(), -1);
    struct Comp {
        int minx = 1 << 30, miny = 1 << 30, maxx = -1, maxy = -1, cells = 0;
        long r = 0, g = 0, b = 0;
    };
    std::vector<Comp> comps;
    std::vector<int> stack;
    for (int cy = 0; cy < gh; cy++) {
        for (int cx = 0; cx < gw; cx++) {
            size_t start = (size_t)cy * gw + cx;
            if (label[start] >= 0 || flat[start] > 40) continue;
            int id = (int)comps.size();
            comps.push_back(Comp{});
            stack.clear();
            stack.push_back((int)start);
            label[start] = id;
            while (!stack.empty()) {
                int cur = stack.back();
                stack.pop_back();
                int x = cur % gw, y = cur / gw;
                Comp& c = comps[id];
                c.cells++;
                c.b += mean[cur] & 0xFF;
                c.g += (mean[cur] >> 8) & 0xFF;
                c.r += (mean[cur] >> 16) & 0xFF;
                if (x < c.minx) c.minx = x;
                if (y < c.miny) c.miny = y;
                if (x > c.maxx) c.maxx = x;
                if (y > c.maxy) c.maxy = y;
                const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
                for (int k = 0; k < 4; k++) {
                    if (nx[k] < 0 || ny[k] < 0 || nx[k] >= gw || ny[k] >= gh) continue;
                    int ni = ny[k] * gw + nx[k];
                    if (label[ni] >= 0 || flat[ni] > 40) continue;
                    if (color_dist(mean[ni], mean[cur]) > 36) continue;
                    label[ni] = id;
                    stack.push_back(ni);
                }
            }
        }
    }

    // 4. Turn the solid components into elements and score how control-like
    //    each one looks: solid, saturated, the right size, not the whole
    //    desktop (that is just a background panel).
    for (const Comp& c : comps) {
        int w = (c.maxx - c.minx + 1) * C, h = (c.maxy - c.miny + 1) * C;
        if (w < 44 || h < 14 || w > img.w * 3 / 4 || h > img.h * 3 / 4) continue;
        double fill = (double)c.cells / ((double)(c.maxx - c.minx + 1) * (c.maxy - c.miny + 1));
        if (fill < 0.72) continue;
        uint32_t color = (uint32_t)(c.b / c.cells) | ((uint32_t)(c.g / c.cells) << 8) |
                         ((uint32_t)(c.r / c.cells) << 16);
        int rr = (int)(color & 0xFF), gg = (int)((color >> 8) & 0xFF), bb = (int)((color >> 16) & 0xFF);
        int sat = std::max(std::max(rr, gg), bb) - std::min(std::min(rr, gg), bb);
        double contrast = color_dist(color, bg) / 510.0;  // 0..1
        double ar = w / (double)h;
        double shape = 0;
        const char* kind = "region";
        if (h >= 18 && h <= 52 && ar >= 1.6 && ar <= 9.0 && w <= 460) {
            shape = 1.0;  // a button-sized strip
            kind = "button";
        } else if (h >= 16 && h <= 44 && ar >= 2.0 && w <= 900) {
            shape = 0.7;
            kind = "field";
        } else if (w >= 120 && h >= 90) {
            shape = 0.35;
            kind = "panel";
        }
        if (shape <= 0) continue;
        UiElement e;
        e.rect = Rect{c.minx * C, c.miny * C, w, h};
        e.color = color;
        e.cells = c.cells;
        e.kind = kind;
        e.score = 0.45 * shape + 0.35 * contrast + 0.20 * (sat / 255.0);
        if (color_dist(color, bg) < 30) e.score *= 0.35;  // same-ish as the background
        out.push_back(e);
    }

    std::sort(out.begin(), out.end(), [](const UiElement& a, const UiElement& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.rect.w * a.rect.h > b.rect.w * b.rect.h;
    });

    // 5. Drop boxes that are just the inside of a box we already kept.
    std::vector<UiElement> kept;
    for (const UiElement& e : out) {
        bool dup = false;
        for (const UiElement& k : kept) {
            int ix = std::max(0, std::min(e.rect.x + e.rect.w, k.rect.x + k.rect.w) -
                                     std::max(e.rect.x, k.rect.x));
            int iy = std::max(0, std::min(e.rect.y + e.rect.h, k.rect.y + k.rect.h) -
                                     std::max(e.rect.y, k.rect.y));
            if ((double)(ix * iy) / (double)(e.rect.w * e.rect.h) > 0.82) {
                dup = true;
                break;
            }
        }
        if (!dup) kept.push_back(e);
        if ((int)kept.size() >= max_elements) break;
    }
    return kept;
}

std::string describe_elements(const std::vector<UiElement>& els) {
    std::string out;
    for (size_t i = 0; i < els.size(); i++) {
        const UiElement& e = els[i];
        char line[256];
        snprintf(line, sizeof(line), "%2d. %-6s %4d,%-4d %3dx%-3d #%s  centre %d,%d\n",
                 (int)i + 1, e.kind, e.rect.x, e.rect.y, e.rect.w, e.rect.h,
                 color_hex(e.color).c_str(), e.rect.x + e.rect.w / 2, e.rect.y + e.rect.h / 2);
        out += line;
    }
    return out;
}

bool find_color_area(const Image& img, uint32_t bgr, int tolerance, Rect* out) {
    if (img.empty()) return false;
    if (tolerance < 0) tolerance = 0;
    const int C = 4;
    int gw = img.w / C, gh = img.h / C;
    if (gw <= 0 || gh <= 0) return false;
    std::vector<uint8_t> hit((size_t)gw * gh, 0);
    for (int cy = 0; cy < gh; cy++) {
        for (int cx = 0; cx < gw; cx++) {
            uint32_t m = box_mean(img, cx * C, cy * C, (cx + 1) * C, (cy + 1) * C);
            if (color_dist(m, bgr) <= tolerance) hit[(size_t)cy * gw + cx] = 1;
        }
    }
    std::vector<uint8_t> seen(hit.size(), 0);
    std::vector<int> stack;
    Rect best{};
    long best_cells = 0;
    for (int start = 0; start < gw * gh; start++) {
        if (!hit[start] || seen[start]) continue;
        stack.clear();
        stack.push_back(start);
        seen[start] = 1;
        int minx = gw, miny = gh, maxx = -1, maxy = -1;
        long cells = 0;
        while (!stack.empty()) {
            int cur = stack.back();
            stack.pop_back();
            int x = cur % gw, y = cur / gw;
            cells++;
            if (x < minx) minx = x;
            if (y < miny) miny = y;
            if (x > maxx) maxx = x;
            if (y > maxy) maxy = y;
            const int nx[4] = {x - 1, x + 1, x, x}, ny[4] = {y, y, y - 1, y + 1};
            for (int k = 0; k < 4; k++) {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= gw || ny[k] >= gh) continue;
                int ni = ny[k] * gw + nx[k];
                if (!hit[ni] || seen[ni]) continue;
                seen[ni] = 1;
                stack.push_back(ni);
            }
        }
        if (cells > best_cells) {
            best_cells = cells;
            best = Rect{minx * C, miny * C, (maxx - minx + 1) * C, (maxy - miny + 1) * C};
        }
    }
    if (best_cells <= 0 || best.w < 2 || best.h < 2) return false;
    if (out) *out = best;
    return true;
}

// A 3x5 bitmap font, enough to label a grid. Rows top to bottom, 3 bits each.
static const uint8_t DIGIT_ROWS[10][5] = {
    {7, 5, 5, 5, 7},  // 0
    {2, 6, 2, 2, 7},  // 1
    {7, 1, 7, 4, 7},  // 2
    {7, 1, 7, 1, 7},  // 3
    {5, 5, 7, 1, 1},  // 4
    {7, 4, 7, 1, 7},  // 5
    {7, 4, 7, 5, 7},  // 6
    {7, 1, 1, 1, 1},  // 7
    {7, 5, 7, 5, 7},  // 8
    {7, 5, 7, 1, 7},  // 9
};

static void draw_digits(Image& img, int value, int x, int y, int scale, uint32_t ink) {
    std::string s = std::to_string(value);
    for (char ch : s) {
        if (ch < '0' || ch > '9') continue;
        const uint8_t* rows = DIGIT_ROWS[ch - '0'];
        for (int r = 0; r < 5; r++)
            for (int c = 0; c < 3; c++)
                if (rows[r] & (4 >> c))
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++)
                            img.set(x + c * scale + dx, y + r * scale + dy, ink);
        x += 4 * scale;
    }
}

Image draw_grid(const Image& src, int step) {
    Image out = src;
    if (out.empty() || step < 40) return out;
    // Lines blend with the backdrop: light ink on a dark screen, dark ink on a
    // light one, so the overlay never washes out what it is labelling.
    uint32_t avg = box_mean(out, 0, 0, out.w, out.h);
    int lum = (int)((avg & 0xFF) + ((avg >> 8) & 0xFF) + ((avg >> 16) & 0xFF));
    bool dark = lum < 384;
    uint32_t line = dark ? 0x005A5A5Au : 0x00A0A0A0u;
    uint32_t ink = dark ? 0x00F0F0F0u : 0x00202020u;
    uint32_t plate = dark ? 0x00141414u : 0x00F0F0F0u;

    for (int x = step; x < out.w; x += step) {
        for (int y = 0; y < out.h; y += 3) {
            out.set(x, y, line);
            out.set(x + 1, y, line);
        }
        // label the line with the x it marks, just under the top edge
        for (int i = -2; i < 14; i++)
            for (int j = -2; j < 9; j++) out.set(x + 2 + i, 2 + j, plate);
        draw_digits(out, x, x + 3, 3, 2, ink);
    }
    for (int y = step; y < out.h; y += step) {
        for (int x = 0; x < out.w; x += 3) {
            out.set(x, y, line);
            out.set(x, y + 1, line);
        }
        for (int i = -2; i < 22; i++)
            for (int j = -2; j < 9; j++) out.set(2 + i, y + 2 + j, plate);
        draw_digits(out, y, 3, y + 3, 2, ink);
    }
    return out;
}

}  // namespace hawk
