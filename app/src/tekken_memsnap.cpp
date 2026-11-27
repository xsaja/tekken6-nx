// tekken6-nx (2026-11-24): guest memory snapshots on the PC build, to find where the game keeps a value by comparing
// snapshots taken in different states (the Arcade stage number, to start Arcade at Azazel's fight; the motion blur
// option, to find the draws it adds). P (the key) writes memsnap/snap_<N>.bin next to the executable and beeps: every committed
// guest page that is not all zeros, as records of (u32 guest address, u32 bytes, bytes), the virtual heaps and the
// physical memory once (through its 0xA0000000 view). tools/memsnap_diff.py compares them.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

REXCVAR_DEFINE_BOOL(tekken_memsnap, true, "Tekken",
                    "PC build: the P key writes a snapshot of guest memory to memsnap/ next to the executable (for "
                    "tools/memsnap_diff.py)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(tekken_poke, "", "Tekken",
                      "PC build: what the O key writes into guest memory, \"<hex address>:<bytes>=<value>\" items "
                      "(big-endian), comma-separated; empty = nothing. (2026-11-24: 82C7F16C / 82C7F188 / 82C7BBF2, which count "
                      "1, 3, 4 over Arcade stages 1, 3, 4, are NOT the stage: set to 8 they broke the next stage's loading)");


// tekken6-nx (2026-11-25, motion blur): counts the game's motion blur pass (sub_8221CC80: a velocity pass that redraws
// the characters, then two blend passes; called by the post-process chain sub_8221DD08 when sub_8242BB78 says so; the
// option is the u32 at 0x8297E3D4, 0 = on). The PC build logs the count every 5 s: zero during a fight with the option
// on means the game does not ask for it (its gating), non-zero means Renut draws it wrong.
namespace tekken {
std::atomic<uint32_t> g_blur_passes{0};
}
REX_EXTERN(__imp__sub_8221CC80);
REX_HOOK_RAW(sub_8221CC80) {
  tekken::g_blur_passes.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_8221CC80(ctx, base);
}

// tekken6-nx (2026-11-25, Switch motion blur): the Switch's resolve trace never shows the blur pass, the PC's shows it
// every fight frame, so the game's gate sub_8242BB78 says no on the Switch. With tekken_blur_gate_log the hook runs
// the gate's checks itself before the gate (they are queries: the camera object's sub_8221ED10 state, sub_8235D570's
// scan of ids 2359..2410, the option at 0x8297E3D4, sub_821C6570 on sub_82170CB0()+888) and counts which one said
// no; every 5 s a [blur-gate] line on stderr (both platforms). Its native copy (bulk) was dropped for this hook.
REXCVAR_DEFINE_BOOL(tekken_blur_gate_log, false, "Tekken6/Diagnostics",
                    "Every 5 s, [blur-gate] on stderr: how often the motion blur gate sub_8242BB78 ran, said yes, "
                    "and which of its checks said no")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// tekken6-nx (2026-11-25): what the gate's last check holds is the game's own performance governor. sub_82219B70 keeps
// the last frame's CPU time (+124) and GPU time (+136, sub_82515980) of the object behind the handle at 0x8273593C
// as percentages of a frame, and sub_821C6738 sets item+88 only while both are below 80 % (95 % in some states). The
// console runs them at 60-145 % (the PC at 30-70 %), so the game drops the blur in most fight frames and it shows only
// now and then. With tekken_blur_always the game's menu option decides alone: when the option is on and only that
// flag says no, the gate says yes (about 3 ms a frame on the console: ~50 fps in Practice, 2026-11-25). Off by
// default (2026-11-25): the game's own rule keeps 60 fps, and the share of frames with blur in the [blur]
// lines grows by itself as the CPU time comes down.
REXCVAR_DEFINE_BOOL(tekken_blur_always, false, "Tekken6",
                    "Motion blur: draw it whenever the game's option is on (true), or only when the game thinks it "
                    "has time to spare, as on the Xbox 360 (false; on the Switch it then shows only now and then)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REX_EXTERN(__imp__sub_8242BB78);
REX_EXTERN(sub_8221ED10);
REX_EXTERN(sub_8235D570);
REX_EXTERN(sub_82170CB0);
REX_EXTERN(sub_821A8FF0);
REX_EXTERN(sub_821C6570);
namespace tekken {
void LogBlurGate();
std::atomic<uint32_t> g_gate_calls{0}, g_gate_yes{0}, g_gate_no_object{0}, g_gate_no_camera{0}, g_gate_no_scan{0},
    g_gate_no_option{0}, g_gate_no_last{0}, g_gate_mismatch{0}, g_gate_camera_value{0}, g_gate_option_value{0};
std::atomic<uint32_t> g_last_no_item{0}, g_last_no_84{0}, g_last_no_180{0}, g_last_no_88{0};
std::atomic<uint32_t> g_last_seen_no[4], g_last_seen_yes[4];
std::mutex g_float_mutex;
float g_float_threshold = 0, g_float_a_min = 0, g_float_a_max = 0, g_float_b_min = 0, g_float_b_max = 0;
uint32_t g_float_other = 0, g_float_n = 0, g_float_a_over = 0, g_float_b_over = 0, g_float_nan = 0;
}
namespace {
uint32_t GateLoad(uint8_t* base, uint32_t va) {
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return __builtin_bswap32(v);
}
}  // namespace
namespace tekken {
std::atomic<uint32_t> g_blur_frames{0}, g_blur_drawn{0}, g_blur_forced{0};
std::atomic<uint32_t> g_blur_option{0xFFFFFFFFu};

// The gate's checks before the performance flag: the camera state, the scan of ids 2359..2410, the option, and the
// item's +84 / +180 (sub_821A8FF0 / sub_821C6570). True when they all pass and only item+88 (the governor) is 0.
bool OnlyGovernorSaysNo(PPCContext& ctx, uint8_t* base) {
  uint32_t object = GateLoad(base, 0x82735C70u + 16);
  if (!(object && GateLoad(base, object + 12) == GateLoad(base, 0x82735C70u + 20) &&
        (GateLoad(base, object + 60) & GateLoad(base, 0x82007560u))))
    object = 0;
  ctx.r3.u64 = GateLoad(base, object + 884);
  sub_8221ED10(ctx, base);
  if (int8_t(ctx.r3.u32 & 0xFF)) return false;
  sub_8235D570(ctx, base);
  if (ctx.r3.u32) return false;
  const uint32_t option = GateLoad(base, 0x8297E3D4u);
  if (option == 1 || option >= 3) return false;
  const uint32_t holder = GateLoad(base, object + 888);  // sub_82170CB0() is the same object
  const uint32_t item = holder ? GateLoad(base, holder + 584) : 0;
  if (!(item && GateLoad(base, item + 12) == GateLoad(base, holder + 588) &&
        (GateLoad(base, item + 60) & GateLoad(base, 0x82007560u))))
    return false;
  return GateLoad(base, item + 84) && GateLoad(base, item + 180) && !GateLoad(base, item + 88);
}

// Always on (one line every 10 s and one when the menu option changes), so measurements and logs from other people
// say whether the blur was being drawn: [blur] on stderr (rex_stderr.log on the console) and in the PC log.
void BlurSummary(uint8_t* base) {
  const uint32_t option = GateLoad(base, 0x8297E3D4u);
  const uint32_t previous = g_blur_option.exchange(option, std::memory_order_relaxed);
  if (previous != option) {
    std::fprintf(stderr, "[blur] game option: motion blur %s (value %u)%s\n", option == 0 ? "ON" : "OFF", option,
                 option == 0 && REXCVAR_GET(tekken_blur_always) ? ", drawn every frame the game allows it (tekken_blur_always)" : "");
    REXLOG_INFO("[blur] game option: motion blur {} (value {})", option == 0 ? "ON" : "OFF", option);
  }
  static std::atomic<int64_t> next{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  int64_t due = next.load(std::memory_order_relaxed);
  if (due == 0) {
    next.compare_exchange_strong(due, now + 10000, std::memory_order_relaxed);
    return;
  }
  if (now < due || !next.compare_exchange_strong(due, now + 10000, std::memory_order_relaxed)) return;
  const uint32_t frames = g_blur_frames.exchange(0, std::memory_order_relaxed);
  const uint32_t drawn = g_blur_drawn.exchange(0, std::memory_order_relaxed);
  const uint32_t forced = g_blur_forced.exchange(0, std::memory_order_relaxed);
  std::fprintf(stderr,
               "[blur] 10 s: option %s, motion blur drawn in %u of %u frames (%u of them past the game's performance "
               "limit, tekken_blur_always %s)\n",
               option == 0 ? "ON" : "OFF", drawn, frames, forced, REXCVAR_GET(tekken_blur_always) ? "on" : "off");
  REXLOG_INFO("[blur] 10 s: option {}, motion blur drawn in {} of {} frames ({} past the game's performance limit)",
              option == 0 ? "ON" : "OFF", drawn, frames, forced);
}
}  // namespace tekken

REX_HOOK_RAW(sub_8242BB78) {
  static const bool on = REXCVAR_GET(tekken_blur_gate_log);
  static const bool always = REXCVAR_GET(tekken_blur_always);
  if (!on) {
    __imp__sub_8242BB78(ctx, base);
    bool forced = false;
    if (always && !ctx.r3.u32 && tekken::OnlyGovernorSaysNo(ctx, base)) forced = true;
    ctx.r3.u64 = forced ? 1 : ctx.r3.u64;
    tekken::g_blur_frames.fetch_add(1, std::memory_order_relaxed);
    if (ctx.r3.u32) tekken::g_blur_drawn.fetch_add(1, std::memory_order_relaxed);
    if (forced) tekken::g_blur_forced.fetch_add(1, std::memory_order_relaxed);
    tekken::BlurSummary(base);
    return;
  }
  using namespace tekken;
  // The gate's own steps (renut_recomp.90.cpp): the object at [0x82735C70 + 16] when its +12 matches +20 and its
  // +60 has a bit of the mask at 0x82007560, else 0.
  int verdict = 0;  // 0 yes, 1 camera, 2 scan, 3 option, 4 last
  uint32_t object = GateLoad(base, 0x82735C70u + 16);
  const bool object_ok = object && GateLoad(base, object + 12) == GateLoad(base, 0x82735C70u + 20) &&
                         (GateLoad(base, object + 60) & GateLoad(base, 0x82007560u));
  if (!object_ok) {
    g_gate_no_object.fetch_add(1, std::memory_order_relaxed);
    object = 0;
  }
  ctx.r3.u64 = GateLoad(base, object + 884);
  sub_8221ED10(ctx, base);
  const int8_t camera = int8_t(ctx.r3.u32 & 0xFF);
  g_gate_camera_value.store(ctx.r3.u32, std::memory_order_relaxed);
  if (camera) verdict = 1;
  if (!verdict) {
    sub_8235D570(ctx, base);
    if (ctx.r3.u32) verdict = 2;
  }
  if (!verdict) {
    const uint32_t option = GateLoad(base, 0x8297E3D4u);
    g_gate_option_value.store(option, std::memory_order_relaxed);
    if (option == 1 || option >= 3) verdict = 3;
  }
  if (!verdict) {
    sub_82170CB0(ctx, base);
    const uint32_t holder = GateLoad(base, ctx.r3.u32 + 888);
    ctx.r3.u64 = holder;
    sub_821A8FF0(ctx, base);
    sub_821C6570(ctx, base);
    if (!ctx.r3.u32) verdict = 4;
    // Which link of the last check (sub_821A8FF0: the handle at holder+584 checked against holder+588 and the
    // mask; sub_821C6570: its +84 and +180 non-zero, the answer its +88).
    const uint32_t item = holder ? GateLoad(base, holder + 584) : 0;
    const bool item_ok = item && GateLoad(base, item + 12) == GateLoad(base, holder + 588) &&
                         (GateLoad(base, item + 60) & GateLoad(base, 0x82007560u));
    const uint32_t f84 = item_ok ? GateLoad(base, item + 84) : 0, f180 = item_ok ? GateLoad(base, item + 180) : 0,
                   f88 = item_ok ? GateLoad(base, item + 88) : 0;
    if (!item_ok) g_last_no_item.fetch_add(1, std::memory_order_relaxed);
    else if (!f84) g_last_no_84.fetch_add(1, std::memory_order_relaxed);
    else if (!f180) g_last_no_180.fetch_add(1, std::memory_order_relaxed);
    else if (!f88) g_last_no_88.fetch_add(1, std::memory_order_relaxed);
    // sub_821C6738 sets item+88 when both floats +124 and +136 of the object behind the handle at 0x8273593C are
    // below the threshold at 0x82003E8C (item+92 set) or 0x82003E88.
    {
      uint32_t other = GateLoad(base, 0x8273593Cu + 16);
      if (!(other && GateLoad(base, other + 12) == GateLoad(base, 0x8273593Cu + 20) &&
            (GateLoad(base, other + 60) & GateLoad(base, 0x82007560u))))
        other = 0;
      auto as_float = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
      const float a = other ? as_float(GateLoad(base, other + 124)) : -1.0f;
      const float b = other ? as_float(GateLoad(base, other + 136)) : -1.0f;
      const uint32_t f92 = item_ok ? GateLoad(base, item + 92) : 0;
      const float threshold = as_float(GateLoad(base, f92 ? 0x82003E8Cu : 0x82003E88u));
      std::lock_guard lock(g_float_mutex);
      g_float_threshold = threshold;
      g_float_other = other;
      g_float_n++;
      if (!(a < threshold)) g_float_a_over++;
      if (!(b < threshold)) g_float_b_over++;
      if (a != a || b != b) g_float_nan++;
      if (g_float_n == 1 || a < g_float_a_min) g_float_a_min = a;
      if (g_float_n == 1 || a > g_float_a_max) g_float_a_max = a;
      if (g_float_n == 1 || b < g_float_b_min) g_float_b_min = b;
      if (g_float_n == 1 || b > g_float_b_max) g_float_b_max = b;
    }
    auto& slot = verdict == 4 ? g_last_seen_no : g_last_seen_yes;
    slot[0].store(item, std::memory_order_relaxed);
    slot[1].store(f84, std::memory_order_relaxed);
    slot[2].store(f180, std::memory_order_relaxed);
    slot[3].store(f88, std::memory_order_relaxed);
  }
  __imp__sub_8242BB78(ctx, base);
  const bool governor_only = verdict == 4 && !ctx.r3.u32 && OnlyGovernorSaysNo(ctx, base);
  if (always && governor_only) ctx.r3.u64 = 1;
  const bool yes = ctx.r3.u32 != 0;
  g_blur_frames.fetch_add(1, std::memory_order_relaxed);
  if (yes) g_blur_drawn.fetch_add(1, std::memory_order_relaxed);
  if (always && governor_only) g_blur_forced.fetch_add(1, std::memory_order_relaxed);
  BlurSummary(base);
  g_gate_calls.fetch_add(1, std::memory_order_relaxed);
  if (yes) g_gate_yes.fetch_add(1, std::memory_order_relaxed);
  if (yes != (verdict == 0)) g_gate_mismatch.fetch_add(1, std::memory_order_relaxed);
  switch (verdict) {
    case 1: g_gate_no_camera.fetch_add(1, std::memory_order_relaxed); break;
    case 2: g_gate_no_scan.fetch_add(1, std::memory_order_relaxed); break;
    case 3: g_gate_no_option.fetch_add(1, std::memory_order_relaxed); break;
    case 4: g_gate_no_last.fetch_add(1, std::memory_order_relaxed); break;
    default: break;
  }
  // The report from the hook itself (no thread of its own: std::thread at startup ends the Switch build).
  static std::atomic<int64_t> next_report{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t due = next_report.load(std::memory_order_relaxed);
  if (due == 0) {
    next_report.compare_exchange_strong(due, now + 5000, std::memory_order_relaxed);
  } else if (now >= due && next_report.compare_exchange_strong(due, now + 5000, std::memory_order_relaxed)) {
    tekken::LogBlurGate();
  }
}

namespace tekken {

namespace {
#if defined(_WIN32)
bool PageHasData(const uint8_t* p) {
  const uint64_t* w = reinterpret_cast<const uint64_t*>(p);
  for (size_t i = 0; i < 4096 / 8; ++i)
    if (w[i])
      return true;
  return false;
}

void WriteSnapshot(int n) {
  auto* memory = rex::system::kernel_memory();
  if (!memory)
    return;
  const auto dir = rex::filesystem::GetExecutableFolder() / "memsnap";
  std::filesystem::create_directories(dir);
  char name[64];
  std::snprintf(name, sizeof(name), "snap_%02d.bin", n);
  const auto path = dir / name;
  FILE* f = _wfopen(path.wstring().c_str(), L"wb");
  if (!f)
    return;
  uint64_t pages = 0;
  struct Range {
    uint32_t begin, end;
  };
  // The virtual heaps, then the physical memory once (0xC0000000 / 0xE0000000 are other views of it).
  static const Range kRanges[] = {{0x00010000u, 0x40000000u}, {0x40000000u, 0x80000000u},
                                  {0x80000000u, 0xA0000000u}, {0xA0000000u, 0xC0000000u}};
  for (const Range& r : kRanges) {
    uint64_t address = r.begin;
    while (address < r.end) {
      auto* heap = memory->LookupHeap(uint32_t(address));
      rex::memory::HeapAllocationInfo info{};
      if (!heap || !heap->QueryRegionInfo(uint32_t(address), &info) || !info.region_size) {
        address += 0x10000;
        continue;
      }
      const uint64_t region_end = std::min<uint64_t>(uint64_t(info.base_address) + info.region_size, r.end);
      if ((info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & rex::memory::kMemoryProtectRead)) {
        for (uint64_t a = std::max<uint64_t>(address, info.base_address) & ~0xFFFull; a < region_end; a += 4096) {
          const uint8_t* p = memory->TranslateVirtual<const uint8_t*>(uint32_t(a));
          if (!p || !PageHasData(p))
            continue;
          const uint32_t header[2] = {uint32_t(a), 4096u};
          std::fwrite(header, sizeof(header), 1, f);
          std::fwrite(p, 4096, 1, f);
          ++pages;
        }
      }
      address = std::max<uint64_t>(region_end, address + 4096);
    }
  }
  std::fclose(f);
  REXLOG_INFO("[memsnap] {} written: {} pages ({:.1f} MB)", path.string(), pages, pages * 4096.0 / 1048576.0);
  std::fprintf(stderr, "[memsnap] snap_%02d.bin: %llu pages\n", n, static_cast<unsigned long long>(pages));
}

// tekken6-nx (2026-11-24): the O key writes tekken_poke's values into guest memory ("<hex address>:<bytes>=<value>"
// items, big-endian, comma-separated): first use, the Arcade stage number found by the snapshots, to jump to Azazel.
void Poke() {
  auto* memory = rex::system::kernel_memory();
  if (!memory)
    return;
  const std::string list = REXCVAR_GET(tekken_poke);
  size_t at = 0;
  while (at < list.size()) {
    size_t end = list.find(',', at);
    if (end == std::string::npos)
      end = list.size();
    const std::string item = list.substr(at, end - at);
    at = end + 1;
    unsigned address = 0, bytes = 0;
    unsigned long long value = 0;
    if (std::sscanf(item.c_str(), "%x:%u=%llu", &address, &bytes, &value) != 3 || (bytes != 1 && bytes != 2 && bytes != 4))
      continue;
    uint8_t* p = memory->TranslateVirtual<uint8_t*>(address);
    if (!p)
      continue;
    for (unsigned i = 0; i < bytes; ++i)
      p[i] = uint8_t(value >> (8 * (bytes - 1 - i)));
    REXLOG_INFO("[poke] {:08X} ({} bytes) = {}", address, bytes, value);
    std::fprintf(stderr, "[poke] %08X (%u bytes) = %llu\n", address, bytes, value);
  }
}

// Only while the game's window is in front (a key typed in another program does nothing).
bool GameInFront() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
}

// tekken6-nx (2026-11-24): the C key asks RenderDoc (when the game runs under it, CAPTURAR_AZAZEL_RENDERDOC.bat) for a
// capture of the next frame, through its in-application API (RENDERDOC_API_1_1_2: GetAPIVersion is entry 0 and
// TriggerCapture entry 15 of the function table; renderdoc_app.h), for keyboards without an easy F12.
void RenderDocCapture() {
  HMODULE rd = GetModuleHandleA("renderdoc.dll");
  if (!rd)
    return;
  using GetApi = int(__cdecl*)(int version, void** out);
  auto get = reinterpret_cast<GetApi>(GetProcAddress(rd, "RENDERDOC_GetAPI"));
  void** api = nullptr;
  if (!get || !get(10102, reinterpret_cast<void**>(&api)) || !api)
    return;
  int major = 0, minor = 0, patch = 0;
  reinterpret_cast<void(__cdecl*)(int*, int*, int*)>(api[0])(&major, &minor, &patch);
  if (major != 1)
    return;
  reinterpret_cast<void(__cdecl*)()>(api[15])();
  REXLOG_INFO("[renderdoc] capture of the next frame asked (API {}.{}.{})", major, minor, patch);
}

void LogBlur() {
  auto* memory = rex::system::kernel_memory();
  const uint8_t* p = memory ? memory->TranslateVirtual<const uint8_t*>(0x8297E3D4u) : nullptr;
  const uint32_t option = p ? (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]) : 0xFFFFFFFFu;
  const uint32_t passes = g_blur_passes.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("[blur] last 5 s: {} motion blur passes (sub_8221CC80), option 0x8297E3D4 = {} (0 = on)", passes, option);
}

void Watch() {
  int n = 0;
  int ticks = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (++ticks % 50 == 0)
      LogBlur();
    const bool snap = GetAsyncKeyState('P') & 1;
    const bool poke = GetAsyncKeyState('O') & 1;
    const bool capture = GetAsyncKeyState('C') & 1;
    if (!GameInFront())
      continue;
    if (capture)
      RenderDocCapture();
    if (snap) {
      ++n;
      MessageBeep(MB_OK);
      WriteSnapshot(n);
      MessageBeep(MB_ICONASTERISK);
    }
    if (poke)
      Poke();
  }
}
#endif
}  // namespace

void LogBlurGate() {
  auto take = [](std::atomic<uint32_t>& a) { return a.exchange(0, std::memory_order_relaxed); };
  const uint32_t calls = take(g_gate_calls), yes = take(g_gate_yes), object = take(g_gate_no_object),
                 camera = take(g_gate_no_camera), scan = take(g_gate_no_scan), option = take(g_gate_no_option),
                 last = take(g_gate_no_last), mismatch = take(g_gate_mismatch), passes = take(g_blur_passes);
  std::fprintf(stderr,
               "[blur-gate] 5 s: gate %u calls, %u yes, blur passes %u | no because: camera %u (last value %08X, "
               "object missing %u), scan 2359..2410 %u, option %u (value %u), last check %u | my reading differed "
               "%u\n",
               calls, yes, passes, camera, g_gate_camera_value.load(std::memory_order_relaxed), object, scan, option,
               g_gate_option_value.load(std::memory_order_relaxed), last, mismatch);
  auto r = [](std::atomic<uint32_t>& a) { return a.load(std::memory_order_relaxed); };
  std::fprintf(stderr,
               "[blur-gate] last check no: item invalid %u, +84 zero %u, +180 zero %u, +88 zero %u | last no: item "
               "%08X +84 %08X +180 %08X +88 %08X | last yes: item %08X +84 %08X +180 %08X +88 %08X\n",
               take(g_last_no_item), take(g_last_no_84), take(g_last_no_180), take(g_last_no_88), r(g_last_seen_no[0]),
               r(g_last_seen_no[1]), r(g_last_seen_no[2]), r(g_last_seen_no[3]), r(g_last_seen_yes[0]),
               r(g_last_seen_yes[1]), r(g_last_seen_yes[2]), r(g_last_seen_yes[3]));
  std::lock_guard lock(g_float_mutex);
  std::fprintf(stderr,
               "[blur-gate] floats of %08X: +124 %.4f..%.4f (%u at or over), +136 %.4f..%.4f (%u at or over), NaN %u, "
               "threshold %.4f, %u samples\n",
               g_float_other, double(g_float_a_min), double(g_float_a_max), g_float_a_over, double(g_float_b_min),
               double(g_float_b_max), g_float_b_over, g_float_nan, double(g_float_threshold), g_float_n);
  g_float_n = g_float_a_over = g_float_b_over = g_float_nan = 0;
}

void StartMemorySnapshots() {
#if defined(_WIN32)
  if (!REXCVAR_GET(tekken_memsnap))
    return;
  static std::atomic<bool> started{false};
  if (started.exchange(true))
    return;
  std::thread(Watch).detach();
  REXLOG_INFO("[memsnap] P writes a guest memory snapshot to memsnap/");
#endif
}

}  // namespace tekken
