#pragma once

// PixelPress HAP 1.1 "accurate" texture encoders, plus the quality report.
//
// Encoder engines
//   Accurate (default from 1.1): rgbcx (Rich Geldreich, MIT / public domain) for DXT1 / DXT5 /
//       RGTC1, a least-squares YCoCg-DXT5 encoder for Hap Q, bc7e for Hap R (unchanged).
//   Legacy: the exact PixelPress HAP 1.0.0 encoders (squish + the real-time YCoCg encoder).
//       Selected with pphap -engine legacy, or PPHAP_ENGINE=legacy in the environment (this also
//       reaches the Adobe plugin), so 1.0 output can always be reproduced.
//
// Quality report
//   When enabled, every encoded texture is decoded again (exactly as a GPU decodes it) and
//   compared with the frame the encoder was given. pphap -psnr prints the result.

#include <cstddef>
#include <cstdint>
#include <string>

enum class TextureEngine { Accurate = 0, Legacy = 1 };

TextureEngine textureEngine();                 // PPHAP_ENGINE env on first use, else Accurate
void setTextureEngine(TextureEngine engine);
const char* textureEngineName(TextureEngine engine);

class TextureConverter;
struct FrameSize;
#include <memory>
std::unique_ptr<TextureConverter> createAccurateTextureConverter(const FrameSize& frameSize,
                                                                 unsigned int destFormat, int quality);

// ---- quality report -------------------------------------------------------------------------
struct QualityReport {
    unsigned long long pixels = 0;        // colour pixels compared
    double sse[3] = { 0, 0, 0 };          // R G B squared error
    double sseLuma = 0;                   // Rec.709 luma
    int maxErr = 0;                       // worst single channel error (0-255)
    unsigned long long alphaPixels = 0;
    double sseAlpha = 0;
    unsigned long long dePixels = 0;      // CIEDE2000 (sRGB / D65), on every 2nd pixel each way
    double deSum = 0, deMax = 0;
    unsigned long long deHist[1001] = {}; // 0.05 wide bins, last bin = 50+
    long long frames = 0;

    double psnrRGB() const;
    double psnrChannel(int c) const;
    double psnrLuma() const;
    double psnrAlpha() const;
    double deMean() const;
    double dePercentile(double p) const;
};

void enableQualityReport(bool on);
bool qualityReportEnabled();
QualityReport qualityReport();                  // snapshot

// called by TextureConverter::convert after encoding one texture of one frame
void accumulateQuality(unsigned int textureFormat, int width, int height,
                       const uint8_t* rgbaIn, const uint8_t* texture);
