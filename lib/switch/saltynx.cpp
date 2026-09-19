#include "saltynx.hpp"

#include "../logging.hpp"
#include "../window.hpp"

#include <aurora/saltynx.h>
#include <cstring>

#if defined(__SWITCH__)
#include <switch.h>

namespace aurora::saltynx {
namespace {
constexpr Module Log{"aurora::saltynx"};

struct ResolutionCalls {
  uint16_t width;
  uint16_t height;
  uint16_t calls;
} __attribute__((packed));

struct NxFpsSharedBlock {
  uint32_t magic;
  uint8_t fps;
  float fpsAvg;
  bool pluginActive;
  uint8_t fpsLocked;
  uint8_t fpsMode;
  uint8_t zeroSync;
  uint8_t patchApplied;
  uint8_t api;
  uint32_t fpsTicks[10];
  uint8_t buffers;
  uint8_t setBuffers;
  uint8_t activeBuffers;
  uint8_t setActiveBuffers;
  uint8_t displaySync;
  ResolutionCalls renderCalls[8];
  ResolutionCalls viewportCalls[8];
  bool forceOriginalRefreshRate;
  bool dontForce60InDocked;
  bool forceSuspend;
  uint8_t currentRefreshRate;
  float readSpeedPerSecond;
  uint8_t fpsLockedDocked;
  uint64_t frameNumber;
  int8_t expectedSetBuffers;
} __attribute__((packed));
static_assert(sizeof(NxFpsSharedBlock) == 174);

struct NxrtSharedBlock {
  uint32_t magic;
  bool isDocked;
  bool def;
  bool pluginActive;
  uint8_t res;
  bool wasDDRused;
} __attribute__((packed));
static_assert(sizeof(NxrtSharedBlock) == 9);

constexpr uint32_t kFpsMagic = 0x465053;
constexpr uint32_t kNxrtMagic = 0x5452584E;
constexpr size_t kPageSize = 0x1000;

bool g_disabled = false;
bool g_ready = false;
uint64_t g_framesSinceAttempt = 0;
bool g_didSmDance = false;
SharedMemory g_shmem{};
uint8_t* g_page = nullptr;
volatile NxFpsSharedBlock* g_fps = nullptr;
volatile NxrtSharedBlock* g_nxrt = nullptr;

uint64_t g_lastTick = 0;
uint64_t g_tickRing[10] = {};
uint32_t g_ringIdx = 0;
uint64_t g_frameNumber = 0;
uint64_t g_secWindowStart = 0;
uint32_t g_secFrames = 0;
uint8_t g_fpsOut = 0;
float g_fpsAvgOut = 0.0f;
volatile uint32_t* find_magic(uint32_t magic) noexcept {
  auto* words = reinterpret_cast<volatile uint32_t*>(g_page);
  for (size_t i = 0; i < kPageSize / 4; ++i) {
    if (words[i] == magic) {
      return const_cast<uint32_t*>(&words[i]);
    }
  }
  return nullptr;
}

bool run_session(Handle session) {
  Service s{};
  serviceCreate(&s, session);
  Handle shmemHandle = INVALID_HANDLE;
  Result rc = serviceDispatch(&s, 7, .in_send_pid = true, .out_handle_attrs = {SfOutHandleAttr_HipcCopy},
                              .out_handles = &shmemHandle);
  if (R_FAILED(rc) || shmemHandle == INVALID_HANDLE) {
    Log.debug("saltynx: cmd7 failed");
    serviceClose(&s);
    return false;
  }
  shmemLoadRemote(&g_shmem, shmemHandle, kPageSize, Perm_Rw);
  if (R_FAILED(shmemMap(&g_shmem))) {
    Log.warn("saltynx: shmem map failed");
    serviceClose(&s);
    return false;
  }
  g_page = static_cast<uint8_t*>(shmemGetAddr(&g_shmem));
  auto* fpsAt = find_magic(kFpsMagic);
  if (fpsAt != nullptr) {
    g_fps = reinterpret_cast<volatile NxFpsSharedBlock*>(fpsAt);
    Log.info("saltynx: reusing existing FPS block");
  } else {
    const uint32_t reqSize = sizeof(NxFpsSharedBlock);
    uint64_t offset = 0;
    rc = serviceDispatchInOut(&s, 6, reqSize, offset, .in_send_pid = true);
    if (R_FAILED(rc) || offset + sizeof(NxFpsSharedBlock) > kPageSize) {
      Log.warn("saltynx: FPS reserve failed");
      serviceClose(&s);
      return false;
    }
    g_fps = reinterpret_cast<volatile NxFpsSharedBlock*>(g_page + offset);
    std::memset(const_cast<NxFpsSharedBlock*>(g_fps), 0, sizeof(NxFpsSharedBlock));
    g_fps->magic = kFpsMagic;
    g_fps->api = 3;
    Log.info("saltynx: reserved new FPS block");
  }
  auto* nxrtAt = find_magic(kNxrtMagic);
  if (nxrtAt != nullptr) {
    g_nxrt = reinterpret_cast<volatile NxrtSharedBlock*>(nxrtAt);
  } else {
    const uint32_t reqSize = sizeof(NxrtSharedBlock);
    uint64_t offset = 0;
    rc = serviceDispatchInOut(&s, 6, reqSize, offset, .in_send_pid = true);
    if (R_SUCCEEDED(rc) && offset + sizeof(NxrtSharedBlock) <= kPageSize) {
      g_nxrt = reinterpret_cast<volatile NxrtSharedBlock*>(g_page + offset);
      std::memset(const_cast<NxrtSharedBlock*>(g_nxrt), 0, sizeof(NxrtSharedBlock));
      g_nxrt->magic = kNxrtMagic;
      g_nxrt->pluginActive = true;
    }
  }
  const uint32_t zero = 0;
  serviceDispatchIn(&s, 0, zero, .in_send_pid = true);
  serviceClose(&s);
  return true;
}

bool try_connect_and_run() {
  Handle session = INVALID_HANDLE;
  Result rc = svcConnectToNamedPort(&session, "SaltySD");
  if (R_SUCCEEDED(rc)) {
    const bool ok = run_session(session);
    svcCloseHandle(session);
    return ok;
  }
  if (rc == 0xF201) {
    Log.info("saltynx: SaltySD port not present, overlay support off");
    g_disabled = true;
    return false;
  }
  if (envIsSyscallHinted(0x7E)) {
    uint64_t rlTmp = 0;
    if (R_SUCCEEDED(svcGetInfo(&rlTmp, InfoType_ResourceLimit, INVALID_HANDLE, 0))) {
      Handle rl = (Handle)rlTmp;
      s64 cur = 0;
      if (R_SUCCEEDED(svcGetResourceLimitLimitValue(&cur, rl, LimitableResource_Sessions)) &&
          R_SUCCEEDED(svcSetResourceLimitLimitValue(rl, LimitableResource_Sessions, cur + 4))) {
        Log.info("saltynx: raised session limit");
      }
      svcCloseHandle(rl);
    }
    rc = svcConnectToNamedPort(&session, "SaltySD");
    if (R_SUCCEEDED(rc)) {
      const bool ok = run_session(session);
      svcCloseHandle(session);
      return ok;
    }
  }
  if (!g_didSmDance) {
    g_didSmDance = true;
    Log.info("saltynx: attempting sm handoff for session room");
    smExit();
    rc = svcConnectToNamedPort(&session, "SaltySD");
    bool ok = false;
    if (R_SUCCEEDED(rc)) {
      ok = run_session(session);
      svcCloseHandle(session);
    }
    smInitialize();
    if (!ok) {
      g_disabled = true;
    }
    return ok;
  }
  return false;
}

bool ensure_init() {
  if (g_ready) {
    return true;
  }
  if (g_disabled) {
    return false;
  }
  if (++g_framesSinceAttempt < 300 && g_framesSinceAttempt != 1) {
    return false;
  }
  g_framesSinceAttempt = 0;
  if (try_connect_and_run() && g_fps != nullptr) {
    g_ready = true;
    g_secWindowStart = armGetSystemTick();
    Log.info("saltynx: overlay link ready");
    return true;
  }
  return false;
}
} // namespace

void on_present() noexcept {
  if (!ensure_init() || g_fps == nullptr) {
    return;
  }
  const uint64_t now = armGetSystemTick();
  const uint64_t freq = armGetSystemTickFreq();
  if (g_lastTick != 0 && now > g_lastTick) {
    const uint64_t delta = now - g_lastTick;
    g_tickRing[g_ringIdx % 10] = delta;
    g_fps->fpsTicks[g_ringIdx % 10] = static_cast<uint32_t>(delta);
    g_ringIdx++;
  }
  g_lastTick = now;
  g_frameNumber++;
  g_secFrames++;
  g_fps->pluginActive = true;
  g_fps->frameNumber = g_frameNumber;
  const auto size = window::get_window_size();
  const uint16_t fbW = static_cast<uint16_t>(size.fb_width);
  const uint16_t fbH = static_cast<uint16_t>(size.fb_height);
  const uint16_t natW = static_cast<uint16_t>(size.native_fb_width);
  const uint16_t natH = static_cast<uint16_t>(size.native_fb_height);
  g_fps->renderCalls[0].width = fbW;
  g_fps->renderCalls[0].height = fbH;
  g_fps->renderCalls[0].calls = static_cast<uint16_t>(g_secFrames);
  g_fps->viewportCalls[0].width = natW;
  g_fps->viewportCalls[0].height = natH;
  g_fps->viewportCalls[0].calls = static_cast<uint16_t>(g_secFrames);
  if (now - g_secWindowStart >= freq) {
    uint64_t sum = 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < 10; ++i) {
      if (g_tickRing[i] != 0) {
        sum += g_tickRing[i];
        n++;
      }
    }
    g_fpsOut = static_cast<uint8_t>(g_secFrames);
    g_fpsAvgOut = (n == 0 || sum == 0) ? 0.0f : static_cast<float>(freq) / (static_cast<float>(sum) / n);
    g_fps->fps = g_fpsOut;
    g_fps->fpsAvg = g_fpsAvgOut;
    g_fps->api = 3;
    g_fps->renderCalls[0].calls = static_cast<uint16_t>(g_secFrames);
    g_fps->viewportCalls[0].calls = static_cast<uint16_t>(g_secFrames);
    g_secFrames = 0;
    g_secWindowStart = now;
  }
  if (g_nxrt != nullptr) {
    g_nxrt->pluginActive = true;
    if (g_nxrt->def) {
      g_nxrt->isDocked = (appletGetOperationMode() == AppletOperationMode_Console);
    }
  }
}

bool docked_override(bool* docked) noexcept {
  if (docked == nullptr || !g_ready || g_nxrt == nullptr) {
    return false;
  }
  if (g_nxrt->def) {
    return false;
  }
  *docked = g_nxrt->isDocked;
  return true;
}

} // namespace aurora::saltynx

extern "C" bool aurora_saltynx_docked_override(bool* docked_out) {
  return aurora::saltynx::docked_override(docked_out);
}
#else

namespace aurora::saltynx {
void on_present() noexcept {}
} // namespace aurora::saltynx

extern "C" bool aurora_saltynx_docked_override(bool*) {
  return false;
}
#endif