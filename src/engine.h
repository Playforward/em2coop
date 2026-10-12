#pragma once

#include <windows.h>

namespace engine {

constexpr DWORD kImageBase = 0x00400000;

inline BYTE* image() {
    static BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleA(nullptr));
    return base;
}

inline BYTE* va(DWORD address) { return image() + (address - kImageBase); }

template <typename T>
inline T fn(DWORD address) { return reinterpret_cast<T>(va(address)); }

inline BYTE* global(DWORD address) {
    return *reinterpret_cast<BYTE**>(va(address));
}

constexpr DWORD kPlayerManager   = 0x01415aa8;
constexpr DWORD kPadManager      = 0x01424510;
constexpr DWORD kCameraManager   = 0x01427394;
constexpr DWORD kNiApplication   = 0x01431ef0;
constexpr DWORD kRenderer        = 0x013f8598;
constexpr DWORD kHudBindings     = 0x01424400;
constexpr DWORD kSpawner         = 0x014243fc;

constexpr DWORD kPlayerCamera0   = 0x0143b900;
constexpr DWORD kPlayerCamera1   = 0x0143b904;

constexpr DWORD kXInputGetStatePtr = 0x0143ddb0;

constexpr DWORD kRecordStride        = 0x110;
constexpr DWORD kRecEntityHandle     = 0x14;
constexpr DWORD kRecPlayerCamera     = 0x1c;
constexpr DWORD kRecControlMode      = 0x20;  // 0/3 human, 1/2 AI
constexpr DWORD kRecPresent          = 0x25;  // player 2: the co-op gate byte

constexpr int kModeHuman = 0;
constexpr int kModeAI    = 1;   // "CoopMove", BEH_BuddyFollow

constexpr DWORD kRec2DAnchor       = 0x18;
constexpr DWORD kApply2DAnchorSlot = 0x48;

constexpr DWORD kBroadcastRevive   = 0x00f34830;
constexpr DWORD kWarpToSpawn       = 0x00ede5f0;
constexpr DWORD k2DComponentSlotByte = 0x0143ad64;
constexpr DWORD k2DComponentAdjust = 0x18;
constexpr DWORD kRecHasSpawn       = 0x26;

constexpr DWORD kReleaseSecondPlayer = 0x0097b060;

constexpr DWORD kCanToggleSecondPlayer = 0x00978640;
constexpr DWORD kSetPlayerControlMode  = 0x0097a200;
constexpr DWORD kResolveEntity = 0x00a39e10;

constexpr DWORD kControllerSlotOffsetByte = 0x01415b48;
constexpr DWORD kClearControllerBehaviors = 0x00a3bc20;
constexpr DWORD kEntityComponentKey   = 0x4c;
constexpr DWORD kEntityComponents     = 0x20;
constexpr DWORD kControllerBehaviors  = 0x78;

constexpr DWORD kPadSlotBase     = 0x04;  // player i's pad at +4 + i*4, -1 = none
constexpr DWORD kPadCoopFlag     = 0x1c;
constexpr DWORD kAssignPadToPlayer = 0x00a22270;  // thiscall(player, pad)
constexpr DWORD kUnassignPlayer    = 0x00a22070;  // thiscall(player, bool)
constexpr int   kNoPad = -1;

constexpr DWORD kSpawnRequest = 0x00a1cc30;

constexpr DWORD kIntern        = 0x0047ac30;  // cdecl(const char*) -> char* (addref)
constexpr DWORD kReleaseString = 0x00401050;  // cdecl(char** slot)

constexpr DWORD kScriptEnableSplitScreen         = 0x00977840;  // (bool)
constexpr DWORD kScriptDisableToggleSecondPlayer = 0x00977880;  // (bool)
constexpr DWORD kScriptActivateSplitScreen       = 0x009778c0;  // ()
constexpr DWORD kScriptDeactivateSplitScreen     = 0x009778f0;  // ()

constexpr DWORD kScriptReaderCtor = 0x00932180;
constexpr DWORD kScriptReadBool   = 0x0090bb30;
constexpr DWORD kScriptReaderDtor = 0x0090b660;
constexpr size_t kScriptReaderSize = 0x60;

constexpr DWORD kHudPublish = 0x00a1f040;  // stdcall(slot*, HudValue*), ret 8

struct HudValue {
    int type;       // 3 = number
    int str_id;
    double value;
};

struct HudSlot {
    DWORD slot;     // byte offset into the binding array
    DWORD getter;   // cdecl(HudValue*) -> bool
    const char* name;
};

// Oswald's panel, from the binding routine at 0x00f2e970.
constexpr HudSlot kOswaldHudSlots[] = {
    { 0x020, 0x00f2d020, "OswaldIsActive" },
    { 0x024, 0x00f2d070, "OswaldIsPlayerControlled" },
    { 0x028, 0x00f2d0c0, "OswaldElectricityPercent" },
    { 0x02c, 0x00f2d130, "OswaldElectricityMax" },
    { 0x030, 0x00f2d160, "OswaldElectricitySegment" },
    { 0x034, 0x00f2d190, "OswaldLifeSegment" },
    { 0x038, 0x00f2e4d0, "OswaldCurrentLifeSegment" },
    { 0x10c, 0x00f2dc00, "OswaldElectricityEnabled" },
};

constexpr DWORD kHudSplitSlot   = 0x0d0;
constexpr DWORD kHudSplitGetter = 0x00f2ce20;  // cdecl(HudValue*) -> bool
// mov eax,[esp+4]; fldz
constexpr BYTE kHudSplitGetterPrologue[] = { 0x8b, 0x44, 0x24, 0x04, 0xd9, 0xee };

constexpr DWORD kPlayerCameraReset = 0x00f18470;

constexpr DWORD kGetCameraController = 0x00aff430;
constexpr DWORD kCameraTargetChanged   = 0x00a8c290;
constexpr DWORD kControllerInterface   = 0x08;
constexpr DWORD kControllerTarget      = 0x24;
constexpr DWORD kControllerNiCamera    = 0x4c;

constexpr DWORD kNiCameraVtbl     = 0x011ffc20;
constexpr DWORD kCameraRotate     = 0x7c;
constexpr DWORD kCameraTranslate  = 0xa0;
constexpr DWORD kCameraFrustum    = 0x104;  // left,right,top,bottom,near,far

constexpr DWORD kCursorVtbl            = 0x01307200;
constexpr DWORD kCursorPropertyAction  = 0xd0;
constexpr DWORD kCursorMoveAction      = 0xd4;
constexpr DWORD kCursorPushAction      = 0xd8;
constexpr DWORD kCursorHudFeed = 0x00a0a7d0;
constexpr DWORD kCursorHudFeedCalls[] = { 0x00f08c6f, 0x00f08dde };

constexpr DWORD kAppLiveClick  = 0x20954;
constexpr DWORD kAppLiveView   = 0x2094c;
constexpr DWORD kAppSceneStep  = 0x20a08;
constexpr DWORD kAppRenderFrame = 0x10;
constexpr DWORD kStepClickList = 0x30;     // NiTPointerList: +4 head, +8 tail, +0xc count
constexpr DWORD kFrameStepList = 0x08;

constexpr DWORD kGameAlloc       = 0x00476a90;  // cdecl(size,1,0,file,0xf7,"operator new")
constexpr DWORD kAllocSourceFile = 0x0139e038;
constexpr DWORD kAllocOperatorNew = 0x0139e024;

constexpr DWORD kRenderStepAppend    = 0x00a24730;  // thiscall(step, click)
constexpr DWORD kClickSetViewport    = 0x004bc6c0;  // thiscall(click, float[4])
constexpr DWORD kDefaultClickCtor    = 0x004bd2f0;
constexpr DWORD kDefaultClickAppend  = 0x004bd120;  // thiscall(click, child)
constexpr DWORD kDefaultClickBindView = 0x004f39a0; // thiscall(click, view, 0)
constexpr DWORD kPointerPoolInit     = 0x00497a80;  // thiscall(pool, capacity)
constexpr DWORD kNiViewRenderClickVtbl = 0x01200a1c;

constexpr DWORD kClickTargetGroup = 0x14;  // NiRenderTargetGroup*, refcounted
constexpr DWORD kClickViewport   = 0x18;
constexpr DWORD kClickClearMode  = 0x38;   // NiRenderClick::m_uiClearMode
constexpr DWORD kClickDerivedByte = 0x54;
constexpr DWORD kClickName       = 0x50;
constexpr DWORD kViewClickName   = 0x20;
constexpr DWORD kClickChildList  = 0x58;   // NiDefaultClick children
constexpr DWORD kClickView       = 0x68;   // the bound render view
constexpr DWORD kClickPool       = 0x70;
constexpr DWORD kViewCamera      = 0x10;
constexpr int   kClickRenderSlot = 8;      // vtable +0x20
constexpr int   kClickVtableEntries = 16;

constexpr DWORD kNiClearStencil = 0x2;
constexpr DWORD kNiClearZBuffer = 0x4;

constexpr DWORD kSceneViewCtor  = 0x00ed8110;  // (camera,process,scene,culler,view,usePort)
constexpr DWORD kSceneViewVtbl  = 0x012fd4f8;
constexpr DWORD kSceneViewSize  = 0x50;
constexpr DWORD kSceneViewCache = 0x0c;
constexpr DWORD kSceneViewCullFrame = 0x1c;
constexpr DWORD kSceneViewCuller = 0x24;
constexpr DWORD kSceneViewIndex  = 0x28;
constexpr DWORD kSceneViewCamera = 0x30;
constexpr DWORD kSceneViewProcess = 0x34;
constexpr DWORD kSceneViewScene  = 0x38;
constexpr DWORD kSceneViewUsePort = 0x4c;

constexpr DWORD kCullerPerView    = 0x0c;     // 4 pointers
constexpr DWORD kCullerLastIndex  = 0x31e18;  // last occupied index, -1 = none
constexpr DWORD kCullerViewDirty  = 0x31e1c;  // 4 bytes
constexpr DWORD kCullerMaxObjects = 0x1770;
constexpr DWORD kCullerObjectFlags = 0x2ef20;
constexpr DWORD kViewCollectedBits = 0x013bf258;  // DWORD[4]
constexpr DWORD kPerViewVtbl       = 0x012eb158;
constexpr DWORD kPerViewSetCamera  = 0x00da72b0;  // vtable +0x04
constexpr DWORD kPerViewQueue      = 0x00da7490;  // vtable +0x08
constexpr DWORD kPerViewGetVisible = 0x00da7370;  // vtable +0x20
constexpr DWORD kPerViewCamera     = 0x5044;
constexpr DWORD kPerViewPending    = 0x4fec;

constexpr DWORD kRendererSetCamera   = 0x0048c590;  // thiscall(renderer, NiCamera*)
constexpr DWORD kRendererCameraData  = 0x15c;       // loc, dir, up, right, frustum...
constexpr size_t kRendererCameraDataSize = 0x60;
constexpr DWORD kRendererCurrentCamera = 0xa8;
constexpr DWORD kRendererSavedCameraData = 0x1b8;
constexpr size_t kRendererCameraBlockSize = 0x5c;  // what +0x7c copies

constexpr DWORD kBillboardVtbl   = 0x01201c20;
constexpr DWORD kRotateToCamera  = 0x004ca640;      // thiscall(billboard, cameraData*)
constexpr DWORD kRotateToCameraCalls[] = { 0x004cabfe, 0x004cac9e, 0x004cacdc };

constexpr DWORD kDepthAccumulatorVtbl = 0x0131dc38;
constexpr DWORD kClickAccumulator     = 0x58;
constexpr DWORD kOnTopList            = 0x318;
constexpr DWORD kOnTopListVtbl        = 0x01399a38;
constexpr DWORD kOnTopCount           = 0x14;
constexpr DWORD kOnTopItems           = 0x1c;

constexpr DWORD kPlaylistEval  = 0x00b6db40;
constexpr DWORD kPlaylistReset = 0x00b6d3c0;
constexpr DWORD kPlaylistEvalCalls[] = {
    0x00b6dd4b, 0x00b6ddcb, 0x00b6de5b, 0x00b6dedb };

constexpr int kD3DDeviceVtableEntries = 134;
constexpr int kD3DGetBackBuffer  = 18;
constexpr int kD3DSetRenderTarget = 37;
constexpr int kD3DClear          = 43;
constexpr int kD3DSetViewport    = 47;
constexpr int kD3DGetViewport    = 48;
constexpr int kSurfaceGetDesc    = 12;

struct D3DViewport9 {
    DWORD x, y, width, height;
    float min_z, max_z;
};

#define EM2_THISCALL __attribute__((thiscall))
#define EM2_CDECL    __attribute__((cdecl))
#define EM2_STDCALL  __attribute__((stdcall))

}  // namespace engine
