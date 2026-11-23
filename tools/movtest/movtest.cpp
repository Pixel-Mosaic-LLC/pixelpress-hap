// End-to-end test of the plugin's encode + write path, outside Adobe:
// CodecRegistry -> HapEncoder(subtype) -> EncoderJob -> MovieWriter (FFmpeg mov muxer)
// Usage: movtest out.mov 4CC width height quality fps frame1.rgba [frame2.rgba ...]
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <vector>
#include "codec_registration.hpp"
#include "movie_writer.hpp"

int main(int argc, char** argv) {
    if (argc < 8) { fprintf(stderr, "usage\n"); return 2; }
    std::string out = argv[1];
    Codec4CC cc{ argv[2][0], argv[2][1], argv[2][2], argv[2][3] };
    int w = atoi(argv[3]), h = atoi(argv[4]), q = atoi(argv[5]), fps = atoi(argv[6]);
    FrameSize fs{ w, h };

    auto codec = CodecRegistry::codec();
    printf("codec: %s / %s, default subtype %.4s\n", codec->details().productName.c_str(),
           codec->details().fileFormatName.c_str(), codec->details().defaultSubType.data());

    auto params = std::make_unique<EncoderParametersBase>(fs, withAlpha, cc, HapChunkCounts{ 0, 0 }, q);
    UniqueEncoder encoder = codec->createEncoder(std::move(params));

    VideoDef video{ w, h, cc, codec->details().fileFormatShortName, encoder->encodedBitDepth(),
                    Rational{ fps, 1 }, argc - 7 };
    MovieWriter writer(0, createMovieFile(out), video, std::nullopt, false);
    writer.writeHeader();

    std::vector<uint8_t> rgba((size_t)w * h * 4);
    size_t total = 0;
    for (int i = 7; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary);
        f.read((char*)rgba.data(), rgba.size());
        // feed it exactly as After Effects does: 8-bit ARGB, top-left origin
        std::vector<uint8_t> argb(rgba.size());
        for (size_t p = 0; p < (size_t)w * h; ++p) {
            argb[p*4+0] = rgba[p*4+3]; argb[p*4+1] = rgba[p*4+0];
            argb[p*4+2] = rgba[p*4+1]; argb[p*4+3] = rgba[p*4+2];
        }
        auto job = encoder->create();
        job->copyExternalToLocal(argb.data(), (size_t)w * 4, FrameFormat(ChannelFormat_U8 | ChannelLayout_ARGB | FrameOrigin_TopLeft));
        EncodeOutput eo;
        job->encode(eo);
        writer.writeVideoFrame(eo.buffer.data(), eo.buffer.size());
        total += eo.buffer.size();
    }
    writer.flush();
    writer.writeTrailer();
    writer.close();
    printf("wrote %s: %d frames, %zu bytes of frame data\n", out.c_str(), argc - 7, total);
    return 0;
}
