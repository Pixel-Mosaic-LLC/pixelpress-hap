// Times the bc7e Hap R encoder on this machine (all cores, "basic" = pphap/AE Normal quality)
// so build-mac.sh can pick the fastest ISPC NEON target for Apple Silicon.
// Output (last line): ms=<milliseconds per 1920x1080 frame> sum=<checksum of the BC7 data>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>
#include "bc7e_ispc.h"

int main()
{
    const int w = 1920, h = 1080, bw = w / 4, bh = h / 4;
    // deterministic photo-like test frame: smooth colour fields, edges and fine texture
    std::vector<uint32_t> img(size_t(w) * h);
    uint32_t seed = 12345;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            seed = seed * 1664525u + 1013904223u;
            int n = int(seed >> 27) - 16;
            double fx = x / double(w), fy = y / double(h);
            int r = int(128 + 100 * std::sin(fx * 7 + fy * 3)) + n;
            int g = int(128 + 90 * std::cos(fy * 9 - fx * 2)) + ((x / 64 + y / 64) & 1 ? 30 : -30) + n / 2;
            int b = int(255 * fx * fy) + (((x * x + y * y) >> 11) & 1 ? 20 : 0) + n;
            auto c = [](int v) { return uint32_t(std::clamp(v, 0, 255)); };
            img[size_t(y) * w + x] = c(r) | c(g) << 8 | c(b) << 16 | 0xFF000000u;
        }

    ispc::bc7e_compress_block_init();
    ispc::bc7e_compress_block_params params;
    ispc::bc7e_compress_block_params_init_basic(&params, true);
    std::vector<uint64_t> out(size_t(bw) * bh * 2);

    auto encode = [&]() {
        std::atomic<int> next{0};
        unsigned n = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < n; t++)
            pool.emplace_back([&]() {
                std::vector<uint32_t> px(size_t(bw) * 16);
                for (int by; (by = next++) < bh;) {
                    for (int bx = 0; bx < bw; bx++)
                        for (int i = 0; i < 16; i++)
                            px[size_t(bx) * 16 + i] = img[size_t(by * 4 + i / 4) * w + bx * 4 + i % 4];
                    ispc::bc7e_compress_blocks(bw, &out[size_t(by) * bw * 2], px.data(), &params);
                }
            });
        for (auto& t : pool) t.join();
    };

    encode(); // warm-up
    double best = 1e30;
    for (int rep = 0; rep < 5; rep++) {
        auto t0 = std::chrono::steady_clock::now();
        encode();
        best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    uint64_t sum = 1469598103934665603ull;
    for (uint64_t v : out) sum = (sum ^ v) * 1099511628211ull;
    std::printf("threads=%u\nms=%.1f sum=%016llx\n", std::thread::hardware_concurrency(), best, (unsigned long long)sum);
    return 0;
}
