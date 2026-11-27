// tekken6-nx (2026-11-15): benchmark of FFmpeg's WMV3 decoder on the console (tekken_wmv3_bench, off).
//
// The intro movies (game/DATA315..320.BIN, ASF: WMV3 1280x720 29.97 fps) play through the XDK's XMV player, whose
// recompiled decoder reaches ~40 % of real time on the console (docs/switch-port-log.md). Before replacing it with
// FFmpeg's decoder, this measures how fast FFmpeg decodes the same frames on one A57 core: at start-up, before the game
// runs, it decodes the frames of a file made on the PC by tools/wmv3_bench_extract.py and prints the rate to stderr
// (rex_stderr.log) and the log. Then the game starts as usual.
//
// File: "WMV3BNCH", u32 width, u32 height, u32 extradata size, extradata, u32 frame count, then per frame: u32 size,
// u32 flags (bit 0: key frame), the frame's bytes (all little-endian).

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/mem.h"
}

#if defined(__SWITCH__)
#include <switch.h>
#endif

REXCVAR_DEFINE_STRING(tekken_wmv3_bench, "", "Tekken6/Diag",
                      "Path of a WMV3 frame file (tools/wmv3_bench_extract.py): at start-up its frames are decoded with "
                      "FFmpeg on one thread and the rate is logged ([wmv3-bench] in rex_stderr.log). Empty: off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(tekken_wmv3_bench_frames, 900, "Tekken6/Diag",
                     "tekken_wmv3_bench: at most this many frames (900 = 30 s of video)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace tekken::wmv3_bench {

namespace {

void Report(const char* text) {
  std::fprintf(stderr, "[wmv3-bench] %s\n", text);
  std::fflush(stderr);
  REXLOG_INFO("[wmv3-bench] {}", text);
}

uint32_t ReadU32(const std::vector<uint8_t>& d, size_t& at, bool& ok) {
  if (at + 4 > d.size()) {
    ok = false;
    return 0;
  }
  uint32_t v;
  std::memcpy(&v, d.data() + at, 4);
  at += 4;
  return v;
}

}  // namespace

void RunIfAsked() {
  const std::string path = REXCVAR_GET(tekken_wmv3_bench);
  if (path.empty()) return;
  char line[256];

  std::vector<uint8_t> data;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    data.resize(size > 0 ? size_t(size) : 0);
    const size_t got = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) data.clear();
  }
  if (data.size() < 24 || std::memcmp(data.data(), "WMV3BNCH", 8) != 0) {
    std::snprintf(line, sizeof(line), "cannot read %s (missing, short or not a WMV3BNCH file)", path.c_str());
    Report(line);
    return;
  }

  bool ok = true;
  size_t at = 8;
  const uint32_t width = ReadU32(data, at, ok), height = ReadU32(data, at, ok), extra = ReadU32(data, at, ok);
  if (!ok || at + extra > data.size()) {
    Report("bad header");
    return;
  }
  const size_t extra_at = at;
  at += extra;
  const uint32_t count = ReadU32(data, at, ok);
  struct Frame {
    size_t at;
    uint32_t size, flags;
  };
  std::vector<Frame> frames;
  const uint32_t limit = uint32_t(std::max(1, int(REXCVAR_GET(tekken_wmv3_bench_frames))));
  for (uint32_t i = 0; ok && i < count && frames.size() < limit; ++i) {
    const uint32_t size = ReadU32(data, at, ok), flags = ReadU32(data, at, ok);
    if (!ok || at + size > data.size()) break;
    frames.push_back({at, size, flags});
    at += size;
  }

  const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_WMV3);
  if (!codec) {
    Report("FFmpeg has no WMV3 decoder in this build");
    return;
  }
  AVCodecContext* ctx = avcodec_alloc_context3(codec);
  ctx->width = int(width);
  ctx->height = int(height);
  ctx->thread_count = 1;
  ctx->extradata = static_cast<uint8_t*>(av_mallocz(extra + AV_INPUT_BUFFER_PADDING_SIZE));
  std::memcpy(ctx->extradata, data.data() + extra_at, extra);
  ctx->extradata_size = int(extra);
  if (avcodec_open2(ctx, codec, nullptr) < 0) {
    Report("avcodec_open2 failed");
    avcodec_free_context(&ctx);
    return;
  }

  // Each packet's bytes, padded as FFmpeg requires (its bitstream reader may read past the end).
  std::vector<uint8_t> buffer;
  AVPacket* packet = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  int decoded = 0, errors = 0;
  double worst_ms = 0.0, total_ms = 0.0;
  using clock = std::chrono::steady_clock;
#if defined(__SWITCH__)
  const int core = int(svcGetCurrentProcessorNumber());
#else
  const int core = -1;
#endif
  std::snprintf(line, sizeof(line), "%zu frames of %ux%u (%.1f MB), FFmpeg %s, 1 thread, on core %d: decoding...",
                frames.size(), width, height, double(at) / 1048576.0, av_version_info(), core);
  Report(line);

  for (const Frame& f : frames) {
    buffer.assign(data.begin() + f.at, data.begin() + f.at + f.size);
    buffer.resize(f.size + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    packet->data = buffer.data();
    packet->size = int(f.size);
    packet->flags = (f.flags & 1) ? AV_PKT_FLAG_KEY : 0;
    const auto t0 = clock::now();
    if (avcodec_send_packet(ctx, packet) < 0) ++errors;
    while (avcodec_receive_frame(ctx, frame) == 0) ++decoded;
    const double ms = std::chrono::duration<double, std::milli>(clock::now() - t0).count();
    total_ms += ms;
    if (ms > worst_ms) worst_ms = ms;
  }
  avcodec_send_packet(ctx, nullptr);  // the decoder's delayed frame
  {
    const auto t0 = clock::now();
    while (avcodec_receive_frame(ctx, frame) == 0) ++decoded;
    total_ms += std::chrono::duration<double, std::milli>(clock::now() - t0).count();
  }

  const double fps = total_ms > 0.0 ? decoded * 1000.0 / total_ms : 0.0;
  std::snprintf(line, sizeof(line),
                "RESULT: %d frames decoded in %.2f s = %.1f fps (the movie needs 29.97), %.2f ms per frame on average, "
                "worst %.1f ms, %d errors",
                decoded, total_ms / 1000.0, fps, decoded ? total_ms / decoded : 0.0, worst_ms, errors);
  Report(line);

  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&ctx);
}

}  // namespace tekken::wmv3_bench
