// tekken6-nx (2026-11-15): intro movies - the game's WMV3 decoding replaced by FFmpeg's (port of nfsmw-nx's
// nfsmw_video_nativo.cpp / nfsmw_video_wmv3.cpp, same XDK WMV library in another version).
//
// The intro movies (game/DATA315..320.BIN: ASF, WMV3 main profile 1280x720 29.97 fps, sequence bytes 4F F1 08 01: no
// loop filter, no B frames, no X8; I and P frames only) play through the XDK's XMV player. Its recompiled WMV3 decoder
// takes ~98 % of a core on the Switch and reaches ~40 % of real time, so the picture lags the audio. FFmpeg's WMV3
// decodes the same frames at ~65 fps on one A57 core (tekken_wmv3_bench).
//
// The game's decoder interface (recompiled code; "ctx" is the decoder context, r3 of every function):
//  - sub_825CF600 prepares a movie's context: [ctx+3364] = r4 = the data structure {application data, function}.
//  - sub_825D6328 decodes one frame: it requests the first chunk of the compressed frame through the data thunk
//    sub_825EA6F8 (jumps to [[r3+4]] with r3 = [r3]; r4 = offset 0, r5 = &pointer to the data, r6 = 4, r7 = &bytes
//    returned, r8 = &data remaining) with r3 = [ctx+3364], and calls DecodeData sub_825D2B30(ctx, data, bytes,
//    remaining, ...). DecodeData sets up the bit reader [ctx+80] (sub_825F8888: [reader+24] = remaining, [reader+40]
//    = ctx, [reader+44] = the data structure), which asks for the next chunks through sub_825F87B0 (same thunk,
//    r3 = [reader+44], r8 = reader+24). The tail of DecodeData (sub_825CE798) drains the rest.
//  - DecodeData stores the per-type functions every frame (profile [ctx+15504] == 6 for WMV3) and calls them by type
//    [ctx+284] (0 I, 1 P, 2 B, 4 BI): I with [ctx+15804], P with [ctx+15808], B with [ctx+3080]. Two variants:
//      variant 1: I sub_827080C8, P sub_82608D20, B sub_827234F8 (the one the console profile shows);
//      variant 2: I sub_825D73A0, P sub_82628670, B sub_8261AAA0.
//    X8 intra frames ([ctx+3972]) go to sub_82612A98 and other cases elsewhere: those the game decodes itself (and
//    FFmpeg decodes the same bytes afterwards to keep its references).
//  - The new picture goes into planes [ctx+3744] (Y), [ctx+3748] (U), [ctx+3752] (V) (the reference set is
//    [ctx+3756/3760/3764]), with the origin at +[ctx+220] (Y) and +[ctx+224] (U/V); strides [ctx+204] (Y) and
//    [ctx+208] (U/V); each buffer has [ctx+212] (Y) / [ctx+216] (U/V) rows (DecodeData copies stride * rows).
//  - [ctx+3916] is the sequence loop filter. When they finish, I sets [ctx+15596] = 0; P sets [ctx+15592] =
//    ([ctx+3916], [ctx+14856] != 0 or [ctx+15228] != -1), [ctx+15596] = 0 and [ctx+15568] = 1 (B would also set
//    [ctx+15592] = [ctx+15568] = [ctx+456] = 1: B frames are not replaced, the game decodes them).
//    nfsmw -> Tekken: 3300 -> 3364, 76 -> 80, 280 -> 284, 15424 -> 15504, 15708/15712/3016 -> 15804/15808/3080,
//    3672.. -> 3744.., 216/220 -> 220/224 (Tekken has explicit strides), 3844 -> 3916, 15516/15512/15488 ->
//    15596/15592/15568, 14776/15148 -> 14856/15228, 436 -> 456.
//
// Cvars:
//  - tekken_video_wmv3_native: the I and P functions do not decode. FFmpeg decodes the same bytes the game requested
//    (each frame comes out of its own call, no reordering) and its planes are copied into the game's buffers. A frame
//    that cannot be replaced is decoded by the game, and the next native one waits for an I frame. Without an FFmpeg
//    WMV3 decoder (the PC build) nothing is hooked in practice: the game decodes as before.
//  - tekken_video_wmv3_shadow (diagnostic): the game decodes and its planes are compared with FFmpeg's; the luma of
//    frame 30 of each movie is saved as PGM in the working folder.
//  - tekken_video_wmv3_diag (diagnostic): logs the data calls and the first frames of each movie.
// FFmpeg needs the size and the sequence bytes: they come from the ASF header of the last ASF file the game read
// (TekkenLastAsfRead, sdk/src/kernel/xboxkrnl/xboxkrnl_io.cpp).

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavutil/mem.h"
}

namespace rex::kernel::xboxkrnl {
bool TekkenLastAsfRead(std::string& path, std::vector<uint8_t>& header);  // SDK xboxkrnl_io.cpp
}  // namespace rex::kernel::xboxkrnl

REXCVAR_DEFINE_BOOL(tekken_video_wmv3_native, true, "Tekken6/Video",
                    "Intro movies: decode the WMV3 frames with FFmpeg instead of the game's recompiled decoder (same "
                    "bytes, same picture buffers). Falls back to the game's decoder when FFmpeg has no WMV3");
REXCVAR_DEFINE_BOOL(tekken_video_wmv3_shadow, false, "Tekken6/Diag",
                    "Diagnostic: the game decodes and its planes are compared with FFmpeg's for the same bytes; the "
                    "luma of frame 30 of each movie is saved as PGM");
REXCVAR_DEFINE_BOOL(tekken_video_wmv3_diag, false, "Tekken6/Diag",
                    "Diagnostic: logs the WMV3 decoder's data calls and the first frames of each movie");

REX_EXTERN(__imp__sub_825EA6F8);
REX_EXTERN(__imp__sub_825D6328);
REX_EXTERN(__imp__sub_825CF600);
REX_EXTERN(__imp__sub_827080C8);
REX_EXTERN(__imp__sub_825D73A0);
REX_EXTERN(__imp__sub_82608D20);
REX_EXTERN(__imp__sub_82628670);

namespace tekken::video_wmv3 {
namespace {

using GuestFn = void (*)(PPCContext&, uint8_t*);

constexpr size_t kMaxFrame = 8 * 1024 * 1024;
constexpr uint32_t kDecodeI1 = 0x827080C8;  // [ctx+15804], variant 1
constexpr uint32_t kDecodeI2 = 0x825D73A0;  // [ctx+15804], variant 2
constexpr uint32_t kDecodeP1 = 0x82608D20;  // [ctx+15808], variant 1
constexpr uint32_t kDecodeP2 = 0x82628670;  // [ctx+15808], variant 2

// Decoder context fields
constexpr uint32_t kCtxReader = 80;
constexpr uint32_t kCtxStrideY = 204;
constexpr uint32_t kCtxStrideC = 208;
constexpr uint32_t kCtxRowsY = 212;
constexpr uint32_t kCtxRowsC = 216;
constexpr uint32_t kCtxOriginY = 220;
constexpr uint32_t kCtxOriginC = 224;
constexpr uint32_t kCtxType = 284;
constexpr uint32_t kCtxDataStruct = 3364;
constexpr uint32_t kCtxPlanes = 3744;  // Y, U, V
constexpr uint32_t kCtxLoopFilter = 3916;
constexpr uint32_t kCtxProfile = 15504;
constexpr uint32_t kCtxSlotI = 15804;
constexpr uint32_t kCtxSlotP = 15808;
constexpr uint32_t kCtxSlotB = 3080;
// Bit reader fields
constexpr uint32_t kReaderRemaining = 24;
constexpr uint32_t kReaderDataStruct = 44;

enum Type : int { kI = 0, kP = 1 };
const char* TypeName(int t) { return t == kI ? "I" : "P"; }

uint32_t Load32(const uint8_t* base, uint32_t va) {
  uint32_t v = 0;
  std::memcpy(&v, base + va, sizeof(v));
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t va, uint32_t value) {
  const uint32_t v = __builtin_bswap32(value);
  std::memcpy(base + va, &v, sizeof(v));
}

int64_t NowUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

bool HaveWmv3() {
  static const bool have = avcodec_find_decoder(AV_CODEC_ID_WMV3) != nullptr;
  return have;
}

bool Active() {
  return ((REXCVAR_GET(tekken_video_wmv3_native) || REXCVAR_GET(tekken_video_wmv3_shadow)) && HaveWmv3()) ||
         REXCVAR_GET(tekken_video_wmv3_diag);
}

// --- ASF header -------------------------------------------------------------------------------------------

struct Wmv3Info {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> sequence;  // extradata of the WMV3 stream (STRUCT_C, 4 bytes + padding)
};

constexpr uint8_t kGuidHeader[16] = {0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11,
                                     0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C};
constexpr uint8_t kGuidStream[16] = {0x91, 0x07, 0xDC, 0xB7, 0xB7, 0xA9, 0xCF, 0x11,
                                     0x8E, 0xE6, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65};
constexpr uint8_t kGuidVideo[16] = {0xC0, 0xEF, 0x19, 0xBC, 0x4D, 0x5B, 0xCF, 0x11,
                                    0xA8, 0xFD, 0x00, 0x80, 0x5F, 0x5C, 0x44, 0x2B};

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }
uint64_t Le64(const uint8_t* p) { return uint64_t(Le32(p)) | (uint64_t(Le32(p + 4)) << 32); }

// Stream properties object: 78 bytes, then the video type data (width, height, flags, format size: 11 bytes) and the
// BITMAPINFOHEADER, whose extra bytes are the WMV3 sequence header.
bool ParseAsf(const std::vector<uint8_t>& h, Wmv3Info& info) {
  if (h.size() < 30 || std::memcmp(h.data(), kGuidHeader, 16) != 0) {
    return false;
  }
  const uint64_t header_size = std::min<uint64_t>(Le64(&h[16]), h.size());
  size_t p = 30;
  while (p + 24 <= header_size) {
    const uint8_t* o = &h[p];
    const uint64_t size = Le64(o + 16);
    if (size < 24 || p + size > header_size) {
      break;
    }
    if (std::memcmp(o, kGuidStream, 16) == 0 && size >= 78 + 11 + 40 && std::memcmp(o + 24, kGuidVideo, 16) == 0) {
      const uint8_t* bmi = o + 78 + 11;
      const uint32_t bmi_size = Le32(bmi);
      if (std::memcmp(bmi + 16, "WMV3", 4) == 0) {
        info.width = int(Le32(bmi + 4));
        info.height = std::abs(int(Le32(bmi + 8)));
        info.sequence.clear();
        if (bmi_size > 40 && 78 + 11 + uint64_t(bmi_size) <= size) {
          info.sequence.assign(bmi + 40, bmi + bmi_size);
        }
        return info.width > 0 && info.height > 0 && info.sequence.size() >= 4;
      }
    }
    p += size_t(size);
  }
  return false;
}

// --- FFmpeg ----------------------------------------------------------------------------------------------

struct Picture {
  const uint8_t* planes[3] = {nullptr, nullptr, nullptr};
  int strides[3] = {0, 0, 0};
  int width = 0;
  int height = 0;
};

class Wmv3Decoder {
 public:
  ~Wmv3Decoder() { Close(); }

  bool Open(const Wmv3Info& info) {
    Close();
    const AVCodec* wmv3 = avcodec_find_decoder(AV_CODEC_ID_WMV3);
    if (!wmv3) {
      return false;
    }
    codec_ = avcodec_alloc_context3(wmv3);
    if (!codec_) {
      return false;
    }
    codec_->width = codec_->coded_width = info.width;
    codec_->height = codec_->coded_height = info.height;
    codec_->thread_count = 1;
    // Each frame comes out of the call that decodes it, without reordering (nfsmw-nx's notes).
    codec_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    codec_->extradata = static_cast<uint8_t*>(av_mallocz(info.sequence.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(codec_->extradata, info.sequence.data(), info.sequence.size());
    codec_->extradata_size = int(info.sequence.size());
    if (avcodec_open2(codec_, wmv3, nullptr) < 0) {
      REXLOG_ERROR("[video] WMV3 native: avcodec_open2 failed ({}x{})", info.width, info.height);
      Close();
      return false;
    }
    // With has_b_frames at 0, vc1dec.c keeps low_delay at 1 from the first frame (nfsmw-nx).
    codec_->has_b_frames = 0;
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    width_ = info.width;
    height_ = info.height;
    return pkt_ && frame_;
  }

  bool Decode(const uint8_t* data, size_t bytes, bool key, Picture& out) {
    if (!codec_ || !bytes) {
      return false;
    }
    av_packet_unref(pkt_);
    if (av_new_packet(pkt_, int(bytes)) < 0) {
      return false;
    }
    std::memcpy(pkt_->data, data, bytes);
    pkt_->flags = key ? AV_PKT_FLAG_KEY : 0;
    int r = avcodec_send_packet(codec_, pkt_);
    if (r < 0) {
      REXLOG_WARN("[video] WMV3 native: avcodec_send_packet {} ({} bytes)", r, bytes);
      return false;
    }
    r = avcodec_receive_frame(codec_, frame_);
    if (r < 0) {
      REXLOG_WARN("[video] WMV3 native: avcodec_receive_frame {} ({} bytes)", r, bytes);
      return false;
    }
    for (int i = 0; i < 3; ++i) {
      out.planes[i] = frame_->data[i];
      out.strides[i] = frame_->linesize[i];
    }
    out.width = frame_->width;
    out.height = frame_->height;
    return true;
  }

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  void Close() {
    if (frame_) av_frame_free(&frame_);
    if (pkt_) av_packet_free(&pkt_);
    if (codec_) avcodec_free_context(&codec_);
    width_ = height_ = 0;
  }

  AVCodecContext* codec_ = nullptr;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
  int width_ = 0;
  int height_ = 0;
};

// --- Compressed frame the game requests ------------------------------------------------------------------

struct Capture {
  uint32_t ctx = 0;
  uint32_t sections = 0;
  bool remaining = false;  // the last section says frame bytes remain
  bool error = false;
  bool seen = false;       // an I/P hook ran for this frame
  std::vector<uint8_t> data;
};
Capture g_capture;
std::mutex g_capture_m;
thread_local Capture* t_capture = nullptr;  // non-null inside sub_825D6328 on this thread
std::atomic<uint32_t> g_data_logged{0};

void NoteSection(const uint8_t* base, uint32_t lr, uint32_t structure, uint32_t p_data, uint32_t p_bytes,
                 uint32_t p_remaining, uint32_t result) {
  Capture& c = *t_capture;
  const uint32_t data = p_data ? Load32(base, p_data) : 0;
  const uint32_t bytes = p_bytes ? Load32(base, p_bytes) : 0;
  const uint32_t remaining = p_remaining ? Load32(base, p_remaining) : 0;
  c.remaining = remaining != 0;
  ++c.sections;
  if (bytes) {
    if (data && uint64_t(data) + bytes <= 0x100000000ull && c.data.size() + bytes <= kMaxFrame) {
      c.data.insert(c.data.end(), base + data, base + data + bytes);
    } else {
      c.error = true;
    }
  }
  if (REXCVAR_GET(tekken_video_wmv3_diag) && g_data_logged.fetch_add(1, std::memory_order_relaxed) < 80) {
    REXLOG_INFO("[video] data: lr={:08X} ctx={:08X} structure={:08X} -> result={:08X} data={:08X} bytes={} "
                "remaining={} | section {}, {} bytes",
                lr, c.ctx, structure, result, data, bytes, remaining, c.sections, c.data.size());
  }
}

// Asks the game's data function for the rest of the frame like the game's bit reader does (sub_825F87B0): structure
// [reader+44], offset 0, 4 bytes requested and "remaining" in [reader+24], where the tail of DecodeData looks for it.
void CompleteFrame(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  Capture& c = *t_capture;
  const uint32_t reader = Load32(base, obj + kCtxReader);
  const uint32_t structure = Load32(base, reader + kReaderDataStruct);
  const uint64_t r1 = ctx.r1.u64;
  const uint64_t lr = ctx.lr;
  const uint32_t sp = ctx.r1.u32 - 128;
  Store32(base, sp, ctx.r1.u32);  // stack back chain
  for (int i = 0; Load32(base, reader + kReaderRemaining) != 0 && !c.error && i < 4096; ++i) {
    const size_t before = c.data.size();
    Store32(base, sp + 80, 0);
    Store32(base, sp + 88, 0);
    ctx.r1.u64 = sp;
    ctx.r3.u64 = structure;
    ctx.r4.u64 = 0;
    ctx.r5.u64 = sp + 88;
    ctx.r6.u64 = 4;
    ctx.r7.u64 = sp + 80;
    ctx.r8.u64 = reader + kReaderRemaining;
    __imp__sub_825EA6F8(ctx, base);
    NoteSection(base, 0, structure, sp + 88, sp + 80, reader + kReaderRemaining, ctx.r3.u32);
    if (c.data.size() == before) {
      break;
    }
  }
  ctx.r1.u64 = r1;
  ctx.lr = lr;
}

// --- The game's picture planes -------------------------------------------------------------------------

struct Planes {
  uint8_t* y = nullptr;
  uint8_t* u = nullptr;
  uint8_t* v = nullptr;
  int stride_y = 0;
  int stride_c = 0;
};

bool GamePlanes(uint8_t* base, uint32_t obj, int width, int height, Planes& p) {
  const uint32_t stride_y = Load32(base, obj + kCtxStrideY);
  const uint32_t stride_c = Load32(base, obj + kCtxStrideC);
  const uint32_t rows_y = Load32(base, obj + kCtxRowsY);
  const uint32_t rows_c = Load32(base, obj + kCtxRowsC);
  const uint32_t origin_y = Load32(base, obj + kCtxOriginY);
  const uint32_t origin_c = Load32(base, obj + kCtxOriginC);
  const uint32_t wc = uint32_t(width + 1) / 2;
  const uint32_t hc = uint32_t(height + 1) / 2;
  if (width <= 0 || height <= 0 || stride_y < uint32_t(width) || stride_c < wc || stride_y > 8192 ||
      stride_c > 8192 || rows_y < uint32_t(height) || rows_c < hc || rows_y > 8192 || rows_c > 8192) {
    return false;
  }
  if (uint64_t(origin_y) + uint64_t(height - 1) * stride_y + uint32_t(width) > uint64_t(rows_y) * stride_y ||
      uint64_t(origin_c) + uint64_t(hc - 1) * stride_c + wc > uint64_t(rows_c) * stride_c) {
    return false;
  }
  const uint32_t y = Load32(base, obj + kCtxPlanes);
  const uint32_t u = Load32(base, obj + kCtxPlanes + 4);
  const uint32_t v = Load32(base, obj + kCtxPlanes + 8);
  if (!y || !u || !v) {
    return false;
  }
  p.y = base + y + origin_y;
  p.u = base + u + origin_c;
  p.v = base + v + origin_c;
  p.stride_y = int(stride_y);
  p.stride_c = int(stride_c);
  return true;
}

void CopyPlanes(const Picture& f, const Planes& g) {
  const int wc = (f.width + 1) / 2;
  const int hc = (f.height + 1) / 2;
  for (int y = 0; y < f.height; ++y) {
    std::memcpy(g.y + int64_t(y) * g.stride_y, f.planes[0] + int64_t(y) * f.strides[0], size_t(f.width));
  }
  for (int y = 0; y < hc; ++y) {
    std::memcpy(g.u + int64_t(y) * g.stride_c, f.planes[1] + int64_t(y) * f.strides[1], size_t(wc));
    std::memcpy(g.v + int64_t(y) * g.stride_c, f.planes[2] + int64_t(y) * f.strides[2], size_t(wc));
  }
}

struct Difference {
  int max = 0;
  double mean = 0.0;
  uint64_t bad = 0;  // pixels differing by more than 3
};

Difference ComparePlane(const uint8_t* g, int stride_g, const uint8_t* n, int stride_n, int width, int height) {
  Difference d;
  uint64_t sum = 0;
  for (int y = 0; y < height; ++y) {
    const uint8_t* rg = g + int64_t(y) * stride_g;
    const uint8_t* rn = n + int64_t(y) * stride_n;
    for (int x = 0; x < width; ++x) {
      const int v = std::abs(int(rg[x]) - int(rn[x]));
      sum += uint64_t(v);
      d.max = std::max(d.max, v);
      d.bad += v > 3;
    }
  }
  d.mean = double(sum) / double(std::max<int64_t>(int64_t(width) * height, 1));
  return d;
}

void SavePgm(const std::string& name, const uint8_t* plane, int stride, int width, int height,
             const uint8_t* minus = nullptr, int stride_minus = 0) {
  FILE* f = std::fopen(name.c_str(), "wb");
  if (!f) {
    return;
  }
  std::fprintf(f, "P5\n%d %d\n255\n", width, height);
  std::vector<uint8_t> row(static_cast<size_t>(width));
  for (int y = 0; y < height; ++y) {
    const uint8_t* p = plane + int64_t(y) * stride;
    if (minus) {
      const uint8_t* m = minus + int64_t(y) * stride_minus;
      for (int x = 0; x < width; ++x) {
        row[size_t(x)] = uint8_t(std::min(255, std::abs(int(p[x]) - int(m[x])) * 16));
      }
    } else {
      std::memcpy(row.data(), p, size_t(width));
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
}

// --- Movie being played --------------------------------------------------------------------------------

struct Movie {
  uint32_t obj = 0;
  std::string path;
  Wmv3Decoder dec;
  bool ready = false;   // FFmpeg open and the game's configuration known
  bool desync = true;   // FFmpeg is waiting for an I frame
  uint64_t frames = 0;  // I and P frames that went through the hooks
  uint64_t native = 0;
  uint64_t rejected = 0;
  uint64_t synced = 0;  // frames the game decoded outside the hooks, fed to FFmpeg afterwards
  uint64_t warnings = 0;
  int worst_max_y = 0;
  double worst_mean_y = 0.0;
  // Summary every 5 s
  int64_t since_us = 0;
  uint64_t n_game = 0;
  uint64_t n_native = 0;
  uint64_t n_shadow = 0;
  int64_t us_game = 0;
  int64_t us_native = 0;
};
std::mutex g_movie_m;
std::unique_ptr<Movie> g_movie;

void Summary(Movie& m, int64_t now) {
  if (m.since_us == 0) {
    m.since_us = now;
    return;
  }
  if (now - m.since_us < 5000000) {
    return;
  }
  const double s = double(now - m.since_us) / 1e6;
  REXLOG_INFO("[video] WMV3 '{}': {:.1f} frames/s | game {} at {:.2f} ms | FFmpeg {} at {:.2f} ms | shadow {}",
              m.path, double(m.n_game + m.n_native) / s, m.n_game,
              m.n_game ? double(m.us_game) / double(m.n_game) / 1000.0 : 0.0, m.n_native,
              m.n_native ? double(m.us_native) / double(m.n_native) / 1000.0 : 0.0, m.n_shadow);
  m.since_us = now;
  m.n_game = m.n_native = m.n_shadow = 0;
  m.us_game = m.us_native = 0;
}

void FinalSummary(const Movie& m) {
  REXLOG_INFO("[video] WMV3 end of '{}' (context {:08X}): {} frames, {} with FFmpeg, {} rejected, {} decoded by the "
              "game outside the hooks; shadow: worst Y max {} mean {:.3f}",
              m.path, m.obj, m.frames, m.native, m.rejected, m.synced, m.worst_max_y, m.worst_mean_y);
}

// With g_movie_m held.
Movie& MovieFor(uint8_t* base, uint32_t obj) {
  if (g_movie && g_movie->obj == obj) {
    return *g_movie;
  }
  if (g_movie) {
    FinalSummary(*g_movie);
  }
  g_movie = std::make_unique<Movie>();
  Movie& m = *g_movie;
  m.obj = obj;
  std::vector<uint8_t> header;
  Wmv3Info info;
  const bool info_ok = rex::kernel::xboxkrnl::TekkenLastAsfRead(m.path, header) && ParseAsf(header, info);
  const auto& s = info.sequence;
  // MAXBFRAMES: 3 bits of the sequence header (STRUCT_C byte 3, bits 6..4). B frames are left to the game.
  const bool with_b = info_ok && ((s[3] >> 4) & 7) != 0;
  const uint32_t filter = Load32(base, obj + kCtxLoopFilter);
  const uint32_t slot_i = Load32(base, obj + kCtxSlotI);
  const uint32_t slot_p = Load32(base, obj + kCtxSlotP);
  const bool config_ok = filter <= 1 && Load32(base, obj + kCtxProfile) == 6 && !with_b &&
                         (slot_i == kDecodeI1 || slot_i == kDecodeI2) && (slot_p == kDecodeP1 || slot_p == kDecodeP2);
  Planes g;
  const bool planes_ok = info_ok && GamePlanes(base, obj, info.width, info.height, g);
  m.ready = info_ok && config_ok && planes_ok && m.dec.Open(info);
  REXLOG_INFO("[video] WMV3: context {:08X} -> '{}' {}x{} sequence {:02X}{:02X}{:02X}{:02X}; loop filter {}, B frames "
              "{}; configuration {} (I {:08X} P {:08X} B {:08X} profile {}); planes {} (stride {}/{} rows {}/{} origin "
              "{}/{}); FFmpeg {}",
              obj, m.path, info.width, info.height, s.size() > 0 ? s[0] : 0, s.size() > 1 ? s[1] : 0,
              s.size() > 2 ? s[2] : 0, s.size() > 3 ? s[3] : 0, filter, with_b ? "yes" : "no",
              config_ok ? "known" : "different", slot_i, slot_p, Load32(base, obj + kCtxSlotB),
              Load32(base, obj + kCtxProfile), planes_ok ? "ok" : "WRONG", Load32(base, obj + kCtxStrideY),
              Load32(base, obj + kCtxStrideC), Load32(base, obj + kCtxRowsY), Load32(base, obj + kCtxRowsC),
              Load32(base, obj + kCtxOriginY), Load32(base, obj + kCtxOriginC),
              m.ready ? "ready" : (HaveWmv3() ? "not used" : "has no WMV3 decoder"));
  return m;
}

// The context as the game's I and P functions leave it when they finish.
void LeaveContext(uint8_t* base, uint32_t obj, int type) {
  if (type == kP) {
    const bool flag = Load32(base, obj + kCtxLoopFilter) != 0 || Load32(base, obj + 14856) != 0 ||
                      Load32(base, obj + 15228) != 0xFFFFFFFFu;
    Store32(base, obj + 15592, flag ? 1 : 0);
    Store32(base, obj + 15568, 1);
  }
  Store32(base, obj + 15596, 0);
}

// A frame FFmpeg does not replace: the game decodes it, and FFmpeg waits for the next I frame.
bool Reject(Movie& m, int type, const char* reason) {
  ++m.rejected;
  m.desync = true;
  if (m.warnings++ < 10) {
    REXLOG_WARN("[video] WMV3 native: frame {} ({}) of '{}' decoded by the game: {}", m.frames, TypeName(type),
                m.path, reason);
  }
  return false;
}

// Replaces the decoding of one frame. true if r3 already holds the result.
bool ReplaceFrame(PPCContext& ctx, uint8_t* base, Movie& m, Capture& c, int type) {
  const bool intra = type == kI;
  const uint32_t obj = m.obj;
  if (m.desync && !intra) {
    ++m.rejected;  // until the next I frame the game decodes (FFmpeg lacks this frame's reference)
    return false;
  }
  if (Load32(base, obj + kCtxProfile) != 6) {
    m.ready = false;
    return Reject(m, type, "different profile");
  }
  if (c.remaining || Load32(base, Load32(base, obj + kCtxReader) + kReaderRemaining) != 0) {
    CompleteFrame(ctx, base, obj);
  }
  if (c.remaining || c.error || c.data.empty()) {
    return Reject(m, type, "incomplete compressed frame");
  }
  const int64_t t0 = NowUs();
  Picture f;
  if (!m.dec.Decode(c.data.data(), c.data.size(), intra, f)) {
    return Reject(m, type, "FFmpeg does not decode it");
  }
  Planes g;
  if (f.width != m.dec.width() || f.height != m.dec.height() || !GamePlanes(base, obj, f.width, f.height, g)) {
    m.ready = false;
    return Reject(m, type, "the game's planes do not match the video");
  }
  CopyPlanes(f, g);
  LeaveContext(base, obj, type);
  ctx.r3.u64 = 0;
  const int64_t t1 = NowUs();
  m.desync = false;
  ++m.frames;
  ++m.native;
  ++m.n_native;
  m.us_native += t1 - t0;
  Summary(m, t1);
  return true;
}

// After the game decodes: FFmpeg decodes the same bytes and the planes are compared.
void Shadow(uint8_t* base, Movie& m, const Capture& c, int type) {
  const bool intra = type == kI;
  const uint64_t n = m.frames - 1;
  if (c.remaining || c.error || c.data.empty()) {
    if (m.warnings++ < 10) {
      REXLOG_WARN("[video] shadow #{}: incomplete compressed frame ({} bytes, remaining {}, error {})", n,
                  c.data.size(), c.remaining, c.error);
    }
    m.desync = true;
    return;
  }
  if (m.desync && !intra) {
    return;
  }
  const int64_t t0 = NowUs();
  Picture f;
  if (!m.dec.Decode(c.data.data(), c.data.size(), intra, f)) {
    m.desync = true;
    return;
  }
  const int64_t t1 = NowUs();
  m.desync = false;
  ++m.n_shadow;
  Planes g;
  if (!GamePlanes(base, m.obj, f.width, f.height, g)) {
    return;
  }
  const int wc = (f.width + 1) / 2;
  const int hc = (f.height + 1) / 2;
  const Difference dy = ComparePlane(g.y, g.stride_y, f.planes[0], f.strides[0], f.width, f.height);
  const Difference du = ComparePlane(g.u, g.stride_c, f.planes[1], f.strides[1], wc, hc);
  const Difference dv = ComparePlane(g.v, g.stride_c, f.planes[2], f.strides[2], wc, hc);
  m.worst_max_y = std::max(m.worst_max_y, dy.max);
  m.worst_mean_y = std::max(m.worst_mean_y, dy.mean);
  const bool odd = dy.max > 8 || du.max > 8 || dv.max > 8;
  if (n < 4 || n % 90 == 0 || (odd && m.warnings++ < 20)) {
    REXLOG_INFO("[video] shadow #{} {} {} bytes in {} sections: Y max {} mean {:.3f} >3 {} | U max {} mean {:.3f} | "
                "V max {} mean {:.3f} | FFmpeg {:.2f} ms",
                n, TypeName(type), c.data.size(), c.sections, dy.max, dy.mean, dy.bad, du.max, du.mean, dv.max,
                dv.mean, double(t1 - t0) / 1000.0);
  }
  if (n == 30) {
    const std::string prefix = fmt::format("wmv3_{:08X}_030", m.obj);
    SavePgm(prefix + "_game.pgm", g.y, g.stride_y, f.width, f.height);
    SavePgm(prefix + "_ffmpeg.pgm", f.planes[0], f.strides[0], f.width, f.height);
    SavePgm(prefix + "_difference.pgm", g.y, g.stride_y, f.width, f.height, f.planes[0], f.strides[0]);
  }
}

void Decode(PPCContext& ctx, uint8_t* base, int type, GuestFn original) {
  const bool native = REXCVAR_GET(tekken_video_wmv3_native) && HaveWmv3();
  const bool shadow = REXCVAR_GET(tekken_video_wmv3_shadow) && HaveWmv3();
  const bool diag = REXCVAR_GET(tekken_video_wmv3_diag);
  if (!native && !shadow && !diag) {
    original(ctx, base);
    return;
  }
  const uint32_t obj = ctx.r3.u32;
  std::lock_guard<std::mutex> lock(g_movie_m);
  Movie& m = MovieFor(base, obj);
  Capture* c = t_capture && t_capture->ctx == obj ? t_capture : nullptr;
  if (c) {
    c->seen = true;
  }
  if (native && m.ready) {
    if (c && ReplaceFrame(ctx, base, m, *c, type)) {
      return;
    }
    if (!c) {
      Reject(m, type, "call outside sub_825D6328");
    }
  }
  const int64_t t0 = NowUs();
  original(ctx, base);
  const int64_t t1 = NowUs();
  ++m.frames;
  ++m.n_game;
  m.us_game += t1 - t0;
  if (diag && m.frames <= 12 && c) {
    REXLOG_INFO("[video] frame #{} {}: {} bytes in {} sections, remaining {}, error {}, result {:08X}, game {:.2f} ms",
                m.frames - 1, TypeName(type), c->data.size(), c->sections, c->remaining, c->error, ctx.r3.u32,
                double(t1 - t0) / 1000.0);
  }
  if (shadow && !native && m.dec.width() > 0 && c && ctx.r3.u32 == 0) {
    Shadow(base, m, *c, type);
  }
  Summary(m, t1);
}

// A frame the game decoded outside the I/P hooks (X8 intra, ...): FFmpeg decodes the same bytes to keep its
// references, so the next frames can still be replaced.
void SyncAfterGame(uint8_t* base, const Capture& c) {
  std::lock_guard<std::mutex> lock(g_movie_m);
  if (!g_movie || g_movie->obj != c.ctx || g_movie->dec.width() <= 0) {
    return;
  }
  Movie& m = *g_movie;
  const uint32_t type = Load32(base, c.ctx + kCtxType);
  const bool intra = type == 0;
  if ((m.desync && !intra) || c.remaining || c.error || c.data.empty()) {
    return;
  }
  Picture f;
  m.desync = !m.dec.Decode(c.data.data(), c.data.size(), intra, f);
  ++m.synced;
  if (m.warnings++ < 10) {
    REXLOG_INFO("[video] WMV3: frame of type {} of '{}' decoded by the game outside the hooks; FFmpeg {}", type,
                m.path, m.desync ? "failed (waits for an I frame)" : "follows");
  }
}

}  // namespace
}  // namespace tekken::video_wmv3

// The decoder's data thunk (jumps to [[r3+4]] with r3 = [r3]).
REX_HOOK_RAW(sub_825EA6F8) {
  using namespace tekken::video_wmv3;
  if (!t_capture) {
    __imp__sub_825EA6F8(ctx, base);
    return;
  }
  const uint32_t obj = t_capture->ctx;
  const uint32_t reader = Load32(base, obj + kCtxReader);
  const uint32_t structure = ctx.r3.u32;
  if (structure != Load32(base, obj + kCtxDataStruct) &&
      (!reader || structure != Load32(base, reader + kReaderDataStruct))) {
    __imp__sub_825EA6F8(ctx, base);
    return;
  }
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t p_data = ctx.r5.u32;
  const uint32_t p_bytes = ctx.r7.u32;
  const uint32_t p_remaining = ctx.r8.u32;
  __imp__sub_825EA6F8(ctx, base);
  NoteSection(base, lr, structure, p_data, p_bytes, p_remaining, ctx.r3.u32);
}

// One frame: requests the first chunk and calls DecodeData (sub_825D2B30); collects the chunks of the compressed
// frame.
REX_HOOK_RAW(sub_825D6328) {
  using namespace tekken::video_wmv3;
  if (!ctx.r3.u32 || !Active() || t_capture) {
    __imp__sub_825D6328(ctx, base);
    return;
  }
  std::unique_lock<std::mutex> lock(g_capture_m, std::try_to_lock);
  if (!lock.owns_lock()) {
    __imp__sub_825D6328(ctx, base);
    return;
  }
  g_capture.ctx = ctx.r3.u32;
  g_capture.sections = 0;
  g_capture.remaining = false;
  g_capture.error = false;
  g_capture.seen = false;
  g_capture.data.clear();
  t_capture = &g_capture;
  __imp__sub_825D6328(ctx, base);
  t_capture = nullptr;
  if (!g_capture.seen && !g_capture.data.empty() && HaveWmv3() &&
      (REXCVAR_GET(tekken_video_wmv3_native) || REXCVAR_GET(tekken_video_wmv3_shadow))) {
    SyncAfterGame(base, g_capture);
  }
}

// I frames ([ctx+15804]), variants 1 and 2.
REX_HOOK_RAW(sub_827080C8) {
  tekken::video_wmv3::Decode(ctx, base, tekken::video_wmv3::kI, __imp__sub_827080C8);
}
REX_HOOK_RAW(sub_825D73A0) {
  tekken::video_wmv3::Decode(ctx, base, tekken::video_wmv3::kI, __imp__sub_825D73A0);
}

// P frames ([ctx+15808]), variants 1 and 2.
REX_HOOK_RAW(sub_82608D20) {
  tekken::video_wmv3::Decode(ctx, base, tekken::video_wmv3::kP, __imp__sub_82608D20);
}
REX_HOOK_RAW(sub_82628670) {
  tekken::video_wmv3::Decode(ctx, base, tekken::video_wmv3::kP, __imp__sub_82628670);
}

// Preparation of a movie's context: if the context is reused, FFmpeg starts from scratch.
REX_HOOK_RAW(sub_825CF600) {
  using namespace tekken::video_wmv3;
  if (Active()) {
    std::lock_guard<std::mutex> lock(g_movie_m);
    if (g_movie && g_movie->obj == ctx.r3.u32) {
      FinalSummary(*g_movie);
      g_movie.reset();
    }
    if (REXCVAR_GET(tekken_video_wmv3_diag)) {
      REXLOG_INFO("[video] sub_825CF600: ctx={:08X} data structure={:08X} lr={:08X}", ctx.r3.u32, ctx.r4.u32,
                  static_cast<uint32_t>(ctx.lr));
    }
  }
  __imp__sub_825CF600(ctx, base);
}
