// pphap - PixelPress HAP command-line encoder / Hap R decoder
//
// Same encoder as the PixelPress HAP Adobe plugin (Hap, Hap Alpha, Hap Q, Hap Q Alpha,
// Hap Alpha-Only, Hap R), with ffmpeg-style options and ffmpeg-compatible -progress output.
//
//   pphap -i input.mov -c:v hap_r output.mov
//   pphap -i input.mov -c:v hap -format hap_q -progress pipe:1 -y output.mov
//   ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - | pphap -f rawvideo -pix_fmt rgba -s 1920x1080 -r 30 -i - -c:v hap_r out.mov
//   pphap -i hap_r.mov -f rawvideo -pix_fmt rgba -            (1.1.7: decoded frames on stdout)
//
// Decoding of -i <file> is done by an ffmpeg executable (next to pphap, on PATH, or $PPHAP_FFMPEG),
// except Hap R (which ffmpeg cannot decode): pphap reads and decodes Hap R files itself (1.1.7).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#define popen _popen
#define pclose _pclose
#else
#include <sys/ioctl.h>
#include <unistd.h>
#include <limits.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "codec_registration.hpp"
#include "movie_writer.hpp"
#include "movie_reader.hpp"
#include "texture_hq.hpp"
#include "gpu_bc7.hpp"
#include "gdeflate_hap.hpp"
#include "texture_converter.hpp"
#include "hap.h"
#include "bc7decomp.h"
extern "C" {
#include <libavutil/log.h>
}

#ifndef PPHAP_VERSION
#define PPHAP_VERSION "1.2.1"
#endif

namespace {

// ---------------------------------------------------------------------------------------------
// small helpers

// 1.1.9 Windows: every string inside pphap is UTF-8 (the arguments are converted from the UTF-16
// command line in main). These helpers turn them back into UTF-16 for the Windows file / process
// APIs, so paths with any characters work (the ANSI code page used to mangle e.g. "é" for Hap R
// input and anything outside the code page everywhere).
#ifdef _WIN32
std::wstring widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}
std::string narrow(const wchar_t* w)
{
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s(size_t(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
#endif

FILE* fopenUtf8(const std::string& path, const char* mode)
{
#ifdef _WIN32
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

[[noreturn]] void die(const std::string& msg)
{
    fprintf(stderr, "pphap: %s\n", msg.c_str());
    exit(1);
}

bool fileExists(const std::string& p)
{
    FILE* f = fopenUtf8(p, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

// full path of this executable ("" if unknown)
std::string exePath()
{
#if defined(_WIN32)
    static wchar_t wbuf[32768];
    GetModuleFileNameW(nullptr, wbuf, 32768);
    return narrow(wbuf);
#elif defined(__APPLE__)
    char buf[4096] = { 0 };
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return "";
    return buf;
#else
    char buf[4096] = { 0 };
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    return n > 0 ? std::string(buf, n) : std::string();
#endif
}

std::string exeDir()
{
    std::string s = exePath();
    auto pos = s.find_last_of("\\/");
    return pos == std::string::npos ? std::string(".") : s.substr(0, pos);
}

std::string shellQuote(const std::string& s)
{
#ifdef _WIN32
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += "\\\""; else out += c; }
    return out + "\"";
#else
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
#endif
}

FILE* openPipe(const std::string& cmd, const char* mode)
{
#ifdef _WIN32
    // cmd.exe /c strips one outer pair of quotes
    return _wpopen(widen("\"" + cmd + "\"").c_str(), widen(mode).c_str());
#else
    // POSIX popen only accepts "r"/"w" (no "b"); pipes are binary anyway
    return popen(cmd.c_str(), mode[0] == 'w' ? "w" : "r");
#endif
}

std::string findTool(const std::string& name)
{
#ifdef _WIN32
    const wchar_t* wenv = _wgetenv(name == "ffmpeg" ? L"PPHAP_FFMPEG" : L"PPHAP_FFPROBE");
    if (wenv && *wenv) return narrow(wenv);
#else
    const char* env = getenv(name == "ffmpeg" ? "PPHAP_FFMPEG" : "PPHAP_FFPROBE");
    if (env && *env) return env;
#endif
#ifdef _WIN32
    std::string local = exeDir() + "\\" + name + ".exe";
#else
    std::string local = exeDir() + "/" + name;
#endif
    if (fileExists(local)) return local;
#ifdef __APPLE__
    for (const char* d : { "/opt/homebrew/bin/", "/usr/local/bin/" })
        if (fileExists(std::string(d) + name)) return std::string(d) + name;
#endif
    return name;  // rely on PATH
}

bool parseRational(const std::string& s, Rational& out)
{
    auto slash = s.find('/');
    try {
        if (slash != std::string::npos) {
            long long n = std::stoll(s.substr(0, slash)), d = std::stoll(s.substr(slash + 1));
            if (n <= 0 || d <= 0) return false;
            out = Rational{ n, d };
            return true;
        }
        double v = std::stod(s);
        if (!(v > 0)) return false;
        // common NTSC rates as exact rationals, otherwise millis
        const std::pair<double, Rational> known[] = {
            { 23.976, {24000, 1001} }, { 29.97, {30000, 1001} }, { 59.94, {60000, 1001} }, { 119.88, {120000, 1001} } };
        for (auto& k : known)
            if (std::fabs(v - k.first) < 0.01) { out = k.second; return true; }
        out = Rational{ (long long)std::llround(v * 1000), 1000 };
        return true;
    } catch (...) { return false; }
}

double toDouble(const Rational& r) { return double(r.numerator) / double(r.denominator); }

std::string formatTime(double seconds)
{
    if (seconds < 0) seconds = 0;
    long long us = (long long)std::llround(seconds * 1e6);
    int h = int(us / 3600000000LL); us %= 3600000000LL;
    int m = int(us / 60000000LL);   us %= 60000000LL;
    char buf[64];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%06lld", h, m, int(us / 1000000), us % 1000000);
    return buf;
}

// ---------------------------------------------------------------------------------------------
// options

struct Variant { const char* name; Codec4CC cc; bool alpha; };
const Variant kVariants[] = {
    { "hap",            {'H','a','p','1'}, false },
    { "hap_alpha",      {'H','a','p','5'}, true  },
    { "hap_q",          {'H','a','p','Y'}, false },
    { "hap_q_alpha",    {'H','a','p','M'}, true  },
    { "hap_alpha_only", {'H','a','p','A'}, true  },
    { "hap_r",          {'H','a','p','7'}, false },   // RGBA texture; header depth 24 like Jokyo unless -alpha 1
};

struct Options {
    std::string input;
    std::string output;
    std::string inFormat;           // "rawvideo" for piped raw frames
    std::string pixFmt = "rgba";    // for rawvideo input
    int width = 0, height = 0;
    std::optional<Rational> rate;
    std::string codec = "hap_r";
    std::string quality = "fast";
    int chunks = 0;                 // 0 = auto
    int threads = 0;
    long long maxFrames = -1;
    std::string progressUrl;
    double statsPeriod = 0.5;
    bool stats = true;
    bool overwrite = false;
    bool noOverwrite = false;
    int alpha = -1;                 // -1 auto
    std::string vf;
    std::string ss, to, t;          // 1.1.5: in / out points (seconds or [HH:]MM:SS[.ms])
    // 1.1.7: audio - like ffmpeg, the source's first audio track is included unless -an
    bool noAudio = false;
    int audioBytes = 2;             // 2 = pcm_s16le (default), 4 = pcm_s32le
    // 1.1.7: decoded frames out instead of a Hap file
    bool rawOut = false;
    std::string outPixFmt = "rgba";
    bool quiet = false;
    // 1.1 colour accuracy
    std::string colorConv = "accurate";   // accurate | legacy (1.0: ffmpeg's default conversion)
    std::string colorspace = "auto";      // override the source's YUV matrix
    std::string colorRange = "auto";      // override the source's range: tv | pc
    std::string engine = "accurate";      // accurate | legacy (1.0 texture encoders)
    std::string gpu;                      // 1.2.0: auto | on | off (Hap R, Fast); "" = PPHAP_GPU or auto
    std::string compressor = "snappy";  // 1.2.2: snappy (standard Hap) | gdeflate (PixelPress players only)
    int gdLevel = 6;
    bool psnr = false;                    // decode the textures again and report accuracy
};

void usage()
{
    printf(
"pphap %s - PixelPress HAP encoder (Hap / Hap Alpha / Hap Q / Hap Q Alpha / Hap Alpha-Only / Hap R)\n"
"\n"
"usage: pphap [options] -i INPUT [options] OUTPUT.mov\n"
"       pphap [options] -i INPUT [options] -f rawvideo -pix_fmt rgba -    (decoded frames on stdout)\n"
"\n"
"Input\n"
"  -i FILE              any video ffmpeg can decode (ffmpeg next to pphap, on PATH, or $PPHAP_FFMPEG),\n"
"                       and Hap R, which pphap decodes itself (ffmpeg can't)\n"
"  -i -  | -i pipe:0    raw frames on stdin; needs -f rawvideo -pix_fmt -s -r\n"
"  -f rawvideo          input is raw video\n"
"  -pix_fmt FMT         raw input pixel format: rgba (default), bgra, argb, rgb24\n"
"  -s WxH               raw input frame size\n"
"  -r RATE              frame rate (e.g. 30, 29.97, 30000/1001); for -i FILE overrides the probed rate\n"
"                       (Hap R input: relabels the timing, every frame is kept)\n"
"  -vf FILTER           ffmpeg video filter applied while decoding -i FILE (e.g. scale=1920:1080)\n"
"                       also accepts -s W:H, -ss, -to and -pix_fmt inside the filter string, e.g.\n"
"                       -vf \"-s 1920:1080 -ss 117.284 -to 121.822,format=rgba\"\n"
"  -ss TIME             start (IN point) in the source: seconds or [HH:]MM:SS[.ms]\n"
"  -to TIME             end (OUT point) in the source, same units as -ss\n"
"  -t DURATION          length to encode from -ss (instead of -to)\n"
"  -frames:v N          stop after N frames\n"
"\n"
"Colour\n"
"  -colorconv MODE      accurate (default): YUV->RGB with the source's own matrix and range,\n"
"                       full-precision rounding and chroma; legacy: ffmpeg defaults, as pphap 1.0\n"
"  -colorspace M        override a missing / wrong source tag: bt709 | bt601 | bt2020 | smpte240m\n"
"                       (untagged sources: bt709 when 1280 wide / 720 high or larger, else bt601)\n"
"  -color_range R       override the source range: tv (limited) | pc (full)\n"
"\n"
"Encoding\n"
"  -c:v CODEC           hap | hap_alpha | hap_q | hap_q_alpha | hap_alpha_only | hap_r (default hap_r)\n"
"  -format FMT          ffmpeg style: -c:v hap -format hap_q (same names as above)\n"
"  -quality Q           fast | normal | high (default fast; every format)\n"
"  -chunks N            1-64 texture chunks per frame (default 8)\n"
"  -alpha 0|1           write 32-bit (alpha) header depth; default: on for alpha variants, off for Hap R\n"
"  -threads N           encoder threads (default: all cores)\n"
"  -engine E            accurate (default) | legacy: the pphap 1.0 texture encoders\n"
"  -gpu off|auto|on     experimental Vulkan GPU encoder for Hap R at -quality fast. off (default):\n"
"                       CPU only; auto: times GPU and CPU before the first frame and keeps the\n"
"                       faster; on: always the GPU. Also: PPHAP_GPU=off|auto|on\n"
"  -compressor snappy|gdeflate  second-stage compression. snappy (default): standard Hap, plays\n"
"                       everywhere. gdeflate: ~15%% smaller files that the GPU can unpack, but NOT\n"
"                       standard Hap - only PixelPress players (and pphap) can read them\n"
"  -gdeflate_level N    1-12 (default 6); above 8 is 4-8x slower to encode for ~1%% smaller\n"
"  -legacy              exactly pphap 1.0 output: -engine legacy -colorconv legacy\n"
"  -an                  no audio. Otherwise, like ffmpeg, the source's first audio track is\n"
"                       included (uncompressed PCM, cut to the same range as the video)\n"
"  -c:a pcm_s16le|pcm_s32le   audio sample format (default pcm_s16le)\n"
"  -psnr                decode every encoded frame again and print its accuracy against the\n"
"                       frame that was encoded (PSNR, CIEDE2000 colour difference)\n"
"\n"
"Decoded output (instead of a Hap .mov)\n"
"  -f rawvideo          after -i: write decoded frames, packed, no header; OUTPUT - = stdout\n"
"  -pix_fmt FMT         after -i: rgba (default), bgra, argb, rgb24\n"
"  OUTPUT -             same as -f rawvideo; frames go to stdout (use -progress pipe:2 or a file)\n"
"                       e.g. one frame:  pphap -ss 12.5 -i clip.mov -frames:v 1 -f rawvideo -\n"
"                       to ProRes:  pphap -i hapr.mov -f rawvideo - | ffmpeg -f rawvideo -pix_fmt rgba\n"
"                                   -s WxH -r RATE -i - -c:v prores_ks out.mov\n"
"\n"
"Output / progress\n"
"  -y | -n              overwrite / never overwrite OUTPUT\n"
"  -progress URL        ffmpeg-style key=value progress to pipe:1 (stdout), pipe:2 (stderr) or a file\n"
"  -stats_period SEC    progress/stats update interval (default 0.5)\n"
"  -nostats             no status line on stderr\n"
"  -loglevel quiet      no messages except errors\n"
"  -h, -version\n"
"\n"
"Progress keys: frame fps total_frames percent out_time_us out_time_ms out_time total_size\n"
"               bitrate speed progress=continue|end   (the same keys ffmpeg -progress prints, plus\n"
"               total_frames and percent when the frame count is known)\n",
        PPHAP_VERSION);
}

// 1.1.5: "117.284", "1:57.284", "00:01:57.284" -> seconds (-1 if malformed)
double parseTime(const std::string& v)
{
    if (v.empty()) return -1;
    double total = 0; size_t start = 0; int parts = 0;
    while (true) {
        size_t c = v.find(':', start);
        std::string part = v.substr(start, c == std::string::npos ? std::string::npos : c - start);
        if (part.empty()) return -1;
        char* end = nullptr; double x = strtod(part.c_str(), &end);
        if (!end || *end != '\0' || x < 0) return -1;
        total = total * 60 + x;
        if (++parts > 3) return -1;
        if (c == std::string::npos) break;
        start = c + 1;
    }
    return total;
}

static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// 1.1.5: Smart Preso's CLI passes ffmpeg-style options inside the -vf string, e.g.
//   -vf "-s 1920:1080 -ss 117.284 -to 121.822,format=rgba"
// Options found there are moved to where they belong (-s -> scale filter, -ss / -to / -t ->
// in / out points, -pix_fmt ignored); RGB "format=" filters are dropped because pphap always
// does its own accurate YUV->RGB conversion after the filters. Everything else is kept.
void normalizeFilterOptions(Options& o)
{
    if (o.vf.find('-') == std::string::npos) return;
    std::vector<std::string> keep;
    size_t start = 0;
    while (start <= o.vf.size()) {
        size_t c = o.vf.find(',', start);
        std::string item = trim(o.vf.substr(start, c == std::string::npos ? std::string::npos : c - start));
        start = (c == std::string::npos) ? o.vf.size() + 1 : c + 1;
        if (item.empty()) continue;
        if (item[0] != '-') {
            if (item.rfind("format=", 0) == 0) {
                const std::string f = item.substr(7);
                if (f == "rgba" || f == "bgra" || f == "argb" || f == "abgr" || f == "rgb24" || f == "bgr24") continue;
            }
            keep.push_back(item);
            continue;
        }
        // whitespace-separated "-option value" pairs
        std::vector<std::string> tok;
        size_t p = 0;
        while (p < item.size()) {
            size_t q = item.find_first_of(" \t", p);
            std::string w = item.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (!w.empty()) tok.push_back(w);
            if (q == std::string::npos) break;
            p = q + 1;
        }
        for (size_t k = 0; k < tok.size(); ++k) {
            const std::string& opt = tok[k];
            if (k + 1 >= tok.size()) die("missing value for " + opt + " in -vf");
            const std::string val = tok[++k];
            if (opt == "-s" || opt == "-video_size") {
                std::string wh = val; auto x = wh.find_first_of("x:");
                if (x == std::string::npos) die("bad -s value in -vf: " + val + " (expected W:H or WxH)");
                keep.push_back("scale=" + wh.substr(0, x) + ":" + wh.substr(x + 1));
            }
            else if (opt == "-ss") { if (o.ss.empty()) o.ss = val; }
            else if (opt == "-to") { if (o.to.empty()) o.to = val; }
            else if (opt == "-t") { if (o.t.empty()) o.t = val; }
            else if (opt == "-pix_fmt") { /* pphap always decodes to RGB itself */ }
            else die("unsupported option " + opt + " inside -vf");
        }
    }
    std::string vf;
    for (const auto& k : keep) vf += (vf.empty() ? "" : ",") + k;
    o.vf = vf;
}

Options parse(int argc, char** argv)
{
    Options o;
    auto need = [&](int& i) -> std::string {
        if (i + 1 >= argc) die(std::string("missing value for ") + argv[i]);
        return argv[++i];
    };
    // ffmpeg rule: -f / -pix_fmt before -i describe the input, after -i the output (1.1.7)
    bool seenInput = false;
    std::string fAfter, pixAfter, codecRaw;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-h" || a == "-help" || a == "--help") { usage(); exit(0); }
        else if (a == "-version" || a == "--version") { printf("pphap %s\n", PPHAP_VERSION); exit(0); }
        else if (a == "-i") { o.input = need(i); seenInput = true; }
        else if (a == "-f") { if (seenInput) fAfter = need(i); else o.inFormat = need(i); }
        else if (a == "-pix_fmt") { if (seenInput) pixAfter = need(i); else o.pixFmt = need(i); }
        else if (a == "-s" || a == "-video_size") {
            std::string v = need(i); auto x = v.find('x');
            if (x == std::string::npos) die("bad -s value, expected WxH");
            o.width = atoi(v.substr(0, x).c_str()); o.height = atoi(v.substr(x + 1).c_str());
        }
        else if (a == "-r" || a == "-framerate") { Rational r{}; if (!parseRational(need(i), r)) die("bad -r value"); o.rate = r; }
        else if (a == "-vf" || a == "-filter:v") o.vf = need(i);
        else if (a == "-ss") o.ss = need(i);
        else if (a == "-to") o.to = need(i);
        else if (a == "-t") o.t = need(i);
        else if (a == "-frames:v" || a == "-vframes" || a == "-frames") o.maxFrames = atoll(need(i).c_str());
        else if (a == "-c:v" || a == "-vcodec" || a == "-codec:v" || a == "-c") {
            std::string c = need(i);
            if (c == "rawvideo") codecRaw = c;
            else if (c != "hap") o.codec = c; else if (o.codec == "hap_r") o.codec = "hap";
        }
        else if (a == "-format") o.codec = need(i);
        else if (a == "-quality" || a == "-q:v") o.quality = need(i);
        else if (a == "-chunks" || a == "-chunks:v") o.chunks = atoi(need(i).c_str());
        else if (a == "-alpha") o.alpha = atoi(need(i).c_str());
        else if (a == "-threads") o.threads = atoi(need(i).c_str());
        else if (a == "-engine") o.engine = need(i);
        else if (a == "-gpu") o.gpu = need(i);
        else if (a == "-compressor") o.compressor = need(i);
        else if (a == "-gdeflate_level") o.gdLevel = atoi(need(i).c_str());
        else if (a == "-colorconv") o.colorConv = need(i);
        else if (a == "-colorspace" || a == "-color_matrix") o.colorspace = need(i);
        else if (a == "-color_range") o.colorRange = need(i);
        else if (a == "-legacy") { o.engine = "legacy"; o.colorConv = "legacy"; }
        else if (a == "-psnr") o.psnr = true;
        else if (a == "-progress") o.progressUrl = need(i);
        else if (a == "-stats_period") o.statsPeriod = std::max(0.05, atof(need(i).c_str()));
        else if (a == "-nostats") o.stats = false;
        else if (a == "-stats") o.stats = true;
        else if (a == "-y") o.overwrite = true;
        else if (a == "-n") o.noOverwrite = true;
        else if (a == "-loglevel" || a == "-v") { std::string l = need(i); o.quiet = (l == "quiet" || l == "error" || l == "fatal" || l == "panic"); }
        else if (a == "-an") o.noAudio = true;
        else if (a == "-c:a" || a == "-acodec" || a == "-codec:a") {
            std::string c = need(i);
            if (c == "pcm_s32le" || c == "pcm_s24le") o.audioBytes = 4;      // 24-bit sources are kept as 32-bit
            else if (c == "pcm_s16le" || c == "copy" || c == "pcm") o.audioBytes = 2;
            else fprintf(stderr, "pphap: note: audio is written as uncompressed PCM (pcm_s16le), not %s\n", c.c_str());
        }
        else if (a == "-map") { need(i); /* video and the first audio track are always the ones used */ }
        else if (a == "-hide_banner" || a == "-nostdin") { /* accepted for ffmpeg compatibility */ }
        else if (!a.empty() && a[0] == '-' && a != "-") die("unknown option " + a + " (see pphap -h)");
        else {
            if (!o.output.empty()) die("more than one output given: " + o.output + ", " + a);
            o.output = a;
        }
    }
    normalizeFilterOptions(o);
    {
        const bool inPipe = o.input == "-" || o.input == "pipe:" || o.input == "pipe:0";
        const bool outPipe = o.output == "-" || o.output == "pipe:" || o.output == "pipe:1";
        // piped raw input keeps the 1.1.5 meaning of "-i - -f rawvideo ... out.mov" (input options after -i)
        const bool legacyPipe = inPipe && !outPipe && codecRaw.empty();
        if (!legacyPipe && (outPipe || fAfter == "rawvideo" || !codecRaw.empty())) {
            if (!fAfter.empty() && fAfter != "rawvideo") die("frames can only be written as -f rawvideo");
            o.rawOut = true;
            if (!pixAfter.empty()) o.outPixFmt = pixAfter;
            if (o.outPixFmt != "rgba" && o.outPixFmt != "bgra" && o.outPixFmt != "argb" && o.outPixFmt != "rgb24")
                die("unsupported output -pix_fmt " + o.outPixFmt + " (use rgba, bgra, argb or rgb24)");
        } else if (inPipe) {
            // pphap 1.0-1.1.5 accepted the raw-input options anywhere on the command line
            if (!fAfter.empty()) o.inFormat = fAfter;
            if (!pixAfter.empty()) o.pixFmt = pixAfter;
        } else if (!fAfter.empty() && fAfter != "mov") {
            die("output format " + fAfter + " is not supported (Hap .mov, or -f rawvideo)");
        }
    }
    if (!o.ss.empty() && parseTime(o.ss) < 0) die("bad -ss value: " + o.ss);
    if (!o.to.empty() && parseTime(o.to) < 0) die("bad -to value: " + o.to);
    if (!o.t.empty() && parseTime(o.t) <= 0) die("bad -t value: " + o.t);
    if (!o.to.empty() && !o.t.empty()) die("use -to or -t, not both");
    if (!o.to.empty() && parseTime(o.to) <= (o.ss.empty() ? 0.0 : parseTime(o.ss))) die("-to must be after -ss");
    if (o.engine != "accurate" && o.engine != "legacy") die("-engine must be accurate or legacy");
    if (o.colorConv != "accurate" && o.colorConv != "legacy") die("-colorconv must be accurate or legacy");
    {
        static const char* ok[] = { "auto", "bt709", "bt601", "bt470bg", "smpte170m", "bt2020", "bt2020nc", "bt2020c", "smpte240m", "fcc" };
        bool good = false; for (auto* k : ok) good = good || o.colorspace == k;
        if (!good) die("-colorspace must be bt709, bt601, bt2020 or smpte240m");
    }
    if (o.colorRange == "limited" || o.colorRange == "mpeg") o.colorRange = "tv";
    if (o.colorRange == "full" || o.colorRange == "jpeg") o.colorRange = "pc";
    if (o.colorRange != "auto" && o.colorRange != "tv" && o.colorRange != "pc") die("-color_range must be tv or pc");
    if (o.input.empty()) die("no input: use -i FILE or -i - (see pphap -h)");
    if (o.output.empty()) die("no output file given (or - for raw frames on stdout)");
    if (o.rawOut && (o.progressUrl == "pipe:1" || o.progressUrl == "pipe:" || o.progressUrl == "-")
        && (o.output == "-" || o.output == "pipe:" || o.output == "pipe:1"))
        die("frames go to stdout: send -progress to pipe:2 or a file");
    return o;
}

// 1.1.7: which frames "-ss IN (before -i) ... -t DUR" keeps, exactly as ffmpeg 7.1 does it (measured):
// a frame is kept when its time is >= IN and its time after IN, rounded to the nearest frame (halves
// up), is less than DUR. The ffmpeg command pphap runs gets IN and DUR with 6 decimals, so the same
// rounding is applied here. dur < 0 = no out point. Frame i is assumed to start at i / fps.
static double round6(double v) { return std::floor(v * 1e6 + 0.5) / 1e6; }
static void trimRange(double fps, double inPt, double dur, long long n, long long& first, long long& end)
{
    inPt = round6(inPt);
    first = (long long)std::ceil(inPt * fps - 1e-9);
    if (dur < 0) { end = n; return; }
    const double s = inPt * fps, d = round6(dur) * fps;
    end = first;
    while (end < n && std::floor(double(end) - s + 0.5) < d - 1e-9) ++end;
}

// ---------------------------------------------------------------------------------------------
// input sources: both deliver frames as 8-bit ARGB, top-left origin (the After Effects layout)

struct Source {
    virtual ~Source() {}
    virtual bool read(std::vector<uint8_t>& argb) = 0;   // false at end of stream
    int width = 0, height = 0;
    Rational rate{ 0, 1 };
    long long totalFrames = -1;
    double startTime = 0;           // 1.1.7: source time of the first frame delivered (for the audio)
};

bool readFully(FILE* f, uint8_t* dst, size_t n)
{
    size_t got = 0;
    while (got < n) {
        size_t r = fread(dst + got, 1, n - got, f);
        if (r == 0) return false;
        got += r;
    }
    return true;
}

struct RawPipeSource : Source {
    FILE* f;
    std::string fmt;
    std::vector<uint8_t> tmp;
    size_t bpp;
    RawPipeSource(FILE* file, const Options& o) : f(file), fmt(o.pixFmt)
    {
        width = o.width; height = o.height;
        if (width <= 0 || height <= 0) die("raw input needs -s WxH");
        if (!o.rate) die("raw input needs -r RATE");
        rate = *o.rate;
        if (fmt == "rgba" || fmt == "bgra" || fmt == "argb") bpp = 4;
        else if (fmt == "rgb24") bpp = 3;
        else die("unsupported -pix_fmt " + fmt + " (use rgba, bgra, argb or rgb24)");
        tmp.resize(size_t(width) * height * bpp);
    }
    bool read(std::vector<uint8_t>& argb) override
    {
        const size_t px = size_t(width) * height;
        argb.resize(px * 4);
        if (fmt == "argb") return readFully(f, argb.data(), argb.size());
        if (!readFully(f, tmp.data(), tmp.size())) return false;
        const uint8_t* s = tmp.data(); uint8_t* d = argb.data();
        // 1.2.1: whole 32-bit pixels instead of bytes (the compiler vectorises these loops)
        if (fmt == "rgba" || fmt == "bgra") {
            const uint32_t* s32 = reinterpret_cast<const uint32_t*>(s);
            uint32_t* d32 = reinterpret_cast<uint32_t*>(d);
            if (fmt == "rgba")   // bytes R G B A -> A R G B: rotate the little-endian word left by 8
                for (size_t i = 0; i < px; ++i) { const uint32_t v = s32[i]; d32[i] = (v << 8) | (v >> 24); }
            else                 // bytes B G R A -> A R G B: byte-reverse
                for (size_t i = 0; i < px; ++i) { const uint32_t v = s32[i]; d32[i] = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24); }
        }
        else                    for (size_t i = 0; i < px; ++i, s += 3, d += 4) { d[0] = 255;  d[1] = s[0]; d[2] = s[1]; d[3] = s[2]; }
        return true;
    }
};

// YUV -> RGB conversion for pphap 1.1. ffmpeg's defaults (pphap 1.0) take a fast path that
// drops the extra precision of 10-bit sources and, for untagged files, can pick the wrong matrix.
// Here the matrix and range come from the file (or -colorspace / -color_range), the conversion
// uses full-precision rounding and full chroma interpolation, and nothing is resized.
static bool isRgbPixFmt(const std::string& f)
{
    static const char* rgb[] = { "rgb", "bgr", "argb", "abgr", "rgba", "bgra", "gbr", "0rgb", "0bgr", "x2rgb", "x2bgr", "pal8", "gray", "ya8", "ya16", "monow", "monob" };
    for (auto* p : rgb) if (f.rfind(p, 0) == 0) return true;
    return false;
}

// 1.2.0: does this ffmpeg's scale filter take threads= (FFmpeg 5.0 and later)? Older builds would
// reject the option, so pphap only adds it when the filter's help lists it.
static bool scaleHasThreads()
{
    static int known = -1;
    if (known < 0) {
        known = 0;
        if (FILE* p = openPipe(shellQuote(findTool("ffmpeg")) + " -hide_banner -nostdin -h filter=scale", "r")) {
            char line[512];
            while (fgets(line, sizeof(line), p))
                if (strstr(line, "threads")) known = 1;
            pclose(p);
        }
    }
    return known == 1;
}

static std::string colorFilter(const Options& o, std::map<std::string, std::string>& kv, int w, int h, std::string& note)
{
    const std::string pf = kv["pix_fmt"];
    // 1.2.0: threads=0 = swscale slice threading on all cores. Output is bit-identical to one
    // thread (checked on 8/10-bit 4:2:0/4:2:2 and odd sizes); one thread capped the reader at ~60 fps
    // at 2K, which left most cores of a big workstation idle.
    const std::string flags = std::string("accurate_rnd+full_chroma_int+full_chroma_inp+neighbor") +
                              (scaleHasThreads() ? ":threads=0" : "");
    if (pf.empty()) { note = "unknown source format"; return ""; }
    if (isRgbPixFmt(pf)) {
        note = "RGB source (" + pf + "), no matrix";
        return "scale=flags=" + flags;
    }
    std::string m = o.colorspace, why = "forced";
    if (m == "auto") {
        const std::string cs = kv["color_space"];
        why = "tagged";
        if (cs == "bt709") m = "bt709";
        else if (cs == "bt470bg" || cs == "smpte170m") m = "bt601";
        else if (cs == "bt2020nc" || cs == "bt2020c") m = "bt2020";
        else if (cs == "smpte240m") m = "smpte240m";
        else if (cs == "fcc") m = "fcc";
        else { m = (w >= 1280 || h >= 720) ? "bt709" : "bt601"; why = "untagged, assumed"; }
    }
    else if (m == "bt470bg" || m == "smpte170m") m = "bt601";
    else if (m == "bt2020nc" || m == "bt2020c") m = "bt2020";
    std::string r = o.colorRange, rwhy = "forced";
    if (r == "auto") {
        const std::string cr = kv["color_range"];
        rwhy = "tagged";
        if (cr == "pc" || cr == "jpeg") r = "pc";
        else if (cr == "tv" || cr == "mpeg") r = "tv";
        else if (pf.rfind("yuvj", 0) == 0) r = "pc";
        else { r = "tv"; rwhy = "assumed"; }
    }
    note = m + " (" + why + "), " + (r == "tv" ? "limited" : "full") + " range (" + rwhy + ")";
    const std::string trc = kv["color_transfer"];
    if (trc == "smpte2084" || trc == "arib-std-b67")
        fprintf(stderr, "pphap: warning: %s is HDR (%s); it is converted as-is, without tone mapping\n",
                o.input.c_str(), trc == "smpte2084" ? "PQ" : "HLG");
    return "scale=in_color_matrix=" + m + ":in_range=" + r + ":out_range=pc:flags=" + flags;
}

// 1.2.1: reads the next frames on its own thread while the main thread hands frames to the encoder
// and writes the results, so the decoder (ffmpeg, or the Hap R decoder) never waits for us.
struct PrefetchSource : Source {
    std::unique_ptr<Source> inner;
    std::deque<std::vector<uint8_t>> queue;
    size_t depth;
    bool done = false, stopping = false;
    std::mutex m;
    std::condition_variable changed;
    std::thread reader;
    PrefetchSource(std::unique_ptr<Source> src, size_t queueDepth) : inner(std::move(src)), depth(std::max<size_t>(1, queueDepth))
    {
        width = inner->width; height = inner->height; rate = inner->rate;
        totalFrames = inner->totalFrames; startTime = inner->startTime;
        reader = std::thread([this] {
            for (;;) {
                std::vector<uint8_t> f;
                bool ok = false;
                try { ok = inner->read(f); } catch (...) { ok = false; }
                std::unique_lock<std::mutex> lock(m);
                if (!ok) { done = true; changed.notify_all(); return; }
                changed.wait(lock, [&] { return stopping || queue.size() < depth; });
                if (stopping) return;
                queue.push_back(std::move(f));
                changed.notify_all();
            }
        });
    }
    ~PrefetchSource() override
    {
        { std::lock_guard<std::mutex> lock(m); stopping = true; }
        changed.notify_all();
        // a reader inside inner->read finishes that one frame (or sees the end), then stops
        if (reader.joinable()) reader.join();
    }
    bool read(std::vector<uint8_t>& out) override
    {
        std::unique_lock<std::mutex> lock(m);
        changed.wait(lock, [&] { return !queue.empty() || done; });
        if (queue.empty()) return false;
        out = std::move(queue.front());
        queue.pop_front();
        changed.notify_all();
        return true;
    }
};

struct FfmpegSource : Source {
    FILE* pipe = nullptr;
    double trimDuration = -1;       // seconds after -ss, when an out point is set
    std::string colorNote;
    ~FfmpegSource() override { if (pipe) pclose(pipe); }

    FfmpegSource(const Options& o)
    {
        const std::string ffprobe = findTool("ffprobe"), ffmpeg = findTool("ffmpeg");
        if (!fileExists(o.input)) die("input not found: " + o.input);

        // probe size / rate / frame count
        std::string probeCmd = shellQuote(ffprobe) + " -v error -select_streams v:0 -show_entries "
            "stream=width,height,r_frame_rate,avg_frame_rate,nb_frames,duration,pix_fmt,color_space,color_range,color_transfer"
            ":format=duration -of default=nw=1 "
            + shellQuote(o.input);
        FILE* p = openPipe(probeCmd, "r");
        if (!p) die("could not run ffprobe (" + ffprobe + ")");
        std::map<std::string, std::string> kv;
        char line[512];
        while (fgets(line, sizeof(line), p)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            auto eq = l.find('=');
            if (eq == std::string::npos) continue;
            std::string k = l.substr(0, eq), v = l.substr(eq + 1);
            if (k == "duration") { if (atof(v.c_str()) > 0 && !kv.count(k)) kv[k] = v; }   // stream, then format
            else if (!kv.count(k)) kv[k] = v;
        }
        pclose(p);
        width = atoi(kv["width"].c_str()); height = atoi(kv["height"].c_str());
        if (width <= 0 || height <= 0)
            die("ffprobe could not read a video stream from " + o.input + " (is ffprobe installed? Hap R sources cannot be decoded by ffmpeg)");
        Rational r{}, avg{};
        const bool haveR = parseRational(kv["r_frame_rate"], r) && toDouble(r) > 0;
        const bool haveAvg = parseRational(kv["avg_frame_rate"], avg) && toDouble(avg) > 0;
        if (o.rate) rate = *o.rate;
        // 1.1.5: r_frame_rate can be a container clock (e.g. 1000/1 or 90000/1) for variable-rate
        // or oddly timed sources; above 240 fps, trust the measured average rate instead.
        else if (haveR && (toDouble(r) <= 240.0 || !haveAvg)) rate = r;
        else if (haveAvg) rate = avg;
        else die("could not determine the frame rate; pass -r");
        long long nb = atoll(kv["nb_frames"].c_str());
        double dur = atof(kv["duration"].c_str());
        totalFrames = nb > 0 ? nb : (dur > 0 ? (long long)std::llround(dur * toDouble(rate)) : -1);
        // 1.1.5: nb_frames counts frames an edit list hides (e.g. one pre-roll frame), so when it
        // disagrees with the duration, the duration is what ffmpeg will actually deliver
        if (nb > 0 && dur > 0 && std::fabs(double(nb) - dur * toDouble(rate)) > 0.5)
            totalFrames = (long long)std::llround(dur * toDouble(rate));

        // 1.1.5: in / out points
        const double inPt = o.ss.empty() ? 0.0 : parseTime(o.ss);
        double outPt = !o.to.empty() ? parseTime(o.to) : (!o.t.empty() ? inPt + parseTime(o.t) : -1);
        if (dur > 0 && inPt >= dur) die("-ss " + o.ss + " is past the end of " + o.input);
        if (dur > 0 && (outPt < 0 || outPt > dur)) outPt = dur;          // no / too-late out point -> end of file
        if (!o.ss.empty() || !o.to.empty() || !o.t.empty()) {
            totalFrames = outPt > inPt ? (long long)std::llround((outPt - inPt) * toDouble(rate)) : -1;
            if (!o.rate && outPt > inPt) {   // 1.1.7: the exact count ffmpeg will deliver
                const long long n = totalFrames + (long long)std::ceil(inPt * toDouble(rate)) + 2;
                long long first = 0, end = 0;
                trimRange(toDouble(rate), inPt, outPt - inPt, dur > 0 ? std::min(n, (long long)std::llround(dur * toDouble(rate))) : n, first, end);
                totalFrames = end > first ? end - first : -1;
            }
        }
        trimDuration = outPt > inPt ? outPt - inPt : -1;
        if (!o.ss.empty())
            startTime = o.rate ? round6(inPt) : std::ceil(round6(inPt) * toDouble(rate) - 1e-9) / toDouble(rate);

        // scale filters change the size; let ffmpeg tell us by probing the filtered output
        if (!o.vf.empty()) {
            std::string sizeCmd = shellQuote(ffmpeg) + " -hide_banner -v info -nostdin -i " + shellQuote(o.input) +
                " -vf " + shellQuote(o.vf + ",showinfo") + " -frames:v 1 -f null - 2>&1";
            bool sizeKnown = false;
            FILE* sp = openPipe(sizeCmd, "r");
            if (sp) {
                while (fgets(line, sizeof(line), sp)) {
                    std::string l(line); auto k = l.find(" s:");
                    if (k != std::string::npos) { int w = 0, h = 0; if (sscanf(l.c_str() + k + 3, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { width = w; height = h; sizeKnown = true; } }
                }
                pclose(sp);
            }
            if (!sizeKnown) die("could not determine the frame size after -vf " + o.vf);
        }

        // -ss before -i: fast seek, then ffmpeg decodes up to the exact frame (frame-accurate)
        std::string cmd = shellQuote(ffmpeg) + " -v error -nostdin";
        if (!o.ss.empty()) { char sb[64]; snprintf(sb, sizeof(sb), "%.6f", parseTime(o.ss)); cmd += std::string(" -ss ") + sb; }
        cmd += " -i " + shellQuote(o.input) + " -map 0:v:0 -an -sn";
        if (!o.to.empty() || !o.t.empty()) { char tb[64]; snprintf(tb, sizeof(tb), "%.6f", trimDuration > 0 ? trimDuration : 0.0); cmd += std::string(" -t ") + tb; }
        if (o.rate) { char rb[64]; snprintf(rb, sizeof(rb), "%lld/%lld", (long long)rate.numerator, (long long)rate.denominator); cmd += std::string(" -r ") + rb; }
        if (o.maxFrames > 0) cmd += " -frames:v " + std::to_string(o.maxFrames);   // ffmpeg stops cleanly by itself
        if (!o.vf.empty()) cmd += " -vf " + shellQuote(o.vf);
        colorNote = "ffmpeg default conversion";
        if (o.colorConv == "accurate") {
            const std::string conv = colorFilter(o, kv, width, height, colorNote);
            if (!conv.empty()) {
                // -vf (if any) runs first, then our conversion; both go in one filter chain
                const std::string chain = o.vf.empty() ? conv : o.vf + "," + conv;
                auto pos = cmd.find(" -vf ");
                if (pos != std::string::npos) cmd = cmd.substr(0, pos);   // -vf is the last option added so far
                cmd += " -vf " + shellQuote(chain);
            }
        }
        cmd += " -f rawvideo -pix_fmt argb -";
        pipe = openPipe(cmd, "rb");
        if (!pipe) die("could not run ffmpeg (" + ffmpeg + ")");
    }
    bool read(std::vector<uint8_t>& argb) override
    {
        argb.resize(size_t(width) * height * 4);
        return readFully(pipe, argb.data(), argb.size());
    }
};

// ---------------------------------------------------------------------------------------------
// 1.1.7: Hap R input. ffmpeg has no Hap R decoder, so pphap reads the .mov with the plugin's own
// QuickTime reader and decodes each frame itself: Hap container (snappy chunks) -> BC7 texture ->
// RGBA. Hap / Hap Alpha / Hap Q files are still decoded by ffmpeg, which supports them.

static void hapSerialCallback(HapDecodeWorkFunction fn, void* p, unsigned int count, void*)
{
    for (unsigned int i = 0; i < count; ++i) fn(p, i);
}

// one Hap R frame -> 8-bit ARGB, top-left origin (the layout the encoder pipeline uses)
static void decodeHapRFrame(const std::vector<uint8_t>& in, int w, int h, std::vector<uint8_t>& argb)
{
    unsigned int count = 0, fmt = 0;
    if (HapGetFrameTextureCount(in.data(), (unsigned long)in.size(), &count) != HapResult_No_Error || count != 1)
        throw std::runtime_error("damaged Hap R frame");
    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    std::vector<uint8_t> tex(size_t(bw) * bh * 16);
    unsigned long used = 0;
    unsigned int r = HapDecode(in.data(), (unsigned long)in.size(), 0, hapSerialCallback, nullptr,
                               tex.data(), (unsigned long)tex.size(), &used, &fmt);
    if (r != HapResult_No_Error) throw std::runtime_error("Hap R frame could not be decoded (error " + std::to_string(r) + ")");
    if (fmt != HapTextureFormat_RGBA_BPTC_UNORM) throw std::runtime_error("frame is not a BC7 (Hap R) texture");
    if (used < tex.size()) throw std::runtime_error("Hap R frame is shorter than its frame size");
    argb.resize(size_t(w) * h * 4);
    bc7decomp::color_rgba px[16];
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            bc7decomp::unpack_bc7(&tex[(size_t(by) * bw + bx) * 16], px);
            for (int y = 0; y < 4; ++y) {
                const int yy = by * 4 + y;
                if (yy >= h) break;
                for (int x = 0; x < 4; ++x) {
                    const int xx = bx * 4 + x;
                    if (xx >= w) break;
                    const bc7decomp::color_rgba& c = px[y * 4 + x];
                    uint8_t* d = &argb[(size_t(yy) * w + xx) * 4];
                    d[0] = c.a; d[1] = c.r; d[2] = c.g; d[3] = c.b;
                }
            }
        }
}

// opens INPUT if it is a QuickTime file with a Hap R video track; nullptr otherwise
static std::unique_ptr<MovieReader> openHapR(const std::string& path)
{
    try {
#ifdef _WIN32
        const std::filesystem::path fp = std::filesystem::u8path(path);
#else
        const std::filesystem::path fp(path);
#endif
        return createMovieReader(VideoFormat{ 'H', 'a', 'p', '7' }, fp);
    } catch (...) {
        return nullptr;
    }
}

// frame range [first, end) selected by -ss / -to / -t: the same frames ffmpeg picks for any other source
static void hapRRange(const Options& o, double fps, long long nFrames, const std::string& input, long long& first, long long& end)
{
    const double inPt = o.ss.empty() ? 0.0 : parseTime(o.ss);
    const double dur = !o.to.empty() ? parseTime(o.to) - inPt : (!o.t.empty() ? parseTime(o.t) : -1);
    trimRange(fps, inPt, dur, nFrames, first, end);
    if (first >= nFrames) die("-ss " + o.ss + " is past the end of " + input);
    if (end <= first) die("no frames between the in and out points");
}

struct HapRSource : Source {
    std::unique_ptr<MovieReader> reader;
    long long next = 0, end = 0;
    size_t ahead = 4;
    // the decode thread catches its own errors and returns the message as text: an exception carried
    // across std::future lost its message on some Windows C++ runtimes ("Unknown exception", 1.1.7)
    struct Decoded { std::vector<uint8_t> argb; std::string error; };
    std::deque<std::future<Decoded>> queue;
    std::deque<long long> queuedIndex;
    std::vector<uint8_t> lastGood;
    long long damaged = 0;
    HapRSource(const Options& o, std::unique_ptr<MovieReader> r, int threads) : reader(std::move(r))
    {
        width = reader->width(); height = reader->height();
        const Rational fileRate{ reader->frameRateNumerator(), reader->frameRateDenominator() };
        if (fileRate.numerator <= 0 || fileRate.denominator <= 0) die("could not read the frame rate of " + o.input);
        rate = o.rate ? *o.rate : fileRate;   // -r relabels the timing; every frame is kept
        const long long n = reader->numFrames();
        if (n <= 0) die("no frames in " + o.input);
        hapRRange(o, toDouble(fileRate), n, o.input, next, end);
        startTime = double(next) / toDouble(fileRate);
        totalFrames = end - next;
        ahead = size_t(std::max(2, threads * 2));
    }
    ~HapRSource() override
    {
        for (auto& f : queue) { try { f.wait(); } catch (...) {} }
        if (damaged > 0)
            fprintf(stderr, "pphap: warning: %lld damaged frame%s replaced by the previous frame\n", damaged, damaged == 1 ? "" : "s");
    }
    bool read(std::vector<uint8_t>& argb) override
    {
        while (queue.size() < ahead && next < end) {
            auto packet = std::make_shared<std::vector<uint8_t>>();
            queuedIndex.push_back(next);
            reader->readVideoFrame(int(next++), *packet);
            const int w = width, h = height;
            queue.push_back(std::async(std::launch::async, [packet, w, h]() {
                Decoded d;
                try {
                    decodeHapRFrame(*packet, w, h, d.argb);
                } catch (const std::exception& e) {
                    d.error = e.what();
                    if (d.error.empty()) d.error = "unreadable frame data";
                } catch (...) {
                    d.error = "unreadable frame data";
                }
                return d;
            }));
        }
        if (queue.empty()) return false;
        const long long index = queuedIndex.front();
        queuedIndex.pop_front();
        Decoded d;
        try {
            d = queue.front().get();
        } catch (...) {
            d.error = "decoder failure";
        }
        if (d.error.empty()) {
            argb = std::move(d.argb);
        } else {
            // like ffmpeg: a damaged frame doesn't stop the job; the previous frame is repeated so the
            // timing stays right (transparent black if the very first frame is damaged)
            if (++damaged <= 10)
                fprintf(stderr, "\npphap: warning: frame %lld is damaged (%s); using the previous frame\n", index, d.error.c_str());
            argb = lastGood.empty() ? std::vector<uint8_t>(size_t(width) * height * 4, 0) : lastGood;
        }
        queue.pop_front();
        lastGood = argb;
        return true;
    }
};

// Hap R input with -vf: pphap decodes, ffmpeg filters
//   pphap -i IN [-ss -to] -f rawvideo -pix_fmt rgba - | ffmpeg -f rawvideo ... -i - -vf VF -f rawvideo -pix_fmt argb -
struct HapRFilterSource : Source {
    FILE* pipe = nullptr;
    ~HapRFilterSource() override { if (pipe) pclose(pipe); }
    HapRFilterSource(const Options& o, std::unique_ptr<MovieReader> r)
    {
        const int srcW = r->width(), srcH = r->height();
        const Rational fileRate{ r->frameRateNumerator(), r->frameRateDenominator() };
        long long first = 0, end = 0;
        hapRRange(o, toDouble(fileRate), r->numFrames(), o.input, first, end);
        startTime = double(first) / toDouble(fileRate);
        r.reset();
        rate = o.rate ? *o.rate : fileRate;
        totalFrames = end - first;
        const std::string self = exePath().empty() ? std::string("pphap") : exePath();
        const std::string ffmpeg = findTool("ffmpeg");
        char rb[64]; snprintf(rb, sizeof(rb), "%lld/%lld", (long long)rate.numerator, (long long)rate.denominator);
        std::string dec = shellQuote(self) + " -v error -nostats -i " + shellQuote(o.input);
        if (!o.ss.empty()) dec += " -ss " + shellQuote(o.ss);
        if (!o.to.empty()) dec += " -to " + shellQuote(o.to);
        if (!o.t.empty()) dec += " -t " + shellQuote(o.t);
        const std::string rawIn = " -nostdin -f rawvideo -pix_fmt rgba -s " + std::to_string(srcW) + "x" +
                                  std::to_string(srcH) + " -r " + rb + " -i -";
        // size after the filter
        std::string probe = dec + " -frames:v 1 -f rawvideo -pix_fmt rgba - | " + shellQuote(ffmpeg) + " -hide_banner -v info" + rawIn +
                            " -vf " + shellQuote(o.vf + ",showinfo") + " -frames:v 1 -f null - 2>&1";
        bool sizeKnown = false;
        if (FILE* sp = openPipe(probe, "r")) {
            char line[512];
            while (fgets(line, sizeof(line), sp)) {
                std::string l(line); auto k = l.find(" s:");
                if (k != std::string::npos) { int w = 0, h = 0; if (sscanf(l.c_str() + k + 3, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) { width = w; height = h; sizeKnown = true; } }
            }
            pclose(sp);
        }
        if (!sizeKnown) die("could not determine the frame size after -vf " + o.vf);
        std::string cmd = dec;
        if (o.maxFrames > 0) cmd += " -frames:v " + std::to_string(o.maxFrames);
        cmd += " -f rawvideo -pix_fmt rgba - | " + shellQuote(ffmpeg) + " -v error" + rawIn + " -vf " +
               shellQuote(o.vf + ",scale=flags=accurate_rnd+full_chroma_int+full_chroma_inp+neighbor") + " -f rawvideo -pix_fmt argb -";
        pipe = openPipe(cmd, "rb");
        if (!pipe) die("could not run ffmpeg (" + ffmpeg + ")");
    }
    bool read(std::vector<uint8_t>& argb) override
    {
        argb.resize(size_t(width) * height * 4);
        return readFully(pipe, argb.data(), argb.size());
    }
};

// ---------------------------------------------------------------------------------------------
// 1.1.7: audio. ffmpeg decodes the source's first audio track to PCM; it is cut to start on the
// first video frame delivered and to last exactly as long as the video, and interleaved per frame.

struct AudioFeed {
    FILE* pipe = nullptr;
    int sampleRate = 0, channels = 0, bytes = 2;
    long long written = 0;          // sample frames written so far (= the next packet's pts)
    bool ended = false;
    std::vector<uint8_t> buf;
    ~AudioFeed() { if (pipe) pclose(pipe); }

    // probes INPUT; false (and no audio) if it has no audio track
    bool open(const Options& o, double start, double duration)
    {
        const std::string ffprobe = findTool("ffprobe"), ffmpeg = findTool("ffmpeg");
        FILE* p = openPipe(shellQuote(ffprobe) + " -v error -select_streams a:0 -show_entries stream=sample_rate,channels"
                           " -of default=nw=1 " + shellQuote(o.input), "r");
        if (!p) return false;
        char line[256];
        while (fgets(line, sizeof(line), p)) {
            if (!strncmp(line, "sample_rate=", 12)) sampleRate = atoi(line + 12);
            else if (!strncmp(line, "channels=", 9)) channels = atoi(line + 9);
        }
        pclose(p);
        if (sampleRate <= 0 || channels <= 0) return false;
        if (channels > 16) channels = 16;
        bytes = o.audioBytes;
        char sb[64]; snprintf(sb, sizeof(sb), "%.6f", start);
        std::string cmd = shellQuote(ffmpeg) + " -v error -nostdin";
        if (start > 0) cmd += std::string(" -ss ") + sb;
        cmd += " -i " + shellQuote(o.input) + " -map 0:a:0 -vn -sn -dn";
        if (duration > 0) { char db[64]; snprintf(db, sizeof(db), "%.6f", duration); cmd += std::string(" -t ") + db; }
        cmd += " -ac " + std::to_string(channels) + " -ar " + std::to_string(sampleRate);
        cmd += bytes == 4 ? " -c:a pcm_s32le -f s32le -" : " -c:a pcm_s16le -f s16le -";
        pipe = openPipe(cmd, "rb");
        return pipe != nullptr;
    }
    AudioDef def() const { return AudioDef{ channels, sampleRate, bytes, AudioEncoding_Signed_PCM }; }

    // writes audio up to sample frame `target` (silence if the source's audio ends early)
    void writeUpTo(MovieWriter& w, long long target)
    {
        const size_t frameBytes = size_t(channels) * bytes;
        while (written < target) {
            const long long n = std::min<long long>(target - written, sampleRate);   // at most 1 s per packet
            buf.assign(size_t(n) * frameBytes, 0);
            if (!ended) {
                size_t got = 0;
                while (got < buf.size()) {
                    size_t r = fread(buf.data() + got, 1, buf.size() - got, pipe);
                    if (r == 0) { ended = true; break; }
                    got += r;
                }
            }
            w.writeAudioFrame(buf.data(), buf.size(), written);
            written += n;
        }
    }
};

// ---------------------------------------------------------------------------------------------
// progress reporting

// width of the terminal stderr is attached to (0 if unknown / not a terminal)
static int terminalColumns()
{
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_ERROR_HANDLE), &info))
        return info.srWindow.Right - info.srWindow.Left + 1;
    return 0;
#else
    struct winsize ws{};
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) == 0) return ws.ws_col;
    return 0;
#endif
}

struct Progress {
    FILE* out = nullptr;
    bool ownsOut = false;
    bool stats = true;
    int lastStatusLen = 0;
    Rational rate{ 30, 1 };
    long long total = -1;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    void emit(long long frames, unsigned long long bytes, bool end)
    {
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        double outTime = frames / toDouble(rate);
        double fps = elapsed > 0 ? frames / elapsed : 0.0;
        double speed = elapsed > 0 ? outTime / elapsed : 0.0;
        double kbps = outTime > 0 ? bytes * 8.0 / outTime / 1000.0 : 0.0;
        if (out) {
            long long us = (long long)std::llround(outTime * 1e6);
            fprintf(out, "frame=%lld\nfps=%.2f\n", frames, fps);
            if (total > 0) fprintf(out, "total_frames=%lld\npercent=%.1f\n", total, std::min(100.0, 100.0 * frames / total));
            fprintf(out, "out_time_us=%lld\nout_time_ms=%lld\nout_time=%s\ntotal_size=%llu\nbitrate=%.1fkbits/s\nspeed=%.3gx\nprogress=%s\n",
                    us, us, formatTime(outTime).c_str(), bytes, kbps, speed, end ? "end" : "continue");
            fflush(out);
        }
        if (stats) {
            char pct[32] = "";
            if (total > 0) snprintf(pct, sizeof(pct), " (%3.0f%%)", std::min(100.0, 100.0 * frames / total));
            char line[256];
            snprintf(line, sizeof(line), "frame=%6lld%s fps=%5.1f size=%9lluKiB time=%s bitrate=%8.1fkbits/s speed=%5.2fx",
                     frames, pct, fps, bytes / 1024, formatTime(outTime).substr(0, 11).c_str(), kbps, speed);
            // keep the status on one terminal row: a wrapped line can't be redrawn with \r
            std::string text(line);
            const int cols = terminalColumns();
            if (cols > 1 && int(text.size()) > cols - 1) text.resize(size_t(cols - 1));
            if (int(text.size()) < lastStatusLen) text.append(size_t(lastStatusLen - int(text.size())), ' ');
            lastStatusLen = int(text.size());
            fprintf(stderr, "\r%s", text.c_str());
            if (end) fprintf(stderr, "\n");
            fflush(stderr);
        }
    }
    ~Progress() { if (ownsOut && out) fclose(out); }
};

std::atomic<bool> g_stop{ false };
extern "C" void onSignal(int) { g_stop = true; }

void openProgress(const Options& o, Progress& progress)
{
    if (o.progressUrl.empty()) return;
    if (o.progressUrl == "pipe:1" || o.progressUrl == "pipe:" || o.progressUrl == "-") progress.out = stdout;
    else if (o.progressUrl == "pipe:2") progress.out = stderr;
    else {
        std::string path = o.progressUrl.rfind("file:", 0) == 0 ? o.progressUrl.substr(5) : o.progressUrl;
        progress.out = fopenUtf8(path, "w");
        if (!progress.out) die("cannot open progress output " + path);
        progress.ownsOut = true;
    }
}

// 1.1.7: decoded frames out (stdout or a file) instead of a Hap movie
int runRawOutput(const Options& o, Source& src, bool hapRInput)
{
    const bool toStdout = o.output == "-" || o.output == "pipe:" || o.output == "pipe:1";
    FILE* out = stdout;
    if (toStdout) {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#else
        signal(SIGPIPE, SIG_IGN);   // a reader that closes the pipe early just ends the job
#endif
    } else {
        out = fopenUtf8(o.output, "wb");
        if (!out) die("cannot create " + o.output);
    }
    Progress progress;
    progress.rate = src.rate;
    progress.total = src.totalFrames;
    progress.stats = o.stats && !o.quiet;
    openProgress(o, progress);
    if (!o.quiet)
        fprintf(stderr, "pphap: %s -> %s  %dx%d @ %.3f fps  rawvideo %s%s%s\n", o.input.c_str(), toStdout ? "stdout" : o.output.c_str(),
                src.width, src.height, toDouble(src.rate), o.outPixFmt.c_str(), hapRInput ? " (Hap R decoded by pphap)" : "",
                src.totalFrames > 0 ? (", " + std::to_string(src.totalFrames) + " frames").c_str() : "");

    const size_t px = size_t(src.width) * src.height;
    const size_t bpp = o.outPixFmt == "rgb24" ? 3 : 4;
    std::vector<uint8_t> argb, buf(px * bpp);
    long long frames = 0;
    unsigned long long bytes = 0;
    bool pipeClosed = false;
    int exitCode = 0;
    auto lastReport = std::chrono::steady_clock::now();
    const auto period = std::chrono::duration<double>(o.statsPeriod);
    try {
        while (!g_stop && (o.maxFrames < 0 || frames < o.maxFrames) && src.read(argb)) {
            const uint8_t* s = argb.data();
            uint8_t* d = buf.data();
            if (o.outPixFmt == "argb") memcpy(d, s, px * 4);
            else if (o.outPixFmt == "rgba") for (size_t i = 0; i < px; ++i, s += 4, d += 4) { d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; d[3] = s[0]; }
            else if (o.outPixFmt == "bgra") for (size_t i = 0; i < px; ++i, s += 4, d += 4) { d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = s[0]; }
            else                            for (size_t i = 0; i < px; ++i, s += 4, d += 3) { d[0] = s[1]; d[1] = s[2]; d[2] = s[3]; }
            if (fwrite(buf.data(), 1, buf.size(), out) != buf.size()) { pipeClosed = true; break; }
            ++frames;
            bytes += buf.size();
            auto now = std::chrono::steady_clock::now();
            if (now - lastReport >= period) { lastReport = now; progress.emit(frames, bytes, false); }
        }
        if (!pipeClosed && fflush(out) != 0) pipeClosed = true;
    } catch (const std::exception& e) {
        fprintf(stderr, "\npphap: error: %s\n", e.what());
        exitCode = 1;
    }
    if (!toStdout && fclose(out) != 0 && exitCode == 0) { fprintf(stderr, "pphap: error: could not write %s\n", o.output.c_str()); exitCode = 1; }
    progress.emit(frames, bytes, true);
    if (pipeClosed) {
        if (!toStdout) { fprintf(stderr, "pphap: error: could not write %s\n", o.output.c_str()); exitCode = 1; }
        else if (!o.quiet) fprintf(stderr, "pphap: output closed by the reader after %lld frames\n", frames);
    }
    if (frames == 0 && exitCode == 0 && !pipeClosed) { fprintf(stderr, "pphap: error: no frames were read from %s\n", o.input.c_str()); exitCode = 1; }
    if (g_stop && exitCode == 0) exitCode = 255;
    return exitCode;
}

} // namespace

// ---------------------------------------------------------------------------------------------

int main(int argc, char** argv)
{
    // messages go out at once, also into a pipe (Windows buffered them until exit when piped)
    setvbuf(stderr, nullptr, _IONBF, 0);
    hapgd::registerCodec();   // 1.2.2: read GDeflate Hap R as well
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    // arguments as UTF-8, from the UTF-16 command line (argv is in the ANSI code page)
    std::vector<std::string> u8args;
    std::vector<char*> u8argv;
    {
        int wargc = 0;
        LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv) {
            for (int i = 0; i < wargc; ++i) u8args.push_back(narrow(wargv[i]));
            LocalFree(wargv);
            for (auto& a : u8args) u8argv.push_back(&a[0]);
            u8argv.push_back(nullptr);
            argc = wargc;
            argv = u8argv.data();
        }
    }
#endif
    Options o = parse(argc, argv);
    av_log_set_level(AV_LOG_FATAL);   // the QuickTime reader reports problems as exceptions

    const Variant* variant = nullptr;
    for (auto& v : kVariants) if (o.codec == v.name) variant = &v;
    if (!variant) die("unknown codec '" + o.codec + "' (hap, hap_alpha, hap_q, hap_q_alpha, hap_alpha_only, hap_r)");

    int quality = 0;
    if (o.quality == "fast" || o.quality == "0") quality = 0;
    else if (o.quality == "normal" || o.quality == "1") quality = 1;
    else if (o.quality == "high" || o.quality == "best" || o.quality == "2") quality = 2;
    else die("unknown -quality " + o.quality + " (fast, normal, high)");
    if (o.chunks < 0 || o.chunks > 64) die("-chunks must be 1-64");

    if (fileExists(o.output)) {
        if (o.noOverwrite) die("output exists: " + o.output);
        if (!o.overwrite) die("output exists: " + o.output + " (use -y to overwrite)");
    }

    const int nThreads = o.threads > 0 ? o.threads : std::max(1u, std::thread::hardware_concurrency());
    std::unique_ptr<Source> src;
    bool hapRInput = false;
    if (o.input == "-" || o.input == "pipe:" || o.input == "pipe:0") {
        if (!o.inFormat.empty() && o.inFormat != "rawvideo") die("only -f rawvideo is supported for piped input");
        src = std::make_unique<RawPipeSource>(stdin, o);
    } else {
        if (!fileExists(o.input)) die("input not found: " + o.input);
        if (auto reader = openHapR(o.input)) {   // 1.1.7: Hap R is decoded here, not by ffmpeg
            hapRInput = true;
            if (o.vf.empty()) src = std::make_unique<HapRSource>(o, std::move(reader), nThreads);
            else src = std::make_unique<HapRFilterSource>(o, std::move(reader));
        } else {
            src = std::make_unique<FfmpegSource>(o);
        }
    }
    if (o.maxFrames > 0 && (src->totalFrames < 0 || src->totalFrames > o.maxFrames)) src->totalFrames = o.maxFrames;

    signal(SIGINT, onSignal);
#ifdef SIGTERM
    signal(SIGTERM, onSignal);
#endif
#ifdef SIGBREAK
    signal(SIGBREAK, onSignal);   // 1.1.5: Windows Ctrl+Break (GenerateConsoleCtrlEvent) finishes the file cleanly
#endif
    if (o.rawOut) return runRawOutput(o, *src, hapRInput);

    const int w = src->width, h = src->height;
    Rational rate = SimplifyAndSnapToMpegFrameRate(src->rate);
    bool alpha = o.alpha >= 0 ? (o.alpha != 0) : variant->alpha;

    // encoder: exactly the plugin's codec
    setTextureEngine(o.engine == "legacy" ? TextureEngine::Legacy : TextureEngine::Accurate);
    {   // 1.2.0: GPU Hap R encoder
        std::string g = o.gpu;
        // 1.2.1: off unless asked for - on test PCs (RTX 3080 / i7-13700K) the CPU was ~10x faster
        if (g.empty()) { const char* e = getenv("PPHAP_GPU"); g = e && *e ? e : "off"; }
        gpubc7::Mode m = gpubc7::Mode::Auto;
        if (g == "off" || g == "0" || g == "no" || o.engine == "legacy") m = gpubc7::Mode::Off;
        else if (g == "on" || g == "1" || g == "yes") m = gpubc7::Mode::On;
        else if (g == "force") m = gpubc7::Mode::Force;   // testing: software Vulkan devices too
        else if (g != "auto") die("-gpu must be auto, on or off");
        gpubc7::setMode(m);
    }
    if (o.compressor == "gdeflate") { hapgd::setLevel(o.gdLevel); hapgd::setEnabled(true); }
    else if (o.compressor != "snappy") die("-compressor must be snappy or gdeflate");
    enableQualityReport(o.psnr);
    auto codec = CodecRegistry::codec();
    std::string encoderName = variant->name;
    for (const auto& named : codec->details().subtypes) if (named.first == variant->cc) encoderName = named.second;
    unsigned chunks = (unsigned)o.chunks;
    auto params = std::make_unique<EncoderParametersBase>(FrameSize{ w, h }, alpha ? withAlpha : withoutAlpha,
                                                          variant->cc, HapChunkCounts{ chunks, chunks }, quality);
    UniqueEncoder encoder = codec->createEncoder(std::move(params));

    VideoDef video{ w, h, variant->cc, encoderName, encoder->encodedBitDepth(), rate,
                    src->totalFrames > 0 ? src->totalFrames : 0 };

    // 1.1.7: audio, as ffmpeg does it: the source's first audio track comes along unless -an
    std::unique_ptr<AudioFeed> audio;
    const bool inPipe = o.input == "-" || o.input == "pipe:" || o.input == "pipe:0";
    if (!o.noAudio && !inPipe) {
        if (hapRInput && o.rate)
            fprintf(stderr, "pphap: note: -r retimes a Hap R source, so its audio is left out\n");
        else {
            audio = std::make_unique<AudioFeed>();
            const double dur = src->totalFrames > 0 ? double(src->totalFrames) / toDouble(rate) : -1;
            if (!audio->open(o, src->startTime, dur)) audio.reset();
        }
    }
    std::optional<AudioDef> audioDef;
    if (audio) audioDef = audio->def();

    std::unique_ptr<MovieWriter> writer;
    try {
        writer = std::make_unique<MovieWriter>(0, createMovieFile(o.output), video, audioDef, false);
        writer->writeHeader();
    } catch (const std::exception& e) {
        die(std::string("cannot create ") + o.output + ": " + e.what());
    }

    Progress progress;
    progress.rate = rate;
    progress.total = src->totalFrames;
    progress.stats = o.stats && !o.quiet;
    openProgress(o, progress);
    if (!o.quiet)
        fprintf(stderr, "pphap: %s -> %s  %dx%d @ %.3f fps  %s (%s)%s\n", o.input.c_str(), o.output.c_str(), w, h,
                toDouble(rate), encoderName.c_str(), o.quality.c_str(),
                src->totalFrames > 0 ? (", " + std::to_string(src->totalFrames) + " frames").c_str() : "");
    if (!o.quiet) {
        auto* fs = dynamic_cast<FfmpegSource*>(src.get());
        const bool isHapR = std::string(variant->name) == "hap_r";
        fprintf(stderr, "pphap: colour: %s; encoder: %s%s%s\n",
                fs ? fs->colorNote.c_str() : (hapRInput ? "Hap R source, decoded by pphap" : "raw RGB input, no conversion"),
                textureEngineName(textureEngine()), isHapR ? ", bc7e " : "", isHapR ? bc7eSimdName() : "");
        // 1.2.0: say whether the GPU Hap R encoder can be used (it is tried for Hap R at -quality fast)
        if (std::string(variant->name) == "hap_r" && quality == 0 && gpubc7::mode() != gpubc7::Mode::Off)
            fprintf(stderr, "pphap: GPU: %s%s\n", gpubc7::available() ? "" : "not used - ", gpubc7::description().c_str());
        if (hapgd::enabled())
            fprintf(stderr, "pphap: compression: GDeflate level %d - NOT standard Hap: plays only in PixelPress players\n", hapgd::level());
        if (audio) fprintf(stderr, "pphap: audio: %d Hz, %d channel%s, pcm_s%dle\n", audio->sampleRate, audio->channels,
                           audio->channels == 1 ? "" : "s", audio->bytes * 8);
    }

    // pipeline: read (prefetch thread) -> encode (pool) -> write in order (this thread)
    // 1.2.1: frames in flight are capped by memory too. Each holds the frame (dropped once copied),
    // an RGBA copy and its textures, ~2x the frame size; 2 per thread used to be ~12 GB at 8K and
    // delayed the first finished frame by ~100 frames of decoding. 4K: ~40 frames, 8K: ~10,
    // 1080p and smaller: 2 per thread as before. Every frame is split across all cores anyway.
    const size_t frameBytes = size_t(w) * h * 4;
    const size_t memBudget = size_t(3) << 30;   // 3 GB
    const size_t maxInFlight = std::max<size_t>(4, std::min(size_t(nThreads) * 2, memBudget / (frameBytes * 2 + 1)));
    src = std::make_unique<PrefetchSource>(std::move(src), std::max<size_t>(2, std::min<size_t>(8, (size_t(512) << 20) / (frameBytes + 1))));
    std::deque<std::future<std::vector<uint8_t>>> inFlight;
    std::mutex encMutex;   // encoder->create() is not assumed thread safe
    long long framesRead = 0, framesWritten = 0;
    unsigned long long bytesWritten = 0;
    auto lastReport = std::chrono::steady_clock::now();
    const auto period = std::chrono::duration<double>(o.statsPeriod);

    auto writeOne = [&]() {
        std::vector<uint8_t> encoded = inFlight.front().get();   // rethrows encode errors
        inFlight.pop_front();
        writer->writeVideoFrame(encoded.data(), encoded.size());
        ++framesWritten;
        if (audio)   // audio up to the end of this frame
            audio->writeUpTo(*writer, (long long)std::floor(double(framesWritten) * audio->sampleRate * double(rate.denominator) / double(rate.numerator) + 0.5));
        bytesWritten += encoded.size();
        auto now = std::chrono::steady_clock::now();
        if (now - lastReport >= period) { lastReport = now; progress.emit(framesWritten, bytesWritten, false); }
    };

    int exitCode = 0;
    try {
        std::vector<uint8_t> frame;
        while (!g_stop && (o.maxFrames < 0 || framesRead < o.maxFrames) && src->read(frame)) {
            ++framesRead;
            std::unique_ptr<EncoderJob> job;
            {
                std::lock_guard<std::mutex> lock(encMutex);
                job = encoder->create();
            }
            auto data = std::make_shared<std::vector<uint8_t>>(std::move(frame));
            frame = std::vector<uint8_t>();
            inFlight.push_back(std::async(std::launch::async, [w, data, j = std::shared_ptr<EncoderJob>(std::move(job))]() mutable {
                j->copyExternalToLocal(data->data(), size_t(w) * 4,
                                       FrameFormat(ChannelFormat_U8 | ChannelLayout_ARGB | FrameOrigin_TopLeft));
                data.reset();   // 1.2.1: the job has its own copy now
                EncodeOutput out;
                j->encode(out);
                return std::move(out.buffer);
            }));
            while (inFlight.size() >= maxInFlight) writeOne();
        }
        while (!inFlight.empty()) writeOne();
        writer->flush();
        writer->writeTrailer();
        writer->close();
    } catch (const std::exception& e) {
        while (!inFlight.empty()) { try { inFlight.front().get(); } catch (...) {} inFlight.pop_front(); }
        fprintf(stderr, "\npphap: error: %s\n", e.what());
        exitCode = 1;
    }

    progress.emit(framesWritten, bytesWritten, true);
    if (framesWritten == 0 && exitCode == 0) {
        fprintf(stderr, "pphap: error: no frames were read from %s\n", o.input.c_str());
        exitCode = 1;
    }
    if (o.psnr && framesWritten > 0) {
        const QualityReport q = qualityReport();
        // printed even with -loglevel quiet: it was asked for
        fprintf(stderr, "pphap: accuracy over %lld frames (decoded texture vs the frame that was encoded)\n", q.frames);
        if (q.pixels) {
            fprintf(stderr, "pphap:   PSNR RGB %.2f dB (R %.2f G %.2f B %.2f)  luma %.2f dB  worst channel error %d\n",
                    q.psnrRGB(), q.psnrChannel(0), q.psnrChannel(1), q.psnrChannel(2), q.psnrLuma(), q.maxErr);
            fprintf(stderr, "pphap:   colour difference CIEDE2000: mean %.3f  99th pct %.2f  max %.1f   (1.0 = just noticeable)\n",
                    q.deMean(), q.dePercentile(99), q.deMax);
        }
        if (q.alphaPixels && alpha)
            fprintf(stderr, "pphap:   PSNR alpha %.2f dB\n", q.psnrAlpha());
        if (o.progressUrl.size() && progress.out) {
            fprintf(progress.out, "psnr_rgb=%.3f\npsnr_luma=%.3f\ndelta_e_mean=%.4f\ndelta_e_p99=%.3f\n",
                    q.psnrRGB(), q.psnrLuma(), q.deMean(), q.dePercentile(99));
            fflush(progress.out);
        }
    }
    if (g_stop) {
        if (!o.quiet) fprintf(stderr, "pphap: interrupted - wrote %lld frames (file is complete up to that point)\n", framesWritten);
        if (exitCode == 0) exitCode = 255;   // same as ffmpeg when interrupted
    }
    return exitCode;
}
