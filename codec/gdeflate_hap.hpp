#pragma once
// PixelPress HAP 1.2.2: GDeflate as Hap's second-stage compressor (non-standard Hap; see hap.h
// HapSetGDeflate). Encoding needs hapGDeflateSetEnabled(true); decoding works once registered.
namespace hapgd {
void registerCodec();              // safe to call repeatedly; called by the encoder and pphap
void setEnabled(bool on);          // encoder writes GDeflate chunks instead of Snappy
bool enabled();
void setLevel(int level);          // 1..12, default 6 (levels above 8 are 4-8x slower for ~1% smaller)
int level();
}
