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

}  // namespace hawk
