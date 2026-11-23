// Self-test for the plugin's texture encoders: RGBA -> TextureConverter -> HapEncode
// -> HapDecode round trip -> texture decode -> PSNR against the source.
// Usage: hapr_selftest in.rgba width height quality(0|1|2) chunks out.hapframe [format] [legacy]
//   format: hap_r (default) | hap | hap_alpha | hap_q | alpha
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include "texture_converter.hpp"
#include "hap.h"
#include "texture_hq.hpp"
#include <string>

static void decodeCallback(HapDecodeWorkFunction f, void* p, unsigned int count, void*) {
    for (unsigned int i = 0; i < count; ++i) f(p, i);
}

int main(int argc, char** argv) {
    if (argc < 7) { fprintf(stderr, "usage: hapr_selftest in.rgba w h quality chunks out.hapframe [hap_r|hap|hap_alpha|hap_q|alpha] [legacy]\n"); return 2; }
    int w = atoi(argv[2]), h = atoi(argv[3]), q = atoi(argv[4]); unsigned chunks = atoi(argv[5]);
    std::string kind = argc > 7 ? argv[7] : "hap_r";
    if (argc > 8 && std::string(argv[8]) == "legacy") setTextureEngine(TextureEngine::Legacy);
    unsigned int fmt = HapTextureFormat_RGBA_BPTC_UNORM;
    if (kind == "hap") fmt = HapTextureFormat_RGB_DXT1;
    else if (kind == "hap_alpha") fmt = HapTextureFormat_RGBA_DXT5;
    else if (kind == "hap_q") fmt = HapTextureFormat_YCoCg_DXT5;
    else if (kind == "alpha") fmt = HapTextureFormat_A_RGTC1;
    else if (kind != "hap_r") { fprintf(stderr, "unknown format %s\n", kind.c_str()); return 2; }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    f.read((char*)rgba.data(), rgba.size());
    if ((size_t)f.gcount() != rgba.size()) { fprintf(stderr, "short read\n"); return 2; }

    // the quality report decodes the texture exactly as a player would and compares it
    enableQualityReport(true);
    auto conv = TextureConverter::create(FrameSize{ w, h }, fmt, (SquishEncoderQuality)q);
    std::vector<uint8_t> ycocg, tex;
    auto t0 = std::chrono::steady_clock::now();
    conv->convert(rgba.data(), ycocg, tex);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    unsigned long sz = (unsigned long)tex.size();
    unsigned int comp = HapCompressorSnappy;
    unsigned long maxLen = HapMaxEncodedLength(1, &sz, &fmt, &chunks);
    std::vector<uint8_t> frame(maxLen);
    unsigned long used = 0;
    const void* bufs[1] = { tex.data() };
    if (HapEncode(1, bufs, &sz, &fmt, &comp, &chunks, frame.data(), maxLen, &used) != HapResult_No_Error) { puts("HapEncode FAILED"); return 1; }
    frame.resize(used);
    std::ofstream(argv[6], std::ios::binary).write((char*)frame.data(), frame.size());

    // round trip through the reference Hap decoder
    std::vector<uint8_t> back(tex.size());
    unsigned long outUsed = 0; unsigned int outFmt = 0;
    unsigned r = HapDecode(frame.data(), used, 0, decodeCallback, nullptr, back.data(), back.size(), &outUsed, &outFmt);
    bool same = (r == HapResult_No_Error && outFmt == fmt && outUsed == tex.size() && back == tex);

    const QualityReport rep = qualityReport();
    const double rgb = rep.pixels ? rep.psnrRGB() : 99.0, a = rep.alphaPixels ? rep.psnrAlpha() : 99.0;
    // sanity floor: anything this low means a broken encoder, not a hard image
    const bool sane = rgb > 10.0 && a > 10.0;   // random noise legitimately sits at 13-17 dB; a broken encoder is far lower
    printf("%s %dx%d q=%d  texture %lu B  hap frame %lu B (%.1fx smaller)  encode %.2fs  roundtrip %s  PSNR rgb %.2f dB  alpha %.2f dB%s\n",
           kind.c_str(), w, h, q, sz, used, (double)sz / used, secs, same ? "OK" : "MISMATCH", rgb, a, sane ? "" : "  QUALITY FAIL");
    return (same && sane) ? 0 : 1;
}
