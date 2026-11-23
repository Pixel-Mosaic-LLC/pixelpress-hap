// PixelPress HAP 1.1 accurate texture encoders + quality report. See texture_hq.hpp.

#include "texture_hq.hpp"
#include "texture_converter.hpp"
#include "parallel.hpp"
#include "hap.h"

#include "rgbcx.h"
#include "bc7decomp.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#include <arm_neon.h>
#define PPHAP_NEON 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define PPHAP_SSE2 1
#endif

// The Hap Q error maths below is written as separate multiplies and adds. Never let the
// compiler fuse them (FMA): results then depend only on IEEE float arithmetic, so the SIMD and
// scalar paths, and Mac and Windows builds, produce bit-identical files.
#if defined(__clang__)
#pragma clang fp contract(off)
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

// ---------------------------------------------------------------------------------------------
// engine selection

static std::atomic<int> g_engine{ -1 };

TextureEngine textureEngine()
{
    int e = g_engine.load();
    if (e < 0) {
        const char* env = getenv("PPHAP_ENGINE");
        std::string v = env ? env : "";
        for (auto& ch : v) ch = (char)tolower((unsigned char)ch);
        e = (v == "legacy" || v == "1.0" || v == "1.0.0" || v == "v1") ? int(TextureEngine::Legacy)
                                                                         : int(TextureEngine::Accurate);
        int expected = -1;
        g_engine.compare_exchange_strong(expected, e);
        e = g_engine.load();
    }
    return TextureEngine(e);
}

void setTextureEngine(TextureEngine engine) { g_engine = int(engine); }

const char* textureEngineName(TextureEngine engine)
{
    return engine == TextureEngine::Legacy ? "legacy (1.0)" : "accurate";
}

static void initRgbcx()
{
    static std::once_flag once;
    std::call_once(once, []() { rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal); });
}

// ---------------------------------------------------------------------------------------------
// shared helpers

namespace {

const int kBlockRowsPerBand = 8;   // same banding as the 1.0 converters

int blocksFor(int n) { return (n + 3) / 4; }

// 4x4 block gather (RGBA, block order), clamping at the right / bottom edge
inline void gatherBlock(const uint8_t* rgba, int w, int h, int bx, int by, uint8_t* out)
{
    for (int py = 0; py < 4; ++py) {
        const int sy = std::min(by * 4 + py, h - 1);
        const uint8_t* row = rgba + size_t(sy) * w * 4;
        for (int px = 0; px < 4; ++px) {
            const int sx = std::min(bx * 4 + px, w - 1);
            memcpy(out + (py * 4 + px) * 4, row + size_t(sx) * 4, 4);
        }
    }
}

template <class BlockFn>
void encodeBlocks(int w, int h, size_t bytesPerBlock, std::vector<uint8_t>& out, BlockFn fn)
{
    const int bw = blocksFor(w), bh = blocksFor(h);
    out.resize(size_t(bw) * bh * bytesPerBlock);
    const int bands = (bh + kBlockRowsPerBand - 1) / kBlockRowsPerBand;
    parallelFor(bands, [&](int band) {
        uint8_t px[64];
        const int by1 = std::min(bh, (band + 1) * kBlockRowsPerBand);
        for (int by = band * kBlockRowsPerBand; by < by1; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                fn(bx, by, px, out.data() + (size_t(by) * bw + bx) * bytesPerBlock);
            }
    });
}

// ---------------------------------------------------------------------------------------------
// BC1 / BC3 / BC4 via rgbcx

// quality 0 / 1 / 2 -> rgbcx level, chosen by measurement (1080p test set, RGB PSNR / speed):
//   Fast   level 5  : +2.2 dB over 1.0 Fast (squish range fit) at the same speed
//   Normal level 10 : +0.2 dB over 1.0 Normal (squish cluster fit), ~25% faster
//   High   level 18 : +0.25 dB over 1.0 High, slower
uint32_t bc1Level(int quality)
{
    switch (quality) {
    case 0:  return 5;
    case 2:  return 18;
    default: return 10;
    }
}

uint32_t bc4SearchRadius(int quality) { return quality >= 2 ? 5 : (quality == 1 ? 3 : 2); }

class Bc1Converter : public TextureConverter {
public:
    Bc1Converter(const FrameSize& fs, int quality) : TextureConverter(fs), level_(bc1Level(quality)) { initRgbcx(); }
    size_t size() const override { return size_t(blocksFor(frameSize().width)) * blocksFor(frameSize().height) * 8; }
private:
    void doConvert(const uint8_t* rgba, std::vector<uint8_t>&, std::vector<uint8_t>& out) override
    {
        const int w = frameSize().width, h = frameSize().height;
        const uint32_t level = level_;
        encodeBlocks(w, h, 8, out, [&](int bx, int by, uint8_t* px, uint8_t* dst) {
            gatherBlock(rgba, w, h, bx, by, px);
            // 3-colour blocks allowed (standard BC1), but never the transparent-black texel:
            // opaque Hap must decode with alpha = 1 everywhere.
            rgbcx::encode_bc1(level, dst, px, true, false);
        });
    }
    uint32_t level_;
};

class Bc3Converter : public TextureConverter {
public:
    Bc3Converter(const FrameSize& fs, int quality) : TextureConverter(fs), level_(bc1Level(quality)), rad_(bc4SearchRadius(quality)) { initRgbcx(); }
    size_t size() const override { return size_t(blocksFor(frameSize().width)) * blocksFor(frameSize().height) * 16; }
private:
    void doConvert(const uint8_t* rgba, std::vector<uint8_t>&, std::vector<uint8_t>& out) override
    {
        const int w = frameSize().width, h = frameSize().height;
        encodeBlocks(w, h, 16, out, [&](int bx, int by, uint8_t* px, uint8_t* dst) {
            gatherBlock(rgba, w, h, bx, by, px);
            rgbcx::encode_bc3_hq(level_, dst, px, rad_, rgbcx::BC4_USE_ALL_MODES);
        });
    }
    uint32_t level_, rad_;
};

class Bc4AlphaConverter : public TextureConverter {   // Hap Alpha-Only / Hap Q Alpha plane
public:
    Bc4AlphaConverter(const FrameSize& fs, int quality) : TextureConverter(fs), rad_(bc4SearchRadius(quality)) { initRgbcx(); }
    size_t size() const override { return size_t(blocksFor(frameSize().width)) * blocksFor(frameSize().height) * 8; }
private:
    void doConvert(const uint8_t* rgba, std::vector<uint8_t>&, std::vector<uint8_t>& out) override
    {
        const int w = frameSize().width, h = frameSize().height;
        encodeBlocks(w, h, 8, out, [&](int bx, int by, uint8_t* px, uint8_t* dst) {
            gatherBlock(rgba, w, h, bx, by, px);
            rgbcx::encode_bc4_hq(dst, px + 3, 4, rad_, rgbcx::BC4_USE_ALL_MODES);
        });
    }
    uint32_t rad_;
};

// ---------------------------------------------------------------------------------------------
// Hap Q: scaled YCoCg in DXT5 (Y in the alpha block, Co/Cg/scale in the colour block).
//
// The 1.0 encoder is id Software's real-time one (bounding box + inset, one pass). Here:
//   Y     -> rgbcx BC4 high quality (searches both 8- and 6-level modes)
//   CoCg  -> every legal scale, several starting endpoint pairs, least-squares refit and a
//            +/-1 endpoint search, all scored by the RGB error the Hap Q shader will produce:
//            R = Y+Co-Cg, G = Y+Cg, B = Y-Co-Cg  =>  err = 2 dCo^2 + 3 dCg^2  (per 8-bit unit)

struct CoCgBlockResult { uint16_t e0, e1; uint32_t sel; double err; };

inline int expand5(int v) { return (v << 3) | (v >> 2); }
inline int expand6(int v) { return (v << 2) | (v >> 4); }

// score a given pair of quantised endpoints (co5/cg6) at scale s; fills selectors
double scoreCoCg(const float* co, const float* cg, int s, int a5, int a6, int b5, int b6, uint32_t& sel,
                 double bound = 1e300)
{
    // palette in *unscaled* 8-bit units centred on 0, exactly as the shader decodes it
    const float inv = 1.0f / float(s);
    float pc[4], pg[4];
    const float c0 = (float(expand5(a5)) - 128.f) * inv, g0 = (float(expand6(a6)) - 128.f) * inv;
    const float c1 = (float(expand5(b5)) - 128.f) * inv, g1 = (float(expand6(b6)) - 128.f) * inv;
    pc[0] = c0; pg[0] = g0;
    pc[1] = c1; pg[1] = g1;
    pc[2] = (2 * c0 + c1) / 3; pg[2] = (2 * g0 + g1) / 3;
    pc[3] = (c0 + 2 * c1) / 3; pg[3] = (g0 + 2 * g1) / 3;
    // 1.1.2: 4 pixels at a time on the SIMD units (NEON on Apple Silicon, SSE2 on x64), with
    // exactly the scalar arithmetic: ((2*dc)*dc) + ((3*dg)*dg), first smallest palette entry
    // wins, errors summed in pixel order. Callers only ever ask "is this below `bound`?", so
    // once the running total reaches the bound the candidate is abandoned (+infinity).
    double total = 0; uint32_t sels = 0;
    for (int q = 0; q < 16; q += 4) {
        float bestv[4], bidx[4];
#if PPHAP_NEON
        const float32x4_t vco = vld1q_f32(co + q), vcg = vld1q_f32(cg + q);
        const float32x4_t two = vdupq_n_f32(2.f), three = vdupq_n_f32(3.f);
        float32x4_t b = vdupq_n_f32(0.f), bi = vdupq_n_f32(0.f);
        for (int k = 0; k < 4; ++k) {
            const float32x4_t dc = vsubq_f32(vdupq_n_f32(pc[k]), vco), dg = vsubq_f32(vdupq_n_f32(pg[k]), vcg);
            const float32x4_t e = vaddq_f32(vmulq_f32(vmulq_f32(two, dc), dc), vmulq_f32(vmulq_f32(three, dg), dg));
            if (k == 0) { b = e; continue; }
            const uint32x4_t lt = vcltq_f32(e, b);
            b = vbslq_f32(lt, e, b);
            bi = vbslq_f32(lt, vdupq_n_f32(float(k)), bi);
        }
        vst1q_f32(bestv, b); vst1q_f32(bidx, bi);
#elif PPHAP_SSE2
        const __m128 vco = _mm_loadu_ps(co + q), vcg = _mm_loadu_ps(cg + q);
        const __m128 two = _mm_set1_ps(2.f), three = _mm_set1_ps(3.f);
        __m128 b = _mm_setzero_ps(), bi = _mm_setzero_ps();
        for (int k = 0; k < 4; ++k) {
            const __m128 dc = _mm_sub_ps(_mm_set1_ps(pc[k]), vco), dg = _mm_sub_ps(_mm_set1_ps(pg[k]), vcg);
            const __m128 e = _mm_add_ps(_mm_mul_ps(_mm_mul_ps(two, dc), dc), _mm_mul_ps(_mm_mul_ps(three, dg), dg));
            if (k == 0) { b = e; continue; }
            const __m128 lt = _mm_cmplt_ps(e, b);
            b = _mm_or_ps(_mm_and_ps(lt, e), _mm_andnot_ps(lt, b));
            bi = _mm_or_ps(_mm_and_ps(lt, _mm_set1_ps(float(k))), _mm_andnot_ps(lt, bi));
        }
        _mm_storeu_ps(bestv, b); _mm_storeu_ps(bidx, bi);
#else
        for (int j = 0; j < 4; ++j) {
            float b = 0, bi = 0;
            for (int k = 0; k < 4; ++k) {
                const float dc = pc[k] - co[q + j], dg = pg[k] - cg[q + j];
                const float e = 2 * dc * dc + 3 * dg * dg;
                if (k == 0 || e < b) { b = e; bi = float(k); }
            }
            bestv[j] = b; bidx[j] = bi;
        }
#endif
        for (int j = 0; j < 4; ++j) { total += bestv[j]; sels |= uint32_t(bidx[j]) << ((q + j) * 2); }
        if (total >= bound) { sel = 0; return 1e300; }
    }
    sel = sels;
    return total;
}

// Every candidate a block tries is scored through this cache: the starting guesses often
// converge on the same endpoints and the +/-1 search steps back over ones already scored.
// scoreCoCg is a pure function, so a cached score is exactly the score.
struct CoCgScoreCache {
    struct Entry { uint32_t key; uint32_t sel; double err; };
    Entry e[64]; int n = 0;
    double score(const float* co, const float* cg, int s, int a5, int a6, int b5, int b6, uint32_t& sel,
                 double bound)
    {
        const uint32_t key = uint32_t(s) << 22 | uint32_t(a5) << 17 | uint32_t(a6) << 11 | uint32_t(b5) << 6 | uint32_t(b6);
        for (int i = 0; i < n; ++i) if (e[i].key == key) { sel = e[i].sel; return e[i].err; }
        const double err = scoreCoCg(co, cg, s, a5, a6, b5, b6, sel, bound);
        if (err < 1e300 && n < 64) e[n++] = Entry{ key, sel, err };   // abandoned scores aren't exact
        return err;
    }
};

inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// std::lround for the endpoint quantiser without the libm call: round half away from zero.
// x - trunc(x) is exact in float, so this returns exactly what std::lround does.
inline int roundHalfAway(float x)
{
    x = x < -1e9f ? -1e9f : (x > 1e9f ? 1e9f : x);   // results are clamped to 0..63 afterwards anyway
    int i = int(x);
    const float f = x - float(i);
    if (f >= 0.5f) ++i; else if (f <= -0.5f) --i;
    return i;
}

void encodeCoCgBlock(const uint8_t* px, uint8_t* dst8, int effort)
{
    // source chroma, centred (units of the 8-bit YCoCg values produced by the 1.0 converter)
    float co[16], cg[16];
    float maxAbs = 0;
    for (int i = 0; i < 16; ++i) {
        co[i] = float(px[i * 4 + 0]) - 128.f;
        cg[i] = float(px[i * 4 + 1]) - 128.f;
        maxAbs = std::max(maxAbs, std::max(std::fabs(co[i]), std::fabs(cg[i])));
    }

    CoCgBlockResult best{ 0, 0, 0, 1e30 };
    CoCgScoreCache cache;
    // largest scale that doesn't clip gives the finest chroma steps; High also tries the next
    // one down (occasionally better when the block's range sits right at the limit)
    const int maxScale = maxAbs * 4 <= 127.f ? 4 : (maxAbs * 2 <= 127.f ? 2 : 1);
    const int scales[3] = { 4, 2, 1 };
    for (int s : scales) {
        if (s > maxScale) continue;                    // would clip
        if (s < maxScale && (effort < 3 || s * 2 < maxScale)) break;
        // scaled values in 8-bit code space
        float x[16], y[16];
        float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
        for (int i = 0; i < 16; ++i) {
            x[i] = co[i] * s + 128.f; y[i] = cg[i] * s + 128.f;
            minx = std::min(minx, x[i]); maxx = std::max(maxx, x[i]);
            miny = std::min(miny, y[i]); maxy = std::max(maxy, y[i]);
        }
        // starting endpoint pairs: both bbox diagonals, plus the principal axis
        float mx = 0, my = 0;
        for (int i = 0; i < 16; ++i) { mx += x[i]; my += y[i]; }
        mx /= 16; my /= 16;
        float sxx = 0, sxy = 0, syy = 0;
        for (int i = 0; i < 16; ++i) { float dx = x[i] - mx, dy = y[i] - my; sxx += dx * dx; sxy += dx * dy; syy += dy * dy; }
        float ax = 1, ay = 0;
        if (sxx + syy > 0) {
            // dominant eigenvector of [[sxx sxy][sxy syy]]
            const float tr = sxx + syy, det = sxx * syy - sxy * sxy;
            const float l = tr / 2 + std::sqrt(std::max(0.f, tr * tr / 4 - det));
            if (std::fabs(sxy) > 1e-6f) { ax = l - syy; ay = sxy; } else if (syy > sxx) { ax = 0; ay = 1; }
            const float n = std::sqrt(ax * ax + ay * ay); ax /= n; ay /= n;
        }
        float tmin = 1e9f, tmax = -1e9f;
        for (int i = 0; i < 16; ++i) { float t = (x[i] - mx) * ax + (y[i] - my) * ay; tmin = std::min(tmin, t); tmax = std::max(tmax, t); }

        float starts[3][4] = {
            { maxx, maxy, minx, miny },
            { maxx, miny, minx, maxy },
            { mx + ax * tmax, my + ay * tmax, mx + ax * tmin, my + ay * tmin },
        };
        const int nStarts = effort >= 1 ? 3 : 2;
        for (int st = 0; st < nStarts; ++st) {
            float e0x = starts[st][0], e0y = starts[st][1], e1x = starts[st][2], e1y = starts[st][3];
            int a5 = 0, a6 = 0, b5 = 0, b6 = 0; uint32_t sel = 0; double err = 1e30;
            const int iters = effort >= 1 ? 3 : 2;
            for (int it = 0; it < iters; ++it) {
                int na5 = clampi(roundHalfAway((e0x * 31.f / 255.f)), 0, 31), na6 = clampi(roundHalfAway((e0y * 63.f / 255.f)), 0, 63);
                int nb5 = clampi(roundHalfAway((e1x * 31.f / 255.f)), 0, 31), nb6 = clampi(roundHalfAway((e1y * 63.f / 255.f)), 0, 63);
                uint32_t nsel; double nerr = cache.score(co, cg, s, na5, na6, nb5, nb6, nsel, err);
                if (nerr >= err) break;
                a5 = na5; a6 = na6; b5 = nb5; b6 = nb6; sel = nsel; err = nerr;
                if (err == 0) break;                   // exact: a refit can't improve on zero
                // least-squares refit of both endpoints given the selectors (in scaled code space)
                static const float w0[4] = { 1.f, 0.f, 2.f / 3, 1.f / 3 };
                float A = 0, B = 0, C = 0, bx0 = 0, bx1 = 0, by0 = 0, by1 = 0;
                for (int i = 0; i < 16; ++i) {
                    const int k = (sel >> (i * 2)) & 3; const float a = w0[k], b = 1 - a;
                    A += a * a; B += a * b; C += b * b;
                    bx0 += a * x[i]; bx1 += b * x[i]; by0 += a * y[i]; by1 += b * y[i];
                }
                const float d = A * C - B * B;
                if (std::fabs(d) < 1e-6f) break;
                e0x = (C * bx0 - B * bx1) / d; e1x = (A * bx1 - B * bx0) / d;
                e0y = (C * by0 - B * by1) / d; e1y = (A * by1 - B * by0) / d;
            }
            if (err < best.err) {
                const int sb = s - 1;   // scale stored in the 5-bit blue field
                best.e0 = uint16_t((a5 << 11) | (a6 << 5) | sb);
                best.e1 = uint16_t((b5 << 11) | (b6 << 5) | sb);
                best.sel = sel; best.err = err;
            }
            if (best.err == 0) goto done;              // exact: later candidates only replace on "<"
        }
    }
    // local +/-1 search on the four quantised coordinates of the best candidate
    if (effort >= 1 && best.err > 0) {
        const int s = (best.e0 & 31) + 1, sb = s - 1;
        int a5 = best.e0 >> 11, a6 = (best.e0 >> 5) & 63, b5 = best.e1 >> 11, b6 = (best.e1 >> 5) & 63;
        double err = best.err; uint32_t sel = best.sel;
        bool improved = true; int rounds = 0;
        const int maxRounds = effort >= 3 ? 32 : (effort == 2 ? 6 : 1);   // one round already gets almost all of it
        while (improved && rounds++ < maxRounds) {
            improved = false;
            int* coords[4] = { &a5, &a6, &b5, &b6 };
            const int lim[4] = { 31, 63, 31, 63 };
            for (int c = 0; c < 4; ++c)
                for (int dlt = -1; dlt <= 1; dlt += 2) {
                    const int old = *coords[c], nv = old + dlt;
                    if (nv < 0 || nv > lim[c]) continue;
                    *coords[c] = nv;
                    uint32_t nsel; const double nerr = cache.score(co, cg, s, a5, a6, b5, b6, nsel, err);
                    if (nerr < err) { err = nerr; sel = nsel; improved = true; }
                    else *coords[c] = old;
                }
        }
        best.e0 = uint16_t((a5 << 11) | (a6 << 5) | sb);
        best.e1 = uint16_t((b5 << 11) | (b6 << 5) | sb);
        best.sel = sel; best.err = err;
    }
done:
    dst8[0] = uint8_t(best.e0); dst8[1] = uint8_t(best.e0 >> 8);
    dst8[2] = uint8_t(best.e1); dst8[3] = uint8_t(best.e1 >> 8);
    dst8[4] = uint8_t(best.sel); dst8[5] = uint8_t(best.sel >> 8);
    dst8[6] = uint8_t(best.sel >> 16); dst8[7] = uint8_t(best.sel >> 24);
}

}   // namespace

extern "C" {
#include "YCoCg.h"
}

namespace {

class YCoCgAccurateConverter : public TextureConverter {
public:
    YCoCgAccurateConverter(const FrameSize& fs, int quality)
        : TextureConverter(fs), effort_(std::min(std::max(quality, 0), 2) + 1), yRad_(quality >= 2 ? 5 : (quality == 1 ? 3 : 1))
    {
        initRgbcx();
    }
    size_t size() const override { return size_t(blocksFor(frameSize().width)) * blocksFor(frameSize().height) * 16; }
private:
    void doConvert(const uint8_t* rgba, std::vector<uint8_t>& ycocg, std::vector<uint8_t>& out) override
    {
        const int w = frameSize().width, h = frameSize().height;
        const size_t rowbytes = size_t(w) * 4;
        ycocg.resize(rowbytes * h);
        // same RGB -> [Co, Cg, 0, Y] conversion as 1.0 (so both engines target the same values)
        const int bands = (h + 31) / 32;
        parallelFor(bands, [&](int band) {
            const int y0 = band * 32, bh = std::min(32, h - y0);
            // 1.1.3: same YCoCg as 1.0 but *rounded* (1.0 truncates each /4, which leaves
            // decoded blue ~+0.3 and green ~-0.6 levels off on average)
            for (int yy = 0; yy < bh; ++yy) {
                const uint8_t* s = rgba + (y0 + yy) * rowbytes;
                uint8_t* d = &ycocg[(y0 + yy) * rowbytes];
                for (int x = 0; x < w; ++x, s += 4, d += 4) {
                    const int R = s[0], G = s[1], B = s[2];
                    d[0] = uint8_t(std::min(255, (2 * R - 2 * B + 512 + 2) >> 2));   // Co
                    d[1] = uint8_t(std::min(255, (-R + 2 * G - B + 512 + 2) >> 2));  // Cg
                    d[2] = uint8_t(std::min(255, (4 * s[3] + 2) >> 2));              // A (unused)
                    d[3] = uint8_t(std::min(255, (R + 2 * G + B + 2) >> 2));         // Y
                }
            }
        });
        const uint8_t* src = ycocg.data();
        const int effort = effort_;
        encodeBlocks(w, h, 16, out, [&](int bx, int by, uint8_t* px, uint8_t* dst) {
            gatherBlock(src, w, h, bx, by, px);
            rgbcx::encode_bc4_hq(dst, px + 3, 4, yRad_, rgbcx::BC4_USE_ALL_MODES);   // Y
            encodeCoCgBlock(px, dst + 8, effort);                                 // Co Cg scale
        });
    }
    int effort_;
    uint32_t yRad_;
};

}   // namespace

std::unique_ptr<TextureConverter> createAccurateTextureConverter(const FrameSize& frameSize,
                                                                 unsigned int destFormat, int quality)
{
    switch (destFormat) {
    case HapTextureFormat_RGB_DXT1:   return std::make_unique<Bc1Converter>(frameSize, quality);
    case HapTextureFormat_RGBA_DXT5:  return std::make_unique<Bc3Converter>(frameSize, quality);
    case HapTextureFormat_YCoCg_DXT5: return std::make_unique<YCoCgAccurateConverter>(frameSize, quality);
    case HapTextureFormat_A_RGTC1:    return std::make_unique<Bc4AlphaConverter>(frameSize, quality);
    default:                          return nullptr;   // Hap R and anything else: shared converter
    }
}

// ---------------------------------------------------------------------------------------------
// quality report

namespace {

std::atomic<bool> g_reportOn{ false };
std::mutex g_reportMutex;
QualityReport g_report;

struct LabTables {
    float lin[256];
    LabTables()
    {
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            lin[i] = float(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
        }
    }
};
const LabTables& labTables() { static LabTables t; return t; }

inline void toLab(const uint8_t* rgb, double lab[3])
{
    const float* L = labTables().lin;
    const double r = L[rgb[0]], g = L[rgb[1]], b = L[rgb[2]];
    double X = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
    double Y = (0.2126729 * r + 0.7151522 * g + 0.0721750 * b);
    double Z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883;
    auto f = [](double t) { return t > 216.0 / 24389 ? std::cbrt(t) : (24389.0 / 27 * t + 16) / 116; };
    const double fx = f(X), fy = f(Y), fz = f(Z);
    lab[0] = 116 * fy - 16; lab[1] = 500 * (fx - fy); lab[2] = 200 * (fy - fz);
}

double deltaE2000(const double* l1, const double* l2)
{
    const double kPi = 3.14159265358979323846, d2r = kPi / 180;
    const double C1 = std::hypot(l1[1], l1[2]), C2 = std::hypot(l2[1], l2[2]), Cb = (C1 + C2) / 2;
    const double Cb7 = std::pow(Cb, 7), G = 0.5 * (1 - std::sqrt(Cb7 / (Cb7 + 6103515625.0)));
    const double a1 = (1 + G) * l1[1], a2 = (1 + G) * l2[1];
    const double C1p = std::hypot(a1, l1[2]), C2p = std::hypot(a2, l2[2]);
    double h1 = std::atan2(l1[2], a1) / d2r; if (h1 < 0) h1 += 360;
    double h2 = std::atan2(l2[2], a2) / d2r; if (h2 < 0) h2 += 360;
    const double dL = l2[0] - l1[0], dC = C2p - C1p;
    double dh = 0;
    if (C1p * C2p != 0) { dh = h2 - h1; if (dh > 180) dh -= 360; else if (dh < -180) dh += 360; }
    const double dH = 2 * std::sqrt(C1p * C2p) * std::sin(dh * d2r / 2);
    const double Lb = (l1[0] + l2[0]) / 2, Cbp = (C1p + C2p) / 2;
    double hb = h1 + h2;
    if (C1p * C2p != 0) {
        if (std::fabs(h1 - h2) <= 180) hb /= 2; else hb = hb < 360 ? (hb + 360) / 2 : (hb - 360) / 2;
    }
    const double T = 1 - 0.17 * std::cos((hb - 30) * d2r) + 0.24 * std::cos(2 * hb * d2r)
                   + 0.32 * std::cos((3 * hb + 6) * d2r) - 0.20 * std::cos((4 * hb - 63) * d2r);
    const double dth = 30 * std::exp(-((hb - 275) / 25) * ((hb - 275) / 25));
    const double Cbp7 = std::pow(Cbp, 7), Rc = 2 * std::sqrt(Cbp7 / (Cbp7 + 6103515625.0));
    const double Sl = 1 + 0.015 * (Lb - 50) * (Lb - 50) / std::sqrt(20 + (Lb - 50) * (Lb - 50));
    const double Sc = 1 + 0.045 * Cbp, Sh = 1 + 0.015 * Cbp * T, Rt = -std::sin(2 * dth * d2r) * Rc;
    const double tL = dL / Sl, tC = dC / Sc, tH = dH / Sh;
    return std::sqrt(tL * tL + tC * tC + tH * tH + Rt * tC * tH);
}

// decode one 4x4 block of a Hap texture to RGBA (as the Hap player shaders do)
void decodeBlock(unsigned int fmt, const uint8_t* blk, uint8_t* px /*64*/)
{
    switch (fmt) {
    case HapTextureFormat_RGB_DXT1:
        rgbcx::unpack_bc1(blk, px, true);
        break;
    case HapTextureFormat_RGBA_DXT5:
        rgbcx::unpack_bc3(blk, px);
        break;
    case HapTextureFormat_A_RGTC1:
        for (int i = 0; i < 16; ++i) px[i * 4 + 0] = px[i * 4 + 1] = px[i * 4 + 2] = 0;
        rgbcx::unpack_bc4(blk, px + 3, 4);
        break;
    case HapTextureFormat_RGBA_BPTC_UNORM:
        bc7decomp::unpack_bc7(blk, reinterpret_cast<bc7decomp::color_rgba*>(px));
        break;
    case HapTextureFormat_YCoCg_DXT5: {
        uint8_t tmp[64];
        rgbcx::unpack_bc4(blk, tmp + 3, 4);                   // Y
        // CoCg + scale: DXT5 colour blocks are always 4-colour, whatever the endpoint order
        {
            const uint16_t e0 = uint16_t(blk[8] | (blk[9] << 8)), e1 = uint16_t(blk[10] | (blk[11] << 8));
            float pr[4], pg[4], pb[4];
            pr[0] = float(expand5(e0 >> 11)); pg[0] = float(expand6((e0 >> 5) & 63)); pb[0] = float(expand5(e0 & 31));
            pr[1] = float(expand5(e1 >> 11)); pg[1] = float(expand6((e1 >> 5) & 63)); pb[1] = float(expand5(e1 & 31));
            pr[2] = (2 * pr[0] + pr[1]) / 3; pg[2] = (2 * pg[0] + pg[1]) / 3; pb[2] = (2 * pb[0] + pb[1]) / 3;
            pr[3] = (pr[0] + 2 * pr[1]) / 3; pg[3] = (pg[0] + 2 * pg[1]) / 3; pb[3] = (pb[0] + 2 * pb[1]) / 3;
            const uint32_t sel = uint32_t(blk[12] | (blk[13] << 8) | (blk[14] << 16) | (uint32_t(blk[15]) << 24));
            for (int i = 0; i < 16; ++i) {
                const int k = (sel >> (i * 2)) & 3;
                // Hap Q shader: scale = b*255/8 + 1 ; Co = (r - 128/255)/scale ; ...
                const float scale = pb[k] / 8.f + 1.f;
                const float Co = (pr[k] - 128.f) / scale, Cg = (pg[k] - 128.f) / scale, Y = tmp[i * 4 + 3];
                const float R = Y + Co - Cg, G = Y + Cg, B = Y - Co - Cg;
                px[i * 4 + 0] = uint8_t(clampi(roundHalfAway((R)), 0, 255));
                px[i * 4 + 1] = uint8_t(clampi(roundHalfAway((G)), 0, 255));
                px[i * 4 + 2] = uint8_t(clampi(roundHalfAway((B)), 0, 255));
                px[i * 4 + 3] = 255;
            }
        }
        break;
    }
    default:
        throw std::runtime_error("quality report: unknown texture format");
    }
}

}   // namespace

void enableQualityReport(bool on) { g_reportOn = on; }
bool qualityReportEnabled() { return g_reportOn.load(); }

QualityReport qualityReport()
{
    std::lock_guard<std::mutex> lock(g_reportMutex);
    return g_report;
}

void accumulateQuality(unsigned int fmt, int w, int h, const uint8_t* rgbaIn, const uint8_t* tex)
{
    const bool hasColour = fmt != HapTextureFormat_A_RGTC1;
    const bool hasAlpha = fmt == HapTextureFormat_A_RGTC1 || fmt == HapTextureFormat_RGBA_DXT5 ||
                          fmt == HapTextureFormat_RGBA_BPTC_UNORM;
    const int bw = blocksFor(w), bh = blocksFor(h);
    const size_t blockBytes = (fmt == HapTextureFormat_RGB_DXT1 || fmt == HapTextureFormat_A_RGTC1) ? 8 : 16;

    const int bands = (bh + kBlockRowsPerBand - 1) / kBlockRowsPerBand;
    std::vector<QualityReport> parts(bands);
    parallelFor(bands, [&](int band) {
        QualityReport& q = parts[band];
        uint8_t px[64];
        const int by1 = std::min(bh, (band + 1) * kBlockRowsPerBand);
        for (int by = band * kBlockRowsPerBand; by < by1; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                decodeBlock(fmt, tex + (size_t(by) * bw + bx) * blockBytes, px);
                for (int i = 0; i < 16; ++i) {
                    const int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                    if (x >= w || y >= h) continue;
                    const uint8_t* s = rgbaIn + (size_t(y) * w + x) * 4;
                    const uint8_t* d = px + i * 4;
                    if (hasColour) {
                        double yl = 0; const double k[3] = { 0.2126, 0.7152, 0.0722 };
                        for (int c = 0; c < 3; ++c) {
                            const int e = int(s[c]) - int(d[c]);
                            q.sse[c] += double(e) * e;
                            q.maxErr = std::max(q.maxErr, std::abs(e));
                            yl += k[c] * e;
                        }
                        q.sseLuma += yl * yl;
                        ++q.pixels;
                        if (((x | y) & 1) == 0) {
                            double la[3], lb[3];
                            toLab(s, la); toLab(d, lb);
                            const double de = deltaE2000(la, lb);
                            q.deSum += de; q.deMax = std::max(q.deMax, de); ++q.dePixels;
                            q.deHist[std::min(1000, int(de * 20))]++;
                        }
                    }
                    if (hasAlpha) {
                        const int e = int(s[3]) - int(d[3]);
                        q.sseAlpha += double(e) * e; ++q.alphaPixels;
                    }
                }
            }
    });
    std::lock_guard<std::mutex> lock(g_reportMutex);
    for (const auto& q : parts) {
        g_report.pixels += q.pixels;
        for (int c = 0; c < 3; ++c) g_report.sse[c] += q.sse[c];
        g_report.sseLuma += q.sseLuma;
        g_report.maxErr = std::max(g_report.maxErr, q.maxErr);
        g_report.alphaPixels += q.alphaPixels; g_report.sseAlpha += q.sseAlpha;
        g_report.dePixels += q.dePixels; g_report.deSum += q.deSum; g_report.deMax = std::max(g_report.deMax, q.deMax);
        for (int i = 0; i <= 1000; ++i) g_report.deHist[i] += q.deHist[i];
    }
    if (hasColour) ++g_report.frames;
}

static double psnrFrom(double sse, unsigned long long n)
{
    if (n == 0) return 0;
    const double mse = sse / double(n);
    return mse <= 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}

double QualityReport::psnrRGB() const { return psnrFrom(sse[0] + sse[1] + sse[2], pixels * 3); }
double QualityReport::psnrChannel(int c) const { return psnrFrom(sse[c], pixels); }
double QualityReport::psnrLuma() const { return psnrFrom(sseLuma, pixels); }
double QualityReport::psnrAlpha() const { return psnrFrom(sseAlpha, alphaPixels); }
double QualityReport::deMean() const { return dePixels ? deSum / double(dePixels) : 0; }
double QualityReport::dePercentile(double p) const
{
    if (!dePixels) return 0;
    const double target = p / 100.0 * double(dePixels);
    unsigned long long acc = 0;
    for (int i = 0; i <= 1000; ++i) { acc += deHist[i]; if (double(acc) >= target) return (i + 1) * 0.05; }
    return 50;
}
