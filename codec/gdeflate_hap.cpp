#include "gdeflate_hap.hpp"
#include "hap.h"
#include "GDeflate.h"
#include "TileStream.h"
#include <atomic>
#include <mutex>

namespace {
std::atomic<bool> g_enabled{ false };
std::atomic<int> g_level{ 6 };

size_t gdBound(size_t n) { return GDeflate::CompressBound(n); }
size_t gdCompress(void* dst, size_t dstSize, const void* src, size_t srcSize)
{
    size_t out = dstSize;
    if (!GDeflate::Compress((uint8_t*)dst, &out, (const uint8_t*)src, srcSize, (uint32_t)g_level.load(),
                            GDeflate::COMPRESS_SINGLE_THREAD))   // Hap chunks are already compressed in parallel
        return 0;
    return out;
}
int gdDecompress(void* dst, size_t dstSize, const void* src, size_t srcSize)
{
    return GDeflate::Decompress((uint8_t*)dst, dstSize, (const uint8_t*)src, srcSize, 1) ? 1 : 0;
}
size_t gdSize(const void* src, size_t srcSize)
{
    if (srcSize < sizeof(GDeflate::TileStream)) return 0;
    auto h = (const GDeflate::TileStream*)src;
    return h->IsValid() ? h->GetUncompressedSize() : 0;
}
}

namespace hapgd {
void registerCodec()
{
    static std::once_flag once;
    std::call_once(once, [] { HapSetGDeflate(gdBound, gdCompress, gdDecompress, gdSize); });
}
void setEnabled(bool on) { registerCodec(); g_enabled = on; }
bool enabled() { return g_enabled; }
void setLevel(int l) { g_level = l < 1 ? 1 : l > 12 ? 12 : l; }
int level() { return g_level; }
}
