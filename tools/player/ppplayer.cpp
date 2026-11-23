// PixelPress Player (Windows) - plays Hap / Hap Alpha / Hap Q / Hap Q Alpha / Hap Alpha-Only / Hap R
// QuickTime files, with Snappy (standard Hap) or GDeflate (PixelPress extension) chunk compression.
//
//   ppplayer [options] FILE [FILE ...]      or a playlist (.txt / .m3u: one file per line)
//
// Design (v1):
//  - The file index (from FFmpeg's mov demuxer) gives every frame's file offset, so frames are read
//    and decoded ahead, out of order, by a pool of worker threads (each frame is independent in Hap).
//  - Decoded frames are GPU-compressed textures (BC1/BC3/BC4/BC7/BC6H), uploaded as they are and
//    turned into pixels by the graphics card (D3D11). Hap Q's YCoCg is converted in the shader.
//  - Audio (any codec the bundled FFmpeg decodes: PCM, AAC, MP3) goes out through WASAPI and is the
//    clock: the frame shown is the one for the audio that is playing. Without audio, the system
//    clock (QueryPerformanceCounter) is used.
//  - Looping is seamless: the play position keeps counting, frame = position mod clip length.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <shellapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}
#include "hap.h"
#include "gdeflate_hap.hpp"

#ifndef PPPLAYER_VERSION
#define PPPLAYER_VERSION "dev"
#endif

// ------------------------------------------------------------------------------------------------
// small helpers

static std::mutex g_logMutex;
static void logf(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
[[noreturn]] static void die(const char* fmt, ...)
{
    char buf[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    fprintf(stderr, "ppplayer: error: %s\n", buf);
    if (!GetConsoleWindow()) MessageBoxA(nullptr, buf, "PixelPress Player", MB_ICONERROR);
    exit(1);
}
static std::wstring widen(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
static std::string narrow(const wchar_t* w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
static double nowSeconds()
{
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}
template <class T> static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ------------------------------------------------------------------------------------------------
// options

struct Options {
    std::vector<std::string> files;
    int display = 1;            // 1-based monitor number
    bool span = false;          // one window over all monitors (one canvas)
    bool windowed = false;      // 1280x720 window (testing)
    std::string scale = "fit";  // fit | fill | stretch | native
    std::string loop;           // clip | playlist | none ("" = clip for one file, playlist for more)
    bool audio = true;
    float volume = 1.0f;
    int threads = 0;
    bool stats = false;
    bool listDisplays = false;
    // testing
    int testFrames = 0;         // render N frames, then exit
    std::string screenshot;     // with testFrames: save the last frame (BMP)
    std::string testAudio;      // decode the first file's audio (48 kHz stereo float, 2 loops) to this file, exit
    double startAt = 0;
    // 0.2: sync across PCs
    std::string sync;           // "" | leader | follow
    int syncPort = 7654;
    std::string syncTarget = "255.255.255.255";   // leader: where state goes (broadcast, or one follower's IP)
    double syncOffsetMs = 0;    // follower: shift (+ = later), to match displays with different latency
    bool syncAudio = false;     // follower: play audio too (default: only the leader plays sound)
    bool bench = false;         // decode as fast as possible, report speed, exit
};

static void usage()
{
    fprintf(stderr,
"PixelPress Player " PPPLAYER_VERSION " - Hap / Hap Q / Hap R player (Snappy or GDeflate)\n\n"
"usage: ppplayer [options] FILE [FILE ...]     (or a playlist: .txt / .m3u, one file per line)\n\n"
"  -display N          play full screen on display N (default 1; see -list-displays)\n"
"  -span               one canvas over all displays (the clip is scaled across them)\n"
"  -window             in a 1280x720 window instead (testing)\n"
"  -list-displays      show the displays and their numbers\n"
"  -scale fit|fill|stretch|native   (default fit: whole picture, black bars if needed)\n"
"  -loop clip|playlist|none         (default: clip for one file, playlist for several)\n"
"  -an                 no audio      -volume 0-100\n"
"  -ss SECONDS         start position\n"
"  -threads N          decode threads (default: CPU threads, up to 16)\n"
"  -stats              print playback statistics every second\n"
"  -bench              decode every file as fast as possible and report the speed (no picture)\n\n"
"sync across PCs (same files and playlist on every PC, same network):\n"
"  -sync leader        this PC decides what plays and when (keys work here)\n"
"  -sync follow        follow the leader: same clip, same frame (only the leader plays audio)\n"
"  -sync-offset MS     follower: show frames MS later (+) or earlier (-) to line up displays\n"
"  -sync-audio         follower: play audio as well\n"
"  -sync-port N        UDP port (default 7654; also uses N+1)\n"
"  -sync-target IP     leader: send to this address instead of the whole network (broadcast)\n\n"
"keys: Space pause/play   Right/N next   Left/P previous   Home restart   1-9 clip 1-9\n"
"      L loop mode   Esc quit\n");
}

static Options parseArgs(int argc, wchar_t** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = narrow(argv[i]);
        auto need = [&]() -> std::string { if (i + 1 >= argc) die("%s needs a value", a.c_str()); return narrow(argv[++i]); };
        if (a == "-h" || a == "-help" || a == "--help" || a == "/?") { usage(); exit(0); }
        else if (a == "-version") { printf("ppplayer " PPPLAYER_VERSION "\n"); exit(0); }
        else if (a == "-display") o.display = atoi(need().c_str());
        else if (a == "-span") o.span = true;
        else if (a == "-window") o.windowed = true;
        else if (a == "-list-displays") o.listDisplays = true;
        else if (a == "-scale") o.scale = need();
        else if (a == "-loop") o.loop = need();
        else if (a == "-an") o.audio = false;
        else if (a == "-volume") o.volume = std::clamp(float(atof(need().c_str())) / 100.0f, 0.0f, 1.0f);
        else if (a == "-threads") o.threads = atoi(need().c_str());
        else if (a == "-stats") o.stats = true;
        else if (a == "-ss") o.startAt = atof(need().c_str());
        else if (a == "-test-frames") o.testFrames = atoi(need().c_str());
        else if (a == "-screenshot") o.screenshot = need();
        else if (a == "-test-audio") o.testAudio = need();
        else if (a == "-sync") o.sync = need();
        else if (a == "-sync-port") o.syncPort = atoi(need().c_str());
        else if (a == "-sync-target") o.syncTarget = need();
        else if (a == "-sync-offset") o.syncOffsetMs = atof(need().c_str());
        else if (a == "-sync-audio") o.syncAudio = true;
        else if (a == "-bench") o.bench = true;
        else if (!a.empty() && a[0] == '-' && a.size() > 1) die("unknown option %s (ppplayer -h)", a.c_str());
        else {
            std::string lower = a; for (auto& c : lower) c = (char)tolower((unsigned char)c);
            auto ends = [&](const char* e) { size_t n = strlen(e); return lower.size() >= n && lower.compare(lower.size() - n, n, e) == 0; };
            if (ends(".txt") || ends(".m3u") || ends(".m3u8")) {
                std::ifstream f(widen(a).c_str());
                if (!f) die("can't open playlist %s", a.c_str());
                std::string base = a.substr(0, a.find_last_of("\\/") + 1), line;
                while (std::getline(f, line)) {
                    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                    if (line.size() >= 3 && (unsigned char)line[0] == 0xEF) line = line.substr(3);   // UTF-8 BOM
                    if (line.empty() || line[0] == '#') continue;
                    bool absolute = line.size() > 1 && (line[1] == ':' || line[0] == '\\' || line[0] == '/');
                    o.files.push_back(absolute ? line : base + line);
                }
            } else o.files.push_back(a);
        }
    }
    if (o.loop.empty()) o.loop = o.files.size() > 1 ? "playlist" : "clip";
    if (!o.sync.empty() && o.sync != "leader" && o.sync != "follow") die("-sync must be leader or follow");
    if (o.loop != "clip" && o.loop != "playlist" && o.loop != "none") die("-loop must be clip, playlist or none");
    if (o.scale != "fit" && o.scale != "fill" && o.scale != "stretch" && o.scale != "native") die("-scale must be fit, fill, stretch or native");
    return o;
}

// ------------------------------------------------------------------------------------------------
// displays

struct Display { RECT rect; std::string name; bool primary; };
static std::vector<Display> listDisplays()
{
    std::vector<Display> d;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
        MONITORINFOEXW mi{}; mi.cbSize = sizeof mi;
        GetMonitorInfoW(m, &mi);
        ((std::vector<Display>*)p)->push_back({ mi.rcMonitor, narrow(mi.szDevice), (mi.dwFlags & MONITORINFOF_PRIMARY) != 0 });
        return TRUE;
    }, (LPARAM)&d);
    // primary first, then left to right, top to bottom (stable numbering for show files)
    std::stable_sort(d.begin(), d.end(), [](const Display& a, const Display& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.rect.left != b.rect.left) return a.rect.left < b.rect.left;
        return a.rect.top < b.rect.top;
    });
    return d;
}

// ------------------------------------------------------------------------------------------------
// clip: file index + random-access frame reads

enum class TexKind { RGB_BC1, RGBA_BC3, YCoCg_BC3, YCoCgA_BC3_BC4, Alpha_BC4, RGBA_BC7, RGB_BC6U, RGB_BC6S };

struct Clip {
    std::string path;
    int width = 0, height = 0;
    double fps = 30;
    uint32_t fourcc = 0;
    std::vector<std::pair<int64_t, int>> frames;     // file offset, size
    // audio
    int audioStream = -1;
    AVCodecParameters* audioPar = nullptr;
    AVRational audioTimeBase{ 1, 1 };
    std::vector<std::tuple<int64_t, int, int64_t>> audioPackets;   // offset, size, pts
    AVFormatContext* fmt = nullptr;

    ~Clip() { if (fmt) avformat_close_input(&fmt); }
    double duration() const { return frames.size() / fps; }
    std::string codecName() const
    {
        switch (fourcc) {
        case MKTAG('H','a','p','1'): return "Hap";
        case MKTAG('H','a','p','5'): return "Hap Alpha";
        case MKTAG('H','a','p','Y'): return "Hap Q";
        case MKTAG('H','a','p','M'): return "Hap Q Alpha";
        case MKTAG('H','a','p','A'): return "Hap Alpha-Only";
        case MKTAG('H','a','p','7'): return "Hap R";
        case MKTAG('H','a','p','H'): return "Hap HDR";
        }
        return "?";
    }
};

static std::unique_ptr<Clip> openClip(const std::string& path)
{
    auto c = std::make_unique<Clip>();
    c->path = path;
    if (avformat_open_input(&c->fmt, path.c_str(), nullptr, nullptr) < 0) throw std::runtime_error("can't open " + path);
    if (avformat_find_stream_info(c->fmt, nullptr) < 0) throw std::runtime_error("can't read " + path);
    int vs = -1;
    for (unsigned i = 0; i < c->fmt->nb_streams; ++i) {
        AVStream* st = c->fmt->streams[i];
        uint32_t tag = st->codecpar->codec_tag;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vs < 0 &&
            (tag & 0xFFFFFF) == MKTAG('H','a','p',0)) { vs = (int)i; c->fourcc = tag; }
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && c->audioStream < 0) c->audioStream = (int)i;
    }
    if (vs < 0) throw std::runtime_error(path + " has no Hap video track (only Hap / Hap Q / Hap R files play)");
    AVStream* st = c->fmt->streams[vs];
    c->width = st->codecpar->width; c->height = st->codecpar->height;
    AVRational r = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    if (r.num && r.den) c->fps = av_q2d(r);
    for (int i = 0; i < st->nb_index_entries; ++i) {
        const AVIndexEntry& e = st->index_entries[i];
        if (e.flags & AVINDEX_DISCARD_FRAME) continue;
        c->frames.push_back({ e.pos, e.size });
    }
    if (c->frames.empty()) throw std::runtime_error(path + ": no frames");
    if (c->audioStream >= 0) {
        AVStream* as = c->fmt->streams[c->audioStream];
        c->audioPar = as->codecpar;
        c->audioTimeBase = as->time_base;
        for (int i = 0; i < as->nb_index_entries; ++i) {
            const AVIndexEntry& e = as->index_entries[i];
            if (e.flags & AVINDEX_DISCARD_FRAME) continue;
            c->audioPackets.push_back({ e.pos, e.size, e.timestamp });
        }
        if (c->audioPackets.empty()) c->audioStream = -1;
    }
    return c;
}

// positional file reads (one handle per thread: no shared file pointer)
struct FileReader {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit FileReader(const std::string& path)
    {
        h = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
    }
    ~FileReader() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool read(int64_t pos, void* dst, int size)
    {
        OVERLAPPED ov{}; ov.Offset = DWORD(pos); ov.OffsetHigh = DWORD(pos >> 32);
        DWORD got = 0;
        return ReadFile(h, dst, (DWORD)size, &got, &ov) && got == (DWORD)size;
    }
};

// ------------------------------------------------------------------------------------------------
// decoded frame

struct DecodedFrame {
    int64_t index = -1;            // play position (frame counter, keeps counting over loops)
    std::vector<uint8_t> tex[2];   // compressed texture data (BCn blocks)
    unsigned fmt[2] = { 0, 0 };    // HapTextureFormat
    int count = 0;
    bool ok = false;
};

static void hapSerial(HapDecodeWorkFunction fn, void* p, unsigned int count, void*)
{
    for (unsigned i = 0; i < count; ++i) fn(p, i);
}

static size_t texBytes(unsigned fmt, int w, int h)
{
    size_t blocks = size_t((w + 3) / 4) * ((h + 3) / 4);
    switch (fmt) {
    case HapTextureFormat_RGB_DXT1: case HapTextureFormat_A_RGTC1: return blocks * 8;
    default: return blocks * 16;
    }
}

static bool decodeFrame(const Clip& c, FileReader& rd, std::vector<uint8_t>& packet, int64_t playIndex, DecodedFrame& out)
{
    const auto& fr = c.frames[size_t(playIndex % (int64_t)c.frames.size())];
    packet.resize(fr.second);
    out.index = playIndex; out.ok = false;
    if (!rd.read(fr.first, packet.data(), fr.second)) return false;
    unsigned int count = 0;
    if (HapGetFrameTextureCount(packet.data(), (unsigned long)packet.size(), &count) != HapResult_No_Error || count < 1 || count > 2)
        return false;
    out.count = (int)count;
    for (unsigned t = 0; t < count; ++t) {
        unsigned fmt = 0;
        if (HapGetFrameTextureFormat(packet.data(), (unsigned long)packet.size(), t, &fmt) != HapResult_No_Error) return false;
        out.tex[t].resize(texBytes(fmt, c.width, c.height));
        unsigned long used = 0; unsigned outFmt = 0;
        unsigned r = HapDecode(packet.data(), (unsigned long)packet.size(), t, hapSerial, nullptr,
                               out.tex[t].data(), (unsigned long)out.tex[t].size(), &used, &outFmt);
        if (r != HapResult_No_Error) return false;
        out.fmt[t] = outFmt;
    }
    out.ok = true;
    return true;
}

// ------------------------------------------------------------------------------------------------
// decode-ahead: workers decode play positions [cur, cur + ahead) out of order

class FrameQueue {
public:
    FrameQueue(const Clip& clip, int threads, int ahead) : clip_(clip), ahead_(ahead)
    {
        for (int i = 0; i < threads; ++i) workers_.emplace_back([this] { work(); });
    }
    ~FrameQueue()
    {
        { std::lock_guard<std::mutex> lk(m_); quit_ = true; }
        cv_.notify_all();
        for (auto& t : workers_) t.join();
    }
    // the renderer wants position `pos`: frames before it are dropped, decoding continues from it
    void setPosition(int64_t pos, bool loop)
    {
        std::lock_guard<std::mutex> lk(m_);
        loop_ = loop;
        if (pos < cur_ || pos > next_ + ahead_) next_ = pos;    // a jump: restart decoding there
        cur_ = pos;
        for (auto it = ready_.begin(); it != ready_.end();) it = it->first < pos ? ready_.erase(it) : std::next(it);
        if (next_ < pos) next_ = pos;
        cv_.notify_all();
    }
    // newest decoded frame at or before pos (nullptr if none)
    std::shared_ptr<DecodedFrame> get(int64_t pos)
    {
        std::lock_guard<std::mutex> lk(m_);
        auto it = ready_.upper_bound(pos);
        if (it == ready_.begin()) return nullptr;
        return std::prev(it)->second;
    }
    std::shared_ptr<DecodedFrame> waitFor(int64_t pos, double timeoutSec)
    {
        std::unique_lock<std::mutex> lk(m_);
        cv2_.wait_for(lk, std::chrono::duration<double>(timeoutSec), [&] { return ready_.count(pos) || failed_; });
        auto it = ready_.find(pos);
        return it == ready_.end() ? nullptr : it->second;
    }
    std::atomic<int64_t> decoded{ 0 }, errors{ 0 };

private:
    void work()
    {
        FileReader rd(clip_.path);
        std::vector<uint8_t> packet;
        for (;;) {
            int64_t pos;
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return quit_ || next_ < cur_ + ahead_; });
                if (quit_) return;
                pos = next_++;
                if (!loop_ && pos >= (int64_t)clip_.frames.size()) { next_--; cv_.wait_for(lk, std::chrono::milliseconds(20)); continue; }
            }
            auto f = std::make_shared<DecodedFrame>();
            bool ok = decodeFrame(clip_, rd, packet, pos, *f);
            {
                std::lock_guard<std::mutex> lk(m_);
                if (ok) { if (pos >= cur_) ready_[pos] = f; decoded++; }
                else { errors++; if (errors > 50) failed_ = true; }
            }
            cv2_.notify_all();
        }
    }
    const Clip& clip_;
    int ahead_;
    std::mutex m_;
    std::condition_variable cv_, cv2_;
    std::map<int64_t, std::shared_ptr<DecodedFrame>> ready_;
    int64_t cur_ = 0, next_ = 0;
    bool loop_ = true, quit_ = false, failed_ = false;
    std::vector<std::thread> workers_;
};

// ------------------------------------------------------------------------------------------------
// audio: WASAPI shared mode; the samples played so far are the media clock

class AudioOut {
public:
    bool init(float volume)
    {
        volume_ = volume;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&enum_))) return false;
        if (FAILED(enum_->GetDefaultAudioEndpoint(eRender, eConsole, &dev_))) return false;
        if (FAILED(dev_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client_))) return false;
        WAVEFORMATEX* mix = nullptr;
        if (FAILED(client_->GetMixFormat(&mix))) return false;
        rate_ = (int)mix->nSamplesPerSec;
        channels_ = mix->nChannels;
        // float32 interleaved at the device's rate and channel count (the shared-mode mix format)
        WAVEFORMATEXTENSIBLE wf{};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = (WORD)channels_;
        wf.Format.nSamplesPerSec = (DWORD)rate_;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = WORD(4 * channels_);
        wf.Format.nAvgBytesPerSec = DWORD(rate_ * wf.Format.nBlockAlign);
        wf.Format.cbSize = 22;
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? ((WAVEFORMATEXTENSIBLE*)mix)->dwChannelMask
                                                                     : (channels_ == 2 ? 3 : 0);
        wf.SubFormat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };   // IEEE float
        mask_ = wf.dwChannelMask;
        CoTaskMemFree(mix);
        const REFERENCE_TIME bufferDur = 1000000;   // 100 ms
        HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                         bufferDur, 0, (WAVEFORMATEX*)&wf, nullptr);
        if (FAILED(hr)) return false;
        client_->GetBufferSize(&bufFrames_);
        if (FAILED(client_->GetService(__uuidof(IAudioRenderClient), (void**)&render_))) return false;
        if (FAILED(client_->GetService(__uuidof(IAudioClock), (void**)&clock_))) return false;
        clock_->GetFrequency(&clockFreq_);
        return true;
    }
    ~AudioOut() { stop(); release(clock_); release(render_); release(client_); release(dev_); release(enum_); }
    int rate() const { return rate_; }
    int channels() const { return channels_; }
    uint64_t channelMask() const { return mask_; }

    // the feeder supplies interleaved float samples on demand: fill(dst, frames) -> frames written
    void start(std::function<int(float*, int)> fill)
    {
        fill_ = std::move(fill);
        quit_ = false;
        // preroll the whole buffer so playback starts with sound
        pump();
        client_->Start();
        started_ = true;
        thread_ = std::thread([this] { while (!quit_) { pump(); Sleep(5); } });
    }
    void stop()
    {
        quit_ = true;
        if (thread_.joinable()) thread_.join();
        if (client_ && started_) { client_->Stop(); client_->Reset(); }
        started_ = false;
    }
    void pause(bool p) { if (!client_ || !started_) return; if (p) client_->Stop(); else client_->Start(); }
    // seconds of audio actually played since start()
    double position() const
    {
        if (!clock_) return 0;
        UINT64 pos = 0, qpc = 0;
        if (FAILED(clock_->GetPosition(&pos, &qpc)) || !clockFreq_) return 0;
        return double(pos) / double(clockFreq_);
    }

private:
    void pump()
    {
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) return;
        UINT32 avail = bufFrames_ - padding;
        if (!avail) return;
        BYTE* data = nullptr;
        if (FAILED(render_->GetBuffer(avail, &data))) return;
        float* f = (float*)data;
        int got = fill_ ? fill_(f, (int)avail) : 0;
        if (got < (int)avail) memset(f + size_t(got) * channels_, 0, size_t(avail - got) * channels_ * 4);
        if (volume_ != 1.0f) for (size_t i = 0; i < size_t(avail) * channels_; ++i) f[i] *= volume_;
        render_->ReleaseBuffer(avail, 0);
    }
    IMMDeviceEnumerator* enum_ = nullptr;
    IMMDevice* dev_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    IAudioClock* clock_ = nullptr;
    UINT64 clockFreq_ = 0;
    UINT32 bufFrames_ = 0;
    int rate_ = 48000, channels_ = 2;
    uint64_t mask_ = 3;
    float volume_ = 1.0f;
    std::function<int(float*, int)> fill_;
    std::thread thread_;
    std::atomic<bool> quit_{ false };
    bool started_ = false;
};

// decodes a clip's audio to the device format; loops with the video (each pass is exactly the
// video's length: cut or padded with silence)
class AudioSource {
public:
    AudioSource(const Clip& c, int outRate, int outChannels, uint64_t outMask, bool loop, double startSec)
        : clip_(c), rd_(c.path), outRate_(outRate), outCh_(outChannels), loop_(loop)
    {
        const AVCodec* dec = avcodec_find_decoder(c.audioPar->codec_id);
        if (!dec) throw std::runtime_error("no decoder for this audio");
        ctx_ = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(ctx_, c.audioPar);
        ctx_->pkt_timebase = c.audioTimeBase;
        if (avcodec_open2(ctx_, dec, nullptr) < 0) throw std::runtime_error("can't open the audio decoder");
        int64_t inLayout = ctx_->channel_layout ? (int64_t)ctx_->channel_layout : av_get_default_channel_layout(ctx_->channels);
        int64_t outLayout = outMask ? (int64_t)outMask : av_get_default_channel_layout(outChannels);
        swr_ = swr_alloc_set_opts(nullptr, outLayout, AV_SAMPLE_FMT_FLT, outRate, inLayout, ctx_->sample_fmt, ctx_->sample_rate, 0, nullptr);
        if (!swr_ || swr_init(swr_) < 0) throw std::runtime_error("can't convert this audio");
        passFrames_ = (int64_t)llround(c.duration() * outRate);
        frame_ = av_frame_alloc();
        pkt_ = av_packet_alloc();
        seek(startSec);
    }
    ~AudioSource() { av_frame_free(&frame_); av_packet_free(&pkt_); swr_free(&swr_); avcodec_free_context(&ctx_); }

    // position inside the current pass, in output samples
    void seek(double sec)
    {
        std::lock_guard<std::mutex> lk(m_);
        int64_t target = (int64_t)llround(sec * outRate_);
        passPos_ = passFrames_ ? target % passFrames_ : 0;
        restartPass(passPos_);
    }
    int fill(float* dst, int frames)
    {
        std::lock_guard<std::mutex> lk(m_);
        int done = 0;
        while (done < frames) {
            if (passPos_ >= passFrames_) {
                if (!loop_) break;
                restartPass(0);
            }
            int want = (int)std::min<int64_t>(frames - done, passFrames_ - passPos_);
            int got = take(dst + size_t(done) * outCh_, want);
            if (got < want) {   // audio shorter than video: silence to the end of the pass
                memset(dst + size_t(done + got) * outCh_, 0, size_t(want - got) * outCh_ * 4);
                got = want;
            }
            done += got; passPos_ += got;
        }
        return done;
    }

private:
    // start decoding at output sample `skip` of the pass
    void restartPass(int64_t skip)
    {
        passPos_ = skip;
        fifo_.clear(); fifoPos_ = 0;
        avcodec_flush_buffers(ctx_);
        swr_init(swr_);
        // first packet whose start is at or before the wanted time (AAC needs one packet of preroll)
        double t = double(skip) / outRate_;
        int64_t pts = (int64_t)(t / av_q2d(clip_.audioTimeBase));
        size_t i = 0;
        while (i + 1 < clip_.audioPackets.size() && std::get<2>(clip_.audioPackets[i + 1]) <= pts) ++i;
        if (i > 0) --i;
        pktIndex_ = i;
        int64_t startPts = std::get<2>(clip_.audioPackets[i]);
        drop_ = (int64_t)llround((t - startPts * av_q2d(clip_.audioTimeBase)) * outRate_);
        if (drop_ < 0) drop_ = 0;
    }
    int take(float* dst, int frames)
    {
        int done = 0;
        while (done < frames) {
            size_t availFrames = (fifo_.size() - fifoPos_) / outCh_;
            if (availFrames == 0) { if (!decodeMore()) break; continue; }
            int n = (int)std::min<size_t>(availFrames, frames - done);
            memcpy(dst + size_t(done) * outCh_, fifo_.data() + fifoPos_, size_t(n) * outCh_ * 4);
            fifoPos_ += size_t(n) * outCh_; done += n;
            if (fifoPos_ == fifo_.size()) { fifo_.clear(); fifoPos_ = 0; }
        }
        return done;
    }
    bool decodeMore()
    {
        for (;;) {
            int r = avcodec_receive_frame(ctx_, frame_);
            if (r == 0) {
                int maxOut = swr_get_out_samples(swr_, frame_->nb_samples) + 16;
                std::vector<float> out(size_t(maxOut) * outCh_);
                uint8_t* o = (uint8_t*)out.data();
                int n = swr_convert(swr_, &o, maxOut, (const uint8_t**)frame_->extended_data, frame_->nb_samples);
                av_frame_unref(frame_);
                if (n <= 0) continue;
                int skip = (int)std::min<int64_t>(drop_, n);
                drop_ -= skip;
                fifo_.insert(fifo_.end(), out.begin() + size_t(skip) * outCh_, out.begin() + size_t(n) * outCh_);
                if (n > skip) return true;
                continue;
            }
            if (r != AVERROR(EAGAIN)) return false;
            if (pktIndex_ >= clip_.audioPackets.size()) return false;
            auto& p = clip_.audioPackets[pktIndex_++];
            av_packet_unref(pkt_);
            if (av_new_packet(pkt_, std::get<1>(p)) < 0) return false;
            if (!rd_.read(std::get<0>(p), pkt_->data, std::get<1>(p))) return false;
            pkt_->pts = pkt_->dts = std::get<2>(p);
            avcodec_send_packet(ctx_, pkt_);
        }
    }
    const Clip& clip_;
    FileReader rd_;
    int outRate_, outCh_;
    bool loop_;
    AVCodecContext* ctx_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    std::vector<float> fifo_;
    size_t fifoPos_ = 0;
    size_t pktIndex_ = 0;
    int64_t drop_ = 0;
    int64_t passFrames_ = 0, passPos_ = 0;
    std::mutex m_;
};

// ------------------------------------------------------------------------------------------------
// sync across PCs (0.2)
//
// The leader broadcasts its state ~100x a second over UDP: clip number, media time, and its own clock
// at that moment. Followers measure the offset between their clock and the leader's with ping/pong
// round trips (keeping the fastest ones, as NTP does), so they can work out the leader's media time
// "now" from any state packet, however late it arrived. Each follower then shows the frame for that
// time. Accuracy is limited by the clock estimate (typically well under 1 ms on a wired LAN) and the
// displays' own refresh timing; true frame lock (genlock) needs sync hardware (NVIDIA Sync).

#pragma pack(push, 1)
struct SyncPacket {
    uint32_t magic;          // 'PPSY'
    uint16_t version;        // 1
    uint16_t type;           // 1 state, 2 ping, 3 pong
    uint32_t session;        // leader's random id (a restarted leader is noticed)
    int32_t clip;            // playlist index
    uint32_t flags;          // 1 paused, 2 looping clip
    double mediaTime;        // seconds into the clip (keeps counting over loops)
    double leaderClock;      // leader's clock when mediaTime was true / when the pong was sent
    double pingClock;        // follower's clock when it sent the ping (echoed in the pong)
};
#pragma pack(pop)
static const uint32_t kSyncMagic = 0x59535050;   // "PPSY"

static bool g_wsaReady = false;
static void wsaInit() { if (!g_wsaReady) { WSADATA w; g_wsaReady = WSAStartup(MAKEWORD(2, 2), &w) == 0; } }

class SyncLeader {
public:
    SyncLeader(int port, const std::string& target)
    {
        wsaInit();
        session_ = (uint32_t)GetTickCount() ^ (uint32_t)GetCurrentProcessId() << 16;
        out_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BOOL yes = TRUE;
        setsockopt(out_, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof yes);
        dst_.sin_family = AF_INET; dst_.sin_port = htons((u_short)port);
        inet_pton(AF_INET, target.c_str(), &dst_.sin_addr);
        // pings arrive on port + 1
        in_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)(port + 1)); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(in_, (sockaddr*)&a, sizeof a) != 0) logf("ppplayer: sync: port %d is in use - is another leader running here?", port + 1);
        DWORD tmo = 200; setsockopt(in_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
        thread_ = std::thread([this] { pongLoop(); });
        logf("ppplayer: sync: leading on UDP %d (to %s)", port, target.c_str());
    }
    ~SyncLeader() { quit_ = true; closesocket(in_); if (thread_.joinable()) thread_.join(); closesocket(out_); }
    void publish(int clip, double mediaTime, bool paused, bool looping)
    {
        double now = nowSeconds();
        if (now - last_ < 0.01) return;
        last_ = now;
        SyncPacket p{ kSyncMagic, 1, 1, session_, clip, uint32_t((paused ? 1 : 0) | (looping ? 2 : 0)), mediaTime, now, 0 };
        sendto(out_, (const char*)&p, sizeof p, 0, (sockaddr*)&dst_, sizeof dst_);
    }
private:
    void pongLoop()
    {
        while (!quit_) {
            SyncPacket p; sockaddr_in from{}; int fl = sizeof from;
            int n = recvfrom(in_, (char*)&p, sizeof p, 0, (sockaddr*)&from, &fl);
            if (n != (int)sizeof p || p.magic != kSyncMagic || p.type != 2) continue;
            p.type = 3; p.session = session_; p.leaderClock = nowSeconds();
            sendto(in_, (const char*)&p, sizeof p, 0, (sockaddr*)&from, fl);
        }
    }
    SOCKET out_ = INVALID_SOCKET, in_ = INVALID_SOCKET;
    sockaddr_in dst_{};
    uint32_t session_ = 0;
    double last_ = 0;
    std::atomic<bool> quit_{ false };
    std::thread thread_;
};

class SyncFollower {
public:
    struct Target { int clip = -1; double mediaTime = 0; bool paused = false, looping = true; bool valid = false; double age = 1e9; };
    SyncFollower(int port, double offsetMs) : offset_(offsetMs / 1000.0)
    {
        wsaInit();
        in_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        BOOL yes = TRUE;
        setsockopt(in_, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);   // several followers on one PC
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port); a.sin_addr.s_addr = INADDR_ANY;
        if (bind(in_, (sockaddr*)&a, sizeof a) != 0) logf("ppplayer: sync: can't listen on UDP %d", port);
        DWORD tmo = 50; setsockopt(in_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
        ping_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        setsockopt(ping_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof tmo);
        port_ = port;
        rx_ = std::thread([this] { stateLoop(); });
        px_ = std::thread([this] { pingLoop(); });
        logf("ppplayer: sync: following on UDP %d%s", port, offsetMs ? (" (offset " + std::to_string(offsetMs) + " ms)").c_str() : "");
    }
    ~SyncFollower() { quit_ = true; closesocket(in_); closesocket(ping_); rx_.join(); px_.join(); }
    // the leader's state now (media time advanced to this moment)
    Target target()
    {
        std::lock_guard<std::mutex> lk(m_);
        Target t;
        if (!haveState_ || !haveOffset_) return t;
        double now = nowSeconds();
        t.valid = true;
        t.clip = st_.clip;
        t.paused = st_.flags & 1;
        t.looping = (st_.flags & 2) != 0;
        t.age = now - rxTime_;
        double leaderNow = now + clockOffset_;
        t.mediaTime = t.paused ? st_.mediaTime : st_.mediaTime + (leaderNow - st_.leaderClock) - offset_;
        return t;
    }
    double clockOffsetMs() { std::lock_guard<std::mutex> lk(m_); return clockOffset_ * 1000; }
    double rttMs() { std::lock_guard<std::mutex> lk(m_); return bestRtt_ * 1000; }
private:
    void stateLoop()
    {
        while (!quit_) {
            SyncPacket p; sockaddr_in from{}; int fl = sizeof from;
            int n = recvfrom(in_, (char*)&p, sizeof p, 0, (sockaddr*)&from, &fl);
            if (n != (int)sizeof p || p.magic != kSyncMagic || p.type != 1) continue;
            std::lock_guard<std::mutex> lk(m_);
            if (session_ != p.session) {   // new leader (or restarted): measure the clock again
                if (session_) logf("ppplayer: sync: new leader");
                session_ = p.session; samples_.clear(); haveOffset_ = false;
                leader_ = from; leader_.sin_port = htons((u_short)(port_ + 1));
                haveLeader_ = true;
            }
            st_ = p; rxTime_ = nowSeconds(); haveState_ = true;
        }
    }
    void pingLoop()
    {
        int sent = 0;
        while (!quit_) {
            sockaddr_in dst; bool have;
            { std::lock_guard<std::mutex> lk(m_); have = haveLeader_; dst = leader_; }
            if (!have) { Sleep(20); continue; }
            SyncPacket p{ kSyncMagic, 1, 2, 0, 0, 0, 0, 0, nowSeconds() };
            sendto(ping_, (const char*)&p, sizeof p, 0, (sockaddr*)&dst, sizeof dst);
            SyncPacket r; sockaddr_in from{}; int fl = sizeof from;
            int n = recvfrom(ping_, (char*)&r, sizeof r, 0, (sockaddr*)&from, &fl);
            double now = nowSeconds();
            if (n == (int)sizeof r && r.magic == kSyncMagic && r.type == 3) {
                double rtt = now - r.pingClock;
                double off = r.leaderClock - (r.pingClock + rtt / 2);
                std::lock_guard<std::mutex> lk(m_);
                samples_.push_back({ rtt, off });
                if (samples_.size() > 64) samples_.pop_front();
                // offset = mean of the 8 fastest round trips (least queueing delay)
                std::vector<std::pair<double, double>> v(samples_.begin(), samples_.end());
                std::sort(v.begin(), v.end());
                size_t k = std::min<size_t>(8, v.size());
                double sum = 0; for (size_t i = 0; i < k; ++i) sum += v[i].second;
                double est = sum / k;
                // smooth changes once settled (no visible jumps from one noisy sample)
                clockOffset_ = haveOffset_ ? clockOffset_ + (est - clockOffset_) * 0.2 : est;
                bestRtt_ = v[0].first;
                if (!haveOffset_ && samples_.size() >= 4) {
                    haveOffset_ = true;
                    logf("ppplayer: sync: locked to leader (round trip %.2f ms)", bestRtt_ * 1000);
                }
            }
            ++sent;
            Sleep(sent < 50 ? 20 : 100);
        }
    }
    SOCKET in_ = INVALID_SOCKET, ping_ = INVALID_SOCKET;
    int port_ = 0;
    double offset_ = 0;
    std::mutex m_;
    SyncPacket st_{};
    double rxTime_ = 0;
    bool haveState_ = false, haveOffset_ = false, haveLeader_ = false;
    uint32_t session_ = 0;
    sockaddr_in leader_{};
    std::deque<std::pair<double, double>> samples_;
    double clockOffset_ = 0, bestRtt_ = 0;
    std::atomic<bool> quit_{ false };
    std::thread rx_, px_;
};

// ------------------------------------------------------------------------------------------------
// renderer (D3D11)

static const char* kShader = R"(
Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
SamplerState s0 : register(s0);
cbuffer C : register(b0) { float4 rect; float4 uvScale; int mode; int3 pad; };
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vs(uint id : SV_VertexID) {
    float2 p = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);
    VO o;
    o.pos = float4(lerp(rect.x, rect.z, p.x), lerp(rect.y, rect.w, p.y), 0, 1);
    o.uv = float2(p.x, 1.0 - p.y) * uvScale.xy;
    return o;
}
float3 ycocg(float4 c) {
    float scale = (c.z * (255.0 / 8.0)) + 1.0;
    float co = (c.x - (0.5 * 256.0 / 255.0)) / scale;
    float cg = (c.y - (0.5 * 256.0 / 255.0)) / scale;
    float y = c.w;
    return float3(y + co - cg, y + cg, y - co - cg);
}
float4 ps(VO i) : SV_Target {
    float4 c = t0.Sample(s0, i.uv);
    // modes: 0 rgb, 1 rgba, 2 ycocg, 3 ycocg + alpha (t1), 4 alpha only, 5 hdr (clamped)
    if (mode == 0) return float4(c.rgb, 1);
    if (mode == 1) return float4(c.rgb * c.a, 1);             // over black
    if (mode == 2) return float4(ycocg(c), 1);
    if (mode == 3) { float a = t1.Sample(s0, i.uv).r; return float4(ycocg(c) * a, 1); }
    if (mode == 4) return float4(c.rrr, 1);
    return float4(saturate(c.rgb), 1);
}
)";

typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

class Renderer {
public:
    bool init(HWND hwnd, int w, int h)
    {
        hwnd_ = hwnd;
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = w; sd.BufferDesc.Height = h;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        D3D_FEATURE_LEVEL fl;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                                   &sd, &swap_, &dev_, &fl, &ctx_);
        if (FAILED(hr)) {   // older Windows / drivers: blit model
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; sd.BufferCount = 1;
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                               &sd, &swap_, &dev_, &fl, &ctx_);
        }
        if (FAILED(hr)) { logf("ppplayer: Direct3D 11 could not start (0x%08lx)", (unsigned long)hr); return false; }
        {
            IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(dev_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && SUCCEEDED(dd->GetAdapter(&ad))) {
                DXGI_ADAPTER_DESC desc; ad->GetDesc(&desc); gpuName_ = narrow(desc.Description);
            }
            release(ad); release(dd);
        }
        width_ = w; height_ = h;
        if (!makeTarget()) return false;
        // shaders, compiled at start (d3dcompiler_47.dll is part of Windows 10/11)
        HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
        if (!dc) dc = LoadLibraryA("d3dcompiler_43.dll");
        auto compile = dc ? (PFN_D3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
        if (!compile) { logf("ppplayer: d3dcompiler_47.dll missing"); return false; }
        ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
        if (FAILED(compile(kShader, strlen(kShader), "player", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
            FAILED(compile(kShader, strlen(kShader), "player", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
            logf("ppplayer: shader error: %s", err ? (const char*)err->GetBufferPointer() : "?");
            return false;
        }
        dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
        dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);
        release(vsb); release(psb); release(err);
        D3D11_SAMPLER_DESC smp{};
        smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.MaxLOD = D3D11_FLOAT32_MAX;
        dev_->CreateSamplerState(&smp, &sampler_);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 48; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        dev_->CreateBuffer(&bd, nullptr, &cb_);
        D3D11_RASTERIZER_DESC rs{}; rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE;
        dev_->CreateRasterizerState(&rs, &raster_);
        return true;
    }
    ~Renderer()
    {
        for (auto& t : tex_) { release(t.srv); release(t.tex); }
        release(raster_); release(cb_); release(sampler_); release(ps_); release(vs_); release(rtv_); release(swap_); release(ctx_); release(dev_);
    }
    const std::string& gpuName() const { return gpuName_; }
    void resize(int w, int h)
    {
        if (w == width_ && h == height_) return;
        release(rtv_);
        swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        width_ = w; height_ = h;
        makeTarget();
    }
    // upload a decoded frame (creates textures on the first frame / a format change)
    bool upload(const DecodedFrame& f, int w, int h)
    {
        for (int t = 0; t < f.count; ++t) {
            DXGI_FORMAT fmt = dxgiFormat(f.fmt[t]);
            if (fmt == DXGI_FORMAT_UNKNOWN) return false;
            int tw = (w + 3) & ~3, th = (h + 3) & ~3;
            Tex& x = tex_[t];
            if (!x.tex || x.fmt != fmt || x.w != tw || x.h != th) {
                release(x.srv); release(x.tex);
                D3D11_TEXTURE2D_DESC td{};
                td.Width = tw; td.Height = th; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
                td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(dev_->CreateTexture2D(&td, nullptr, &x.tex))) { logf("ppplayer: the graphics card can't make a %ux%u texture of this format", tw, th); return false; }
                dev_->CreateShaderResourceView(x.tex, nullptr, &x.srv);
                x.fmt = fmt; x.w = tw; x.h = th;
            }
            UINT pitch = UINT(tw / 4) * ((fmt == DXGI_FORMAT_BC1_UNORM || fmt == DXGI_FORMAT_BC4_UNORM) ? 8 : 16);
            ctx_->UpdateSubresource(x.tex, 0, nullptr, f.tex[t].data(), pitch, 0);
        }
        count_ = f.count;
        mode_ = shaderMode(f);
        srcW_ = w; srcH_ = h;
        return true;
    }
    // draw into the whole target, or one part of it (canvas spans several displays: one window)
    void draw(const std::string& scale)
    {
        float clear[4] = { 0, 0, 0, 1 };
        ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
        ctx_->ClearRenderTargetView(rtv_, clear);
        if (!tex_[0].srv) return;
        D3D11_VIEWPORT vp{ 0, 0, (float)width_, (float)height_, 0, 1 };
        ctx_->RSSetViewports(1, &vp);
        // placement in NDC
        double sw = srcW_, sh = srcH_, dw = width_, dh = height_;
        double x0 = -1, y0 = -1, x1 = 1, y1 = 1;
        if (scale != "stretch") {
            double s = scale == "native" ? 1.0 : scale == "fill" ? std::max(dw / sw, dh / sh) : std::min(dw / sw, dh / sh);
            double pw = sw * s / dw, ph = sh * s / dh;   // fraction of target
            x0 = -pw; x1 = pw; y0 = -ph; y1 = ph;
        }
        struct { float rect[4]; float uv[4]; int mode; int pad[3]; } c{};
        c.rect[0] = (float)x0; c.rect[1] = (float)y0; c.rect[2] = (float)x1; c.rect[3] = (float)y1;
        c.uv[0] = float(srcW_) / float(tex_[0].w); c.uv[1] = float(srcH_) / float(tex_[0].h);
        c.mode = mode_;
        ctx_->UpdateSubresource(cb_, 0, nullptr, &c, 0, 0);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx_->IASetInputLayout(nullptr);
        ctx_->VSSetShader(vs_, nullptr, 0);
        ctx_->VSSetConstantBuffers(0, 1, &cb_);
        ctx_->PSSetShader(ps_, nullptr, 0);
        ctx_->PSSetConstantBuffers(0, 1, &cb_);
        ID3D11ShaderResourceView* srvs[2] = { tex_[0].srv, count_ > 1 ? tex_[1].srv : tex_[0].srv };
        ctx_->PSSetShaderResources(0, 2, srvs);
        ctx_->PSSetSamplers(0, 1, &sampler_);
        ctx_->RSSetState(raster_);
        ctx_->Draw(4, 0);
    }
    void present(bool vsync) { swap_->Present(vsync ? 1 : 0, 0); }
    bool screenshot(const std::string& path)
    {
        ID3D11Texture2D* back = nullptr;
        if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
        D3D11_TEXTURE2D_DESC td; back->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ; td.MiscFlags = 0;
        ID3D11Texture2D* st = nullptr;
        dev_->CreateTexture2D(&td, nullptr, &st);
        ctx_->CopyResource(st, back);
        D3D11_MAPPED_SUBRESOURCE m;
        bool ok = false;
        if (SUCCEEDED(ctx_->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            FILE* f = _wfopen(widen(path).c_str(), L"wb");
            if (f) {
                int W = (int)td.Width, H = (int)td.Height;
                BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{};
                fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + W * H * 4;
                ih.biSize = sizeof ih; ih.biWidth = W; ih.biHeight = -H; ih.biPlanes = 1; ih.biBitCount = 32;
                fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
                std::vector<uint8_t> row(size_t(W) * 4);
                for (int y = 0; y < H; ++y) {
                    const uint8_t* s = (const uint8_t*)m.pData + size_t(y) * m.RowPitch;
                    for (int x = 0; x < W; ++x) { row[x * 4] = s[x * 4 + 2]; row[x * 4 + 1] = s[x * 4 + 1]; row[x * 4 + 2] = s[x * 4]; row[x * 4 + 3] = 255; }
                    fwrite(row.data(), 1, row.size(), f);
                }
                fclose(f); ok = true;
            }
            ctx_->Unmap(st, 0);
        }
        release(st); release(back);
        return ok;
    }

private:
    struct Tex { ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN; int w = 0, h = 0; };
    bool makeTarget()
    {
        ID3D11Texture2D* back = nullptr;
        if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) return false;
        HRESULT hr = dev_->CreateRenderTargetView(back, nullptr, &rtv_);
        release(back);
        return SUCCEEDED(hr);
    }
    static DXGI_FORMAT dxgiFormat(unsigned hf)
    {
        switch (hf) {
        case HapTextureFormat_RGB_DXT1: return DXGI_FORMAT_BC1_UNORM;
        case HapTextureFormat_RGBA_DXT5: case HapTextureFormat_YCoCg_DXT5: return DXGI_FORMAT_BC3_UNORM;
        case HapTextureFormat_A_RGTC1: return DXGI_FORMAT_BC4_UNORM;
        case HapTextureFormat_RGBA_BPTC_UNORM: return DXGI_FORMAT_BC7_UNORM;
        case HapTextureFormat_RGB_BPTC_UNSIGNED_FLOAT: return DXGI_FORMAT_BC6H_UF16;
        case HapTextureFormat_RGB_BPTC_SIGNED_FLOAT: return DXGI_FORMAT_BC6H_SF16;
        }
        return DXGI_FORMAT_UNKNOWN;
    }
    static int shaderMode(const DecodedFrame& f)
    {
        switch (f.fmt[0]) {
        case HapTextureFormat_RGB_DXT1: return 0;
        case HapTextureFormat_RGBA_DXT5: return 1;
        case HapTextureFormat_YCoCg_DXT5: return f.count > 1 ? 3 : 2;
        case HapTextureFormat_A_RGTC1: return 4;
        case HapTextureFormat_RGBA_BPTC_UNORM: return 1;
        default: return 5;
        }
    }
    HWND hwnd_ = nullptr;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain* swap_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11Buffer* cb_ = nullptr;
    ID3D11RasterizerState* raster_ = nullptr;
    Tex tex_[2];
    int count_ = 0, mode_ = 0, srcW_ = 1, srcH_ = 1;
    int width_ = 0, height_ = 0;
    std::string gpuName_;
};

// ------------------------------------------------------------------------------------------------
// window + input

static std::atomic<bool> g_quit{ false };
static std::mutex g_keyMutex;
static std::deque<WPARAM> g_keys;
static std::atomic<int> g_resizeW{ 0 }, g_resizeH{ 0 };

static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLOSE: g_quit = true; return 0;
    case WM_DESTROY: g_quit = true; PostQuitMessage(0); return 0;
    case WM_KEYDOWN: { std::lock_guard<std::mutex> lk(g_keyMutex); g_keys.push_back(wp); return 0; }
    case WM_SIZE: g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); return 0;
    case WM_SETCURSOR: if (LOWORD(lp) == HTCLIENT) { SetCursor(nullptr); return TRUE; } break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND makeWindow(const RECT& r, bool windowed)
{
    WNDCLASSEXW wc{ sizeof wc };
    wc.lpfnWndProc = wndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"PixelPressPlayer";
    RegisterClassExW(&wc);
    DWORD style = windowed ? WS_OVERLAPPEDWINDOW : WS_POPUP;
    RECT wr = r;
    if (windowed) AdjustWindowRect(&wr, style, FALSE);
    HWND h = CreateWindowExW(windowed ? 0 : WS_EX_TOPMOST, wc.lpszClassName, L"PixelPress Player", style,
                             wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOW);
    return h;
}

static void pumpMessages()
{
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}

// ------------------------------------------------------------------------------------------------
// playback of one clip until it ends / the user switches

enum class Next { Quit, NextClip, PrevClip, Restart, Jump };

struct Session {
    Options& o;
    Renderer& r;
    AudioOut* audio;        // nullptr = no audio device
    int threads;
    int jumpTo = -1;
    int clipIndex = 0;
    SyncLeader* leader = nullptr;
    SyncFollower* follower = nullptr;
};

static Next playClip(Session& s, const Clip& clip, bool loopClip, bool& loopModeChanged)
{
    const int64_t n = (int64_t)clip.frames.size();
    const int ahead = std::clamp(int(1.0e9 / std::max<size_t>(1, texBytes(HapTextureFormat_RGBA_BPTC_UNORM, clip.width, clip.height) * 2)), 4, 16);
    FrameQueue q(clip, s.threads, ahead);
    if (s.follower) { auto tg = s.follower->target(); if (tg.valid) s.o.startAt = std::max(0.0, tg.mediaTime); }
    int64_t startIndex = (int64_t)llround(s.o.startAt * clip.fps) % n;
    s.o.startAt = 0;
    q.setPosition(startIndex, loopClip);

    std::unique_ptr<AudioSource> asrc;
    const bool follower = s.follower != nullptr;
    if (s.audio && clip.audioStream >= 0 && (!follower || s.o.syncAudio)) {
        try { asrc = std::make_unique<AudioSource>(clip, s.audio->rate(), s.audio->channels(), s.audio->channelMask(), loopClip, startIndex / clip.fps); }
        catch (const std::exception& e) { logf("ppplayer: audio not played: %s", e.what()); }
    }
    // wait for the first frame so playback starts cleanly
    auto first = q.waitFor(startIndex, 5.0);
    if (first) s.r.upload(*first, clip.width, clip.height);

    double clock0 = nowSeconds(), pausedAt = 0, pausedTotal = 0;
    bool paused = false;
    if (asrc) s.audio->start([&](float* d, int f) { return asrc->fill(d, f); });
    const double startSec = startIndex / clip.fps;
    int64_t shown = first ? startIndex : -1, lastPos = startIndex;
    int64_t presented = 0, dropped = 0, repeats = 0;
    double statT = nowSeconds();
    int64_t statDecoded = 0;
    Next result = Next::Quit;
    bool done = false;

    while (!done) {
        pumpMessages();
        if (g_quit) { result = Next::Quit; break; }
        if (int w = g_resizeW.exchange(0)) s.r.resize(w, g_resizeH.load());
        // keys
        for (;;) {
            WPARAM k;
            { std::lock_guard<std::mutex> lk(g_keyMutex); if (g_keys.empty()) break; k = g_keys.front(); g_keys.pop_front(); }
            if (k == VK_ESCAPE) { result = Next::Quit; done = true; }
            else if (follower) {}   // a follower does what the leader does
            else if (k == VK_RIGHT || k == 'N') { result = Next::NextClip; done = true; }
            else if (k == VK_LEFT || k == 'P') { result = Next::PrevClip; done = true; }
            else if (k == VK_HOME) { result = Next::Restart; done = true; }
            else if (k >= '1' && k <= '9') { s.jumpTo = int(k - '1'); result = Next::Jump; done = true; }
            else if (k == 'L') { loopModeChanged = true; }
            else if (k == VK_SPACE) {
                paused = !paused;
                if (paused) pausedAt = nowSeconds(); else pausedTotal += nowSeconds() - pausedAt;
                if (asrc) s.audio->pause(paused);
            }
        }
        if (done) break;

        // media time: the leader's (follower), else the audio actually playing, else the system clock
        double t;
        bool loopNow = loopClip;
        if (follower) {
            auto tg = s.follower->target();
            if (tg.valid && tg.clip != s.clipIndex) { s.jumpTo = tg.clip; result = Next::Jump; break; }
            t = tg.valid ? tg.mediaTime : lastPos / clip.fps;
            if (tg.valid) { paused = tg.paused; loopNow = tg.looping; }
            static bool lostNoted = false;
            if (tg.valid && tg.age > 2.0 && !lostNoted) { logf("ppplayer: sync: lost the leader - holding position"); lostNoted = true; }
            if (tg.valid && tg.age < 1.0 && lostNoted) { logf("ppplayer: sync: leader back"); lostNoted = false; }
            if (tg.valid && tg.age > 2.0) { paused = true; t = lastPos / clip.fps; }
            if (asrc) {   // -sync-audio: keep this PC's sound within 40 ms of the leader
                double at = startSec + s.audio->position();
                if (std::fabs(at - t) > 0.04 && nowSeconds() - clock0 > 1.0) { asrc->seek(t + 0.1); }
            }
        } else if (asrc) t = startSec + s.audio->position();
        else t = startSec + ((paused ? pausedAt : nowSeconds()) - clock0 - pausedTotal);
        int64_t pos = (int64_t)std::floor(t * clip.fps + 1e-6);
        if (follower) {
            if (pos < 0) pos = 0;
            if (!loopNow && pos >= n) pos = n - 1;   // hold the last frame until the leader moves on
        } else {
            if (!loopClip && pos >= n) { result = Next::NextClip; break; }
            if (pos < lastPos) pos = lastPos;    // audio clock jitter: never step back
        }
        lastPos = pos;
        if (s.leader) s.leader->publish(s.clipIndex, t, paused, loopClip);
        q.setPosition(pos, loopNow);
        auto f = q.get(pos);
        if (f && f->index != shown) {
            if (shown >= 0 && f->index > shown + 1) dropped += f->index - shown - 1;
            s.r.upload(*f, clip.width, clip.height);
            shown = f->index;
        } else if (f) repeats++;
        s.r.draw(s.o.scale);
        presented++;
        if (s.o.testFrames && presented >= s.o.testFrames) {   // testing: capture before present (flip discards)
            if (!s.o.screenshot.empty()) s.r.screenshot(s.o.screenshot);
            result = Next::Quit; break;
        }
        s.r.present(true);
        if (s.o.stats && nowSeconds() - statT >= 1.0) {
            double dt = nowSeconds() - statT;
            int64_t dec = q.decoded.load();
            char syncInfo[96] = "";
            if (follower) snprintf(syncInfo, sizeof syncInfo, "  sync: clock %+.2f ms, round trip %.2f ms", s.follower->clockOffsetMs(), s.follower->rttMs());
            logf("ppplayer: %s  frame %lld/%lld  t=%.2fs  decode %.1f fps  shown %lld  dropped %lld%s%s",
                 clip.path.substr(clip.path.find_last_of("\\/") + 1).c_str(), (long long)(shown % n + 1), (long long)n, t,
                 (dec - statDecoded) / dt, (long long)presented, (long long)dropped, paused ? "  (paused)" : "", syncInfo);
            statDecoded = dec; statT = nowSeconds();
        }
    }
    if (asrc) s.audio->stop();
    if (dropped) logf("ppplayer: %lld frame(s) were not ready in time (decoding or the disk too slow)", (long long)dropped);
    return result;
}

// ------------------------------------------------------------------------------------------------

int wmain(int argc, wchar_t** argv)
{
    SetConsoleOutputCP(CP_UTF8);
    Options o = parseArgs(argc, argv);
    hapgd::registerCodec();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetProcessDPIAware();
    timeBeginPeriod(1);

    auto displays = listDisplays();
    if (o.listDisplays) {
        for (size_t i = 0; i < displays.size(); ++i) {
            const RECT& r = displays[i].rect;
            printf("%zu: %s  %ldx%ld at %ld,%ld%s\n", i + 1, displays[i].name.c_str(), r.right - r.left, r.bottom - r.top, r.left, r.top,
                   displays[i].primary ? "  (primary)" : "");
        }
        return 0;
    }
    if (o.files.empty()) { usage(); return 1; }
    av_log_set_level(AV_LOG_ERROR);
    if (!o.testAudio.empty()) {   // testing without a sound card: the audio path into a file
        auto clip = openClip(o.files[0]);
        if (clip->audioStream < 0) die("no audio track");
        AudioSource src(*clip, 48000, 2, 3, true, o.startAt);
        int64_t total = (int64_t)llround(clip->duration() * 48000 * 2);
        std::vector<float> buf(4800 * 2);
        FILE* f = _wfopen(widen(o.testAudio).c_str(), L"wb");
        for (int64_t done = 0; done < total; ) {
            int n = src.fill(buf.data(), 4800);
            fwrite(buf.data(), 8, n, f); done += n;
        }
        fclose(f);
        return 0;
    }

    if (o.bench) {   // decode speed: every frame read from disk and unpacked, nothing shown
        int threads = o.threads > 0 ? o.threads : (int)std::max(2u, std::thread::hardware_concurrency());
        for (auto& path : o.files) {
            std::unique_ptr<Clip> clip;
            try { clip = openClip(path); } catch (const std::exception& e) { logf("ppplayer: %s", e.what()); continue; }
            std::atomic<int64_t> next{ 0 }, bytes{ 0 }, bad{ 0 };
            const double t0 = nowSeconds();
            const int64_t n = (int64_t)clip->frames.size();
            const int64_t total = std::max<int64_t>(n, (int64_t)(clip->fps * 10));   // at least ~10 s of video
            std::vector<std::thread> pool;
            for (int i = 0; i < threads; ++i) pool.emplace_back([&] {
                FileReader rd(clip->path); std::vector<uint8_t> pkt; DecodedFrame f;
                for (int64_t k; (k = next++) < total; ) {
                    if (!decodeFrame(*clip, rd, pkt, k, f)) bad++;
                    bytes += (int64_t)pkt.size();
                }
            });
            for (auto& t : pool) t.join();
            double dt = nowSeconds() - t0;
            double fps = total / dt;
            logf("ppplayer: bench: %s  %dx%d %s  %.1f frames/s with %d threads = %.1fx real time (%.0f MB/s from disk)%s",
                 path.substr(path.find_last_of("\\/") + 1).c_str(), clip->width, clip->height, clip->codecName().c_str(), fps, threads,
                 fps / clip->fps, bytes / dt / 1e6, bad ? "  DECODE ERRORS" : "");
        }
        return 0;
    }

    std::unique_ptr<SyncLeader> leader;
    std::unique_ptr<SyncFollower> follower;
    if (o.sync == "leader") leader = std::make_unique<SyncLeader>(o.syncPort, o.syncTarget);
    if (o.sync == "follow") follower = std::make_unique<SyncFollower>(o.syncPort, o.syncOffsetMs);

    RECT rect{ 100, 100, 1380, 820 };
    if (!o.windowed) {
        if (displays.empty()) die("no displays found");
        if (o.span) {
            rect = displays[0].rect;
            for (auto& d : displays) UnionRect(&rect, &rect, &d.rect);
        } else {
            if (o.display < 1 || o.display > (int)displays.size()) die("there is no display %d (%zu found; -list-displays)", o.display, displays.size());
            rect = displays[o.display - 1].rect;
        }
    }
    HWND hwnd = makeWindow(rect, o.windowed);
    RECT cr; GetClientRect(hwnd, &cr);
    Renderer r;
    if (!r.init(hwnd, std::max<LONG>(1, cr.right), std::max<LONG>(1, cr.bottom))) die("the graphics card could not be used (Direct3D 11)");

    std::unique_ptr<AudioOut> audio;
    if (o.audio) {
        audio = std::make_unique<AudioOut>();
        if (!audio->init(o.volume)) { logf("ppplayer: no audio device - playing without sound"); audio.reset(); }
    }
    int threads = o.threads > 0 ? o.threads : (int)std::min(16u, std::max(2u, std::thread::hardware_concurrency()));
    logf("ppplayer " PPPLAYER_VERSION ": %s, %d decode threads, %s%s", r.gpuName().c_str(), threads,
         audio ? "audio on" : "no audio", o.windowed ? ", windowed" : o.span ? ", spanning all displays" : "");

    Session s{ o, r, audio.get(), threads };
    s.leader = leader.get(); s.follower = follower.get();
    int idx = 0;
    const int count = (int)o.files.size();
    if (follower) {   // black until the leader is heard
        logf("ppplayer: sync: waiting for the leader ...");
        double lastNote = nowSeconds();
        while (!g_quit) {
            pumpMessages();
            { std::lock_guard<std::mutex> lk(g_keyMutex); for (auto k : g_keys) if (k == VK_ESCAPE) g_quit = true; g_keys.clear(); }
            auto tg = follower->target();
            if (tg.valid) { idx = std::clamp(tg.clip, 0, count - 1); break; }
            r.draw(o.scale); r.present(true);
            if (nowSeconds() - lastNote > 10) { logf("ppplayer: sync: still no leader on UDP %d (same network? firewall?)", o.syncPort); lastNote = nowSeconds(); }
        }
    }
    while (!g_quit) {
        s.clipIndex = idx;
        std::unique_ptr<Clip> clip;
        try { clip = openClip(o.files[idx]); }
        catch (const std::exception& e) {
            logf("ppplayer: %s", e.what());
            if (count == 1) return 1;
            idx = (idx + 1) % count;
            continue;
        }
        logf("ppplayer: [%d/%d] %s  %dx%d %.3f fps  %s  %zu frames%s", idx + 1, count, clip->path.c_str(), clip->width, clip->height,
             clip->fps, clip->codecName().c_str(), clip->frames.size(), clip->audioStream >= 0 ? "  + audio" : "");
        bool loopModeChanged = false;
        Next nx = playClip(s, *clip, o.loop == "clip", loopModeChanged);
        if (loopModeChanged) {
            o.loop = o.loop == "clip" ? (count > 1 ? "playlist" : "none") : o.loop == "playlist" ? "none" : "clip";
            logf("ppplayer: loop mode: %s", o.loop.c_str());
        }
        if (nx == Next::Quit) break;
        if (nx == Next::NextClip) {
            if (idx + 1 >= count && o.loop == "none") break;
            idx = (idx + 1) % count;
        } else if (nx == Next::PrevClip) idx = (idx + count - 1) % count;
        else if (nx == Next::Jump) {
            if (s.jumpTo >= 0 && s.jumpTo < count) idx = s.jumpTo;
            else if (follower) { logf("ppplayer: sync: the leader plays clip %d, this playlist has %d", s.jumpTo + 1, count); Sleep(1000); }
        }
        // Restart: same idx
    }
    DestroyWindow(hwnd);
    timeEndPeriod(1);
    return 0;
}
