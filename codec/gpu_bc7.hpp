#pragma once

// GPU BC7 encoder for Hap R (1.2.0).
//
// Microsoft's DirectXTex BC7 compute shaders (MIT; codec/shaders/bc7_vk.hlsl), compiled to SPIR-V
// and run through Vulkan. Vulkan is loaded at run time from the graphics driver (vulkan-1.dll on
// Windows, libvulkan.so.1 on Linux, libvulkan.1.dylib on a Mac with MoltenVK), so nothing new has
// to be installed and a machine without a usable GPU simply keeps the CPU encoder (bc7e).
//
// The encoder searches every BC7 mode the CPU "fast" setting searches and more (all 64 partitions
// of the 2-subset modes; the 3-subset modes too in GpuQuality::Thorough), so its output is not
// bit-identical to the CPU's but it is at least as accurate: see doc/gpu-encoding.md.

#include <cstdint>
#include <string>

namespace gpubc7 {

enum class Mode {
    Off,     // never use the GPU (the default for the Adobe plugin)
    Auto,    // use a hardware GPU if there is one, else the CPU (pphap / PixelPress default)
    On,      // like Auto, but say so loudly if no GPU could be used
    Force,   // also accept software Vulkan devices (llvmpipe/lavapipe): testing only
};

// set before the first encode; pphap calls this from -gpu / PPHAP_GPU
void setMode(Mode mode);
Mode mode();

// true if a GPU encoder is ready (initialises Vulkan on the first call; thread safe)
bool available();

// GPU name and Vulkan driver, or why the GPU is not used ("" before available() was called)
std::string description();

// Encode a whole RGBA8 frame (bytes R,G,B,A) to BC7 blocks, row-major, 16 bytes per 4x4 block.
// w and h must be multiples of 4 (callers pad by repeating the last row/column, as the CPU path
// does). Several threads may call this at once. Returns false on any GPU error; the caller then
// encodes the frame on the CPU.
bool encode(const uint8_t* rgba, int w, int h, uint8_t* blocksOut);

// 3-subset modes (BC7 modes 0 and 2): rarely chosen, about 2x the GPU time. Off by default.
void setThorough(bool on);

} // namespace gpubc7
