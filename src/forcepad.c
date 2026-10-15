/*
 * FC4 ForcePad - force Far Cry 4 to treat the gamepad as the only input device.
 *
 * Drop-in proxy DLL (normally installed as xinput1_3.dll next to FarCry4.exe).
 * It forwards every XInput / DirectInput8 call to the real system DLL, and:
 *
 *   - Hides keyboard and mouse input from the game (window messages, Raw Input,
 *     DirectInput8 keyboard/mouse devices, GetAsyncKeyState & friends), so the
 *     game never "switches" to keyboard/mouse prompts because of a stray touchpad,
 *     gyro-mouse or paddle-mapped key event from the handheld.
 *   - Remaps XInput slots so the first *connected* controller always appears as
 *     player 1 (slot 0), even if Windows put the built-in pad in slot 1-3.
 *   - Holding LB + RB + View(Back) for 1.5s toggles the keyboard/mouse blocking.
 *
 * Settings live in ForcePad.ini, diagnostics are written to ForcePad.log,
 * both next to the DLL.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#define PSAPI_VERSION 2
#include <psapi.h>

#ifndef XINPUT_GAMEPAD_GUIDE
#define XINPUT_GAMEPAD_GUIDE 0x0400
#endif

#define PROP_OLD "FC4ForcePad.OldProc"

/* ------------------------------------------------------------------------ */
/* Config / state                                                            */
/* ------------------------------------------------------------------------ */

static struct {
    int enabled;
    int blockKeyboard;
    int blockMouse;
    int freezeCursor;
    int parkCursor;
    int padSlot;        /* -1 = auto */
    int hideOtherPads;
    int toggleCombo;
    int log;
} g_cfg;

static HMODULE g_self;
static char g_dir[MAX_PATH];      /* DLL directory, with trailing backslash */
static char g_exeDir[MAX_PATH];   /* game exe directory, trailing backslash */
static char g_selfName[64];       /* lower-case basename of this DLL */
static int g_primary;             /* 0 = another copy is already active */
static volatile LONG g_blocking;  /* keyboard/mouse blocking currently on */
static CRITICAL_SECTION g_cs;
static HANDLE g_logFile = INVALID_HANDLE_VALUE;

static volatile LONG c_msgKbd, c_msgMouse, c_rawKbd, c_rawMouse, c_diKbd, c_diMouse, c_async;

static void log_msg(const char *fmt, ...)
{
    char buf[1024];
    int n;
    va_list ap;
    SYSTEMTIME st;
    DWORD w;

    if (!g_cfg.log || g_logFile == INVALID_HANDLE_VALUE)
        return;
    GetLocalTime(&st);
    n = _snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute,
                  st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    n += _vsnprintf(buf + n, sizeof(buf) - n - 3, fmt, ap);
    va_end(ap);
    if (n < 0 || n > (int)sizeof(buf) - 3)
        n = sizeof(buf) - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    EnterCriticalSection(&g_cs);
    WriteFile(g_logFile, buf, n, &w, NULL);
    LeaveCriticalSection(&g_cs);
}

static const char *basename_of(const char *p)
{
    const char *b = p, *s;
    for (s = p; *s; s++)
        if (*s == '\\' || *s == '/')
            b = s + 1;
    return b;
}

static void read_config(void)
{
    char ini[MAX_PATH];
    _snprintf(ini, sizeof(ini), "%sForcePad.ini", g_dir);
    ini[MAX_PATH - 1] = 0;
#define CFG(field, key, def) g_cfg.field = GetPrivateProfileIntA("ForcePad", key, def, ini)
    CFG(enabled, "Enabled", 1);
    CFG(blockKeyboard, "BlockKeyboard", 1);
    CFG(blockMouse, "BlockMouse", 1);
    CFG(freezeCursor, "FreezeCursor", 1);
    CFG(parkCursor, "ParkCursor", 1);
    CFG(padSlot, "PadSlot", -1);
    CFG(hideOtherPads, "HideOtherPads", 1);
    CFG(toggleCombo, "ToggleCombo", 1);
    CFG(log, "Log", 1);
#undef CFG
    if (g_cfg.padSlot < -1 || g_cfg.padSlot > 3)
        g_cfg.padSlot = -1;
}

static int block_kbd(void) { return g_blocking && g_cfg.blockKeyboard; }
static int block_mouse(void) { return g_blocking && g_cfg.blockMouse; }

static HMODULE load_system_dll(const char *name)
{
    char p[MAX_PATH];
    UINT n = GetSystemDirectoryA(p, MAX_PATH);
    if (!n || n + strlen(name) + 2 >= MAX_PATH)
        return NULL;
    strcat(p, "\\");
    strcat(p, name);
    return LoadLibraryA(p);
}

/* ------------------------------------------------------------------------ */
/* Real XInput                                                               */
/* ------------------------------------------------------------------------ */

typedef DWORD(WINAPI *XGetState_t)(DWORD, XINPUT_STATE *);
typedef DWORD(WINAPI *XSetState_t)(DWORD, XINPUT_VIBRATION *);
typedef DWORD(WINAPI *XGetCaps_t)(DWORD, DWORD, XINPUT_CAPABILITIES *);
typedef void(WINAPI *XEnable_t)(BOOL);
typedef DWORD(WINAPI *XGetDSound_t)(DWORD, GUID *, GUID *);
typedef DWORD(WINAPI *XGetBattery_t)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION *);
typedef DWORD(WINAPI *XGetKeystroke_t)(DWORD, DWORD, PXINPUT_KEYSTROKE);
typedef DWORD(WINAPI *XWaitGuide_t)(DWORD, DWORD, void *);
typedef DWORD(WINAPI *XIndex_t)(DWORD);
typedef DWORD(WINAPI *XGetAudioIds_t)(DWORD, LPWSTR, UINT *, LPWSTR, UINT *);

static struct {
    HMODULE mod;
    XGetState_t GetState, GetStateEx;
    XSetState_t SetState;
    XGetCaps_t GetCapabilities;
    XEnable_t Enable;
    XGetDSound_t GetDSoundAudioDeviceGuids;
    XGetBattery_t GetBatteryInformation;
    XGetKeystroke_t GetKeystroke;
    XWaitGuide_t WaitForGuideButton;
    XIndex_t CancelGuideButtonWait, PowerOffController;
    XGetAudioIds_t GetAudioDeviceIds;
} rx;

static INIT_ONCE g_xiOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK xinput_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    static const char *fallbacks[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    size_t i;
    (void)o; (void)p; (void)c;

    if (!strncmp(g_selfName, "xinput", 6))
        rx.mod = load_system_dll(g_selfName);
    for (i = 0; !rx.mod && i < sizeof(fallbacks) / sizeof(*fallbacks); i++)
        rx.mod = load_system_dll(fallbacks[i]);
    if (!rx.mod) {
        log_msg("ERROR: could not load any system XInput DLL");
        return TRUE;
    }
#define R(f, name) rx.f = (void *)GetProcAddress(rx.mod, name)
    R(GetState, "XInputGetState");
    R(GetStateEx, (LPCSTR)100);
    R(SetState, "XInputSetState");
    R(GetCapabilities, "XInputGetCapabilities");
    R(Enable, "XInputEnable");
    R(GetDSoundAudioDeviceGuids, "XInputGetDSoundAudioDeviceGuids");
    R(GetBatteryInformation, "XInputGetBatteryInformation");
    R(GetKeystroke, "XInputGetKeystroke");
    R(WaitForGuideButton, (LPCSTR)101);
    R(CancelGuideButtonWait, (LPCSTR)102);
    R(PowerOffController, (LPCSTR)103);
    R(GetAudioDeviceIds, "XInputGetAudioDeviceIds");
#undef R
    if (!rx.GetStateEx)
        rx.GetStateEx = rx.GetState;
    {
        char p2[MAX_PATH];
        GetModuleFileNameA(rx.mod, p2, MAX_PATH);
        log_msg("Real XInput: %s", p2);
    }
    return TRUE;
}

static void ensure_xinput(void) { InitOnceExecuteOnce(&g_xiOnce, xinput_init, NULL, NULL); }

/* ------------------------------------------------------------------------ */
/* Controller slot remapping                                                 */
/* ------------------------------------------------------------------------ */

static volatile LONG g_physSlot;   /* physical slot shown to the game as slot 0 */
static volatile DWORD g_lastProbe;
static volatile LONG g_probing;

static void refresh_pad_slot(int force)
{
    DWORD now = GetTickCount();
    XINPUT_STATE st;
    int i, cur, chosen = -1;

    if (g_cfg.padSlot >= 0) {
        g_physSlot = g_cfg.padSlot;
        return;
    }
    if (!force && now - g_lastProbe < 1000)
        return;
    if (InterlockedCompareExchange(&g_probing, 1, 0))
        return;
    g_lastProbe = now;
    if (rx.GetState) {
        cur = g_physSlot;
        if (rx.GetState(cur, &st) == ERROR_SUCCESS)
            chosen = cur;
        for (i = 0; chosen < 0 && i < 4; i++)
            if (rx.GetState(i, &st) == ERROR_SUCCESS)
                chosen = i;
        if (chosen >= 0 && chosen != cur) {
            log_msg("Controller found in physical slot %d -> presenting it as player 1", chosen);
            g_physSlot = chosen;
        }
    }
    g_probing = 0;
}

/* Map a game-visible slot to a physical slot. Returns -1 if it should look disconnected. */
static int map_slot(DWORD user)
{
    int phys, i, n = 0;
    if (user == XUSER_INDEX_ANY)
        return g_physSlot;
    if (user > 3)
        return (int)user;
    refresh_pad_slot(0);
    phys = g_physSlot;
    if (user == 0)
        return phys;
    if (g_cfg.hideOtherPads)
        return -1;
    for (i = 0; i < 4; i++) {
        if (i == phys)
            continue;
        if (++n == (int)user)
            return i;
    }
    return -1;
}

static void check_toggle_combo(const XINPUT_STATE *st)
{
    static DWORD heldSince;
    static int fired;
    const WORD combo = XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_BACK;

    if (!g_cfg.toggleCombo || !g_cfg.enabled)
        return;
    if ((st->Gamepad.wButtons & combo) == combo) {
        if (!heldSince) {
            heldSince = GetTickCount();
            fired = 0;
        } else if (!fired && GetTickCount() - heldSince >= 1500) {
            fired = 1;
            g_blocking = !g_blocking;
            MessageBeep(g_blocking ? MB_OK : MB_ICONHAND);
            log_msg("Toggle combo: keyboard/mouse blocking %s", g_blocking ? "ON" : "OFF");
        }
    } else {
        heldSince = 0;
    }
}

static DWORD get_state_common(XGetState_t fn, DWORD user, XINPUT_STATE *st)
{
    int phys;
    DWORD r;
    ensure_xinput();
    if (!fn)
        return ERROR_DEVICE_NOT_CONNECTED;
    phys = map_slot(user);
    if (phys < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    r = fn(phys, st);
    if (r == ERROR_SUCCESS && user == 0 && st)
        check_toggle_combo(st);
    return r;
}

DWORD WINAPI FP_XInputGetState(DWORD user, XINPUT_STATE *st) { return get_state_common(rx.GetState, user, st); }
DWORD WINAPI FP_XInputGetStateEx(DWORD user, XINPUT_STATE *st) { ensure_xinput(); return get_state_common(rx.GetStateEx, user, st); }

DWORD WINAPI FP_XInputSetState(DWORD user, XINPUT_VIBRATION *v)
{
    int phys;
    ensure_xinput();
    phys = map_slot(user);
    if (!rx.SetState || phys < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    return rx.SetState(phys, v);
}

DWORD WINAPI FP_XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES *caps)
{
    int phys;
    ensure_xinput();
    phys = map_slot(user);
    if (!rx.GetCapabilities || phys < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    return rx.GetCapabilities(phys, flags, caps);
}

void WINAPI FP_XInputEnable(BOOL enable)
{
    ensure_xinput();
    if (rx.Enable)
        rx.Enable(enable);
}

DWORD WINAPI FP_XInputGetDSoundAudioDeviceGuids(DWORD user, GUID *render, GUID *capture)
{
    int phys;
    ensure_xinput();
    phys = map_slot(user);
    if (!rx.GetDSoundAudioDeviceGuids || phys < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    return rx.GetDSoundAudioDeviceGuids(phys, render, capture);
}

DWORD WINAPI FP_XInputGetBatteryInformation(DWORD user, BYTE type, XINPUT_BATTERY_INFORMATION *info)
{
    int phys;
    ensure_xinput();
    phys = map_slot(user);
    if (!rx.GetBatteryInformation || phys < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    return rx.GetBatteryInformation(phys, type, info);
}

DWORD WINAPI FP_XInputGetKeystroke(DWORD user, DWORD reserved, PXINPUT_KEYSTROKE ks)
{
    int phys;
    DWORD r;
    ensure_xinput();
    phys = map_slot(user);
    if (!rx.GetKeystroke || phys < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    r = rx.GetKeystroke(phys, reserved, ks);
    if (r == ERROR_SUCCESS && ks) {
        if ((int)ks->UserIndex == g_physSlot)
            ks->UserIndex = 0;
        else if (g_cfg.hideOtherPads)
            return ERROR_EMPTY;
    }
    return r;
}

DWORD WINAPI FP_XInputWaitForGuideButton(DWORD user, DWORD flags, void *overlapped)
{
    int phys;
    ensure_xinput();
    phys = map_slot(user);
    if (!rx.WaitForGuideButton || phys < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    return rx.WaitForGuideButton(phys, flags, overlapped);
}

DWORD WINAPI FP_XInputCancelGuideButtonWait(DWORD user)
{
    int phys;
    ensure_xinput();
    phys = map_slot(user);
    if (!rx.CancelGuideButtonWait || phys < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    return rx.CancelGuideButtonWait(phys);
}

DWORD WINAPI FP_XInputPowerOffController(DWORD user)
{
    int phys;
    ensure_xinput();
    phys = map_slot(user);
    if (!rx.PowerOffController || phys < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    return rx.PowerOffController(phys);
}

DWORD WINAPI FP_XInputGetAudioDeviceIds(DWORD user, LPWSTR render, UINT *renderCount, LPWSTR capture, UINT *captureCount)
{
    int phys;
    ensure_xinput();
    phys = map_slot(user);
    if (!rx.GetAudioDeviceIds || phys < 0)
        return ERROR_DEVICE_NOT_CONNECTED;
    return rx.GetAudioDeviceIds(phys, render, renderCount, capture, captureCount);
}

/* ------------------------------------------------------------------------ */
/* DirectInput8 keyboard / mouse blocking                                    */
/* ------------------------------------------------------------------------ */

typedef HRESULT(WINAPI *DI8Create_t)(HINSTANCE, DWORD, REFIID, LPVOID *, void *);
typedef HRESULT(STDMETHODCALLTYPE *DICreateDevice_t)(void *, REFGUID, void **, void *);
typedef HRESULT(STDMETHODCALLTYPE *DIGetCaps_t)(void *, void *);
typedef HRESULT(STDMETHODCALLTYPE *DIGetState_t)(void *, DWORD, void *);
typedef HRESULT(STDMETHODCALLTYPE *DIGetData_t)(void *, DWORD, void *, DWORD *, DWORD);

enum { VT_CREATEDEVICE = 3, VT_GETCAPS = 3, VT_GETSTATE = 9, VT_GETDATA = 10 };
enum { KIND_NONE, KIND_KBD, KIND_MOUSE };

#define MAX_VT 16
#define MAX_DEV 32
static struct { void **vt; void *orig[11]; } g_vts[MAX_VT];
static int g_nvt;
static struct { void *dev; int kind; } g_devs[MAX_DEV];

static HMODULE g_realDinput;
static DI8Create_t r_DirectInput8Create;
static INIT_ONCE g_diOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK dinput_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    g_realDinput = load_system_dll("dinput8.dll");
    if (g_realDinput)
        r_DirectInput8Create = (DI8Create_t)GetProcAddress(g_realDinput, "DirectInput8Create");
    else
        log_msg("ERROR: could not load system dinput8.dll");
    return TRUE;
}

static void ensure_dinput(void) { InitOnceExecuteOnce(&g_diOnce, dinput_init, NULL, NULL); }

static void *vt_orig(void *obj, int idx)
{
    void **vt = *(void ***)obj;
    void *r = NULL;
    int i;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_nvt; i++)
        if (g_vts[i].vt == vt) {
            r = g_vts[i].orig[idx];
            break;
        }
    LeaveCriticalSection(&g_cs);
    return r;
}

static void vt_patch(void *obj, int idx, void *hook)
{
    void **vt = *(void ***)obj;
    DWORD old;
    int i;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < g_nvt && g_vts[i].vt != vt; i++)
        ;
    if (i == g_nvt) {
        if (g_nvt == MAX_VT) {
            LeaveCriticalSection(&g_cs);
            return;
        }
        g_vts[g_nvt].vt = vt;
        memset(g_vts[g_nvt].orig, 0, sizeof(g_vts[g_nvt].orig));
        g_nvt++;
    }
    if (vt[idx] != hook && VirtualProtect(&vt[idx], sizeof(void *), PAGE_READWRITE, &old)) {
        g_vts[i].orig[idx] = vt[idx];
        vt[idx] = hook;
        VirtualProtect(&vt[idx], sizeof(void *), old, &old);
    }
    LeaveCriticalSection(&g_cs);
}

static int dev_kind(void *dev)
{
    int i, k = KIND_NONE;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < MAX_DEV; i++)
        if (g_devs[i].dev == dev) {
            k = g_devs[i].kind;
            break;
        }
    LeaveCriticalSection(&g_cs);
    return k;
}

static void dev_set(void *dev, int kind)
{
    int i, freeSlot = -1;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < MAX_DEV; i++) {
        if (g_devs[i].dev == dev) {
            g_devs[i].kind = kind;
            if (kind == KIND_NONE)
                g_devs[i].dev = NULL;
            LeaveCriticalSection(&g_cs);
            return;
        }
        if (!g_devs[i].dev && freeSlot < 0)
            freeSlot = i;
    }
    if (kind != KIND_NONE && freeSlot >= 0) {
        g_devs[freeSlot].dev = dev;
        g_devs[freeSlot].kind = kind;
    }
    LeaveCriticalSection(&g_cs);
}

static HRESULT STDMETHODCALLTYPE H_DIGetDeviceState(void *self, DWORD cb, void *data)
{
    DIGetState_t orig = (DIGetState_t)vt_orig(self, VT_GETSTATE);
    HRESULT hr = orig ? orig(self, cb, data) : E_FAIL;
    int k;
    if (SUCCEEDED(hr) && data && (k = dev_kind(self)) != KIND_NONE) {
        if (k == KIND_KBD && block_kbd()) {
            memset(data, 0, cb);
            InterlockedIncrement(&c_diKbd);
        } else if (k == KIND_MOUSE && block_mouse()) {
            memset(data, 0, cb);
            InterlockedIncrement(&c_diMouse);
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE H_DIGetDeviceData(void *self, DWORD cbObj, void *rgdod, DWORD *inOut, DWORD flags)
{
    DIGetData_t orig = (DIGetData_t)vt_orig(self, VT_GETDATA);
    HRESULT hr = orig ? orig(self, cbObj, rgdod, inOut, flags) : E_FAIL;
    int k;
    if (SUCCEEDED(hr) && inOut && *inOut && (k = dev_kind(self)) != KIND_NONE) {
        if ((k == KIND_KBD && block_kbd()) || (k == KIND_MOUSE && block_mouse())) {
            InterlockedIncrement(k == KIND_KBD ? &c_diKbd : &c_diMouse);
            *inOut = 0;
        }
    }
    return hr;
}

/* {6F1D2B6x-D5A0-11CF-BFC7-444553540000}: SysMouse / SysKeyboard and their Em variants */
static int guid_kind(REFGUID g)
{
    static const BYTE tail[8] = {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    if (g->Data2 != 0xD5A0 || g->Data3 != 0x11CF || memcmp(g->Data4, tail, 8))
        return KIND_NONE;
    switch (g->Data1) {
    case 0x6F1D2B60: case 0x6F1D2B80: case 0x6F1D2B81: return KIND_MOUSE;
    case 0x6F1D2B61: case 0x6F1D2B82: case 0x6F1D2B83: return KIND_KBD;
    }
    return KIND_NONE;
}

static HRESULT STDMETHODCALLTYPE H_DICreateDevice(void *self, REFGUID g, void **out, void *outer)
{
    DICreateDevice_t orig = (DICreateDevice_t)vt_orig(self, VT_CREATEDEVICE);
    HRESULT hr = orig ? orig(self, g, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) {
        void *dev = *out;
        int kind = guid_kind(g);
        if (kind == KIND_NONE) {
            /* Created by instance GUID: ask the device what it is. */
            DWORD caps[11] = {sizeof(caps)}; /* DIDEVCAPS */
            DIGetCaps_t getCaps = (DIGetCaps_t)(*(void ***)dev)[VT_GETCAPS];
            if (SUCCEEDED(getCaps(dev, caps))) {
                BYTE type = (BYTE)(caps[2] & 0xFF);
                if (type == 0x12) kind = KIND_MOUSE;          /* DI8DEVTYPE_MOUSE */
                else if (type == 0x13) kind = KIND_KBD;       /* DI8DEVTYPE_KEYBOARD */
            }
        }
        dev_set(dev, kind);
        if (kind != KIND_NONE) {
            vt_patch(dev, VT_GETSTATE, (void *)H_DIGetDeviceState);
            vt_patch(dev, VT_GETDATA, (void *)H_DIGetDeviceData);
            log_msg("DirectInput %s device created - its input will be hidden from the game",
                    kind == KIND_KBD ? "keyboard" : "mouse");
        }
    }
    return hr;
}

static void hook_dinput_object(void *di)
{
    vt_patch(di, VT_CREATEDEVICE, (void *)H_DICreateDevice);
}

HRESULT WINAPI FP_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID riid, LPVOID *out, void *outer)
{
    HRESULT hr;
    ensure_dinput();
    if (!r_DirectInput8Create)
        return E_FAIL;
    hr = r_DirectInput8Create(inst, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out && g_primary && g_cfg.enabled)
        hook_dinput_object(*out);
    return hr;
}

/* Remaining dinput8.dll exports, only used if this file is installed as dinput8.dll. */
HRESULT WINAPI FP_DllCanUnloadNow(void)
{
    FARPROC f;
    ensure_dinput();
    f = g_realDinput ? GetProcAddress(g_realDinput, "DllCanUnloadNow") : NULL;
    return f ? ((HRESULT(WINAPI *)(void))f)() : S_FALSE;
}

HRESULT WINAPI FP_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID *out)
{
    FARPROC f;
    ensure_dinput();
    f = g_realDinput ? GetProcAddress(g_realDinput, "DllGetClassObject") : NULL;
    return f ? ((HRESULT(WINAPI *)(REFCLSID, REFIID, LPVOID *))f)(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}

HRESULT WINAPI FP_DllRegisterServer(void)
{
    FARPROC f;
    ensure_dinput();
    f = g_realDinput ? GetProcAddress(g_realDinput, "DllRegisterServer") : NULL;
    return f ? ((HRESULT(WINAPI *)(void))f)() : E_FAIL;
}

HRESULT WINAPI FP_DllUnregisterServer(void)
{
    FARPROC f;
    ensure_dinput();
    f = g_realDinput ? GetProcAddress(g_realDinput, "DllUnregisterServer") : NULL;
    return f ? ((HRESULT(WINAPI *)(void))f)() : E_FAIL;
}

void *WINAPI FP_GetdfDIJoystick(void)
{
    FARPROC f;
    ensure_dinput();
    f = g_realDinput ? GetProcAddress(g_realDinput, "GetdfDIJoystick") : NULL;
    return f ? ((void *(WINAPI *)(void))f)() : NULL;
}

/* ------------------------------------------------------------------------ */
/* user32 hooks (Raw Input, key state, cursor)                               */
/* ------------------------------------------------------------------------ */

typedef UINT(WINAPI *GetRawInputData_t)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
typedef UINT(WINAPI *GetRawInputBuffer_t)(PRAWINPUT, PUINT, UINT);
typedef SHORT(WINAPI *GetKeyState_t)(int);
typedef BOOL(WINAPI *GetKeyboardState_t)(PBYTE);
typedef BOOL(WINAPI *GetCursorPos_t)(LPPOINT);

static GetRawInputData_t o_GetRawInputData;
static GetRawInputBuffer_t o_GetRawInputBuffer;
static GetKeyState_t o_GetAsyncKeyState, o_GetKeyState;
static GetKeyboardState_t o_GetKeyboardState;
static GetCursorPos_t o_GetCursorPos;

static POINT g_frozenPt;
static volatile LONG g_frozenValid;

static int resolve_user32(int mayLoad)
{
    HMODULE u = GetModuleHandleA("user32.dll");
    if (!u && mayLoad)
        u = LoadLibraryA("user32.dll");
    if (!u)
        return 0;
    if (!o_GetRawInputData) {
        o_GetRawInputData = (GetRawInputData_t)GetProcAddress(u, "GetRawInputData");
        o_GetRawInputBuffer = (GetRawInputBuffer_t)GetProcAddress(u, "GetRawInputBuffer");
        o_GetAsyncKeyState = (GetKeyState_t)GetProcAddress(u, "GetAsyncKeyState");
        o_GetKeyState = (GetKeyState_t)GetProcAddress(u, "GetKeyState");
        o_GetKeyboardState = (GetKeyboardState_t)GetProcAddress(u, "GetKeyboardState");
        o_GetCursorPos = (GetCursorPos_t)GetProcAddress(u, "GetCursorPos");
    }
    return o_GetRawInputData != NULL;
}

static void scrub_raw(RAWINPUT *ri)
{
    if (ri->header.dwType == RIM_TYPEMOUSE && block_mouse()) {
        ri->data.mouse.usFlags = MOUSE_MOVE_RELATIVE;
        ri->data.mouse.ulButtons = 0;
        ri->data.mouse.ulRawButtons = 0;
        ri->data.mouse.lLastX = 0;
        ri->data.mouse.lLastY = 0;
        InterlockedIncrement(&c_rawMouse);
    } else if (ri->header.dwType == RIM_TYPEKEYBOARD && block_kbd()) {
        ri->data.keyboard.MakeCode = 0;
        ri->data.keyboard.Flags = RI_KEY_BREAK;
        ri->data.keyboard.VKey = 0xFF; /* "no key" */
        ri->data.keyboard.Message = WM_KEYUP;
        InterlockedIncrement(&c_rawKbd);
    }
}

static UINT WINAPI H_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdrSize)
{
    UINT r = o_GetRawInputData(h, cmd, data, size, hdrSize);
    if (r != (UINT)-1 && r >= sizeof(RAWINPUTHEADER) && data && cmd == RID_INPUT)
        scrub_raw((RAWINPUT *)data);
    return r;
}

static UINT WINAPI H_GetRawInputBuffer(PRAWINPUT data, PUINT size, UINT hdrSize)
{
    UINT r = o_GetRawInputBuffer(data, size, hdrSize), i;
    if (r != (UINT)-1 && data) {
        RAWINPUT *ri = data;
        for (i = 0; i < r; i++) {
            scrub_raw(ri);
            ri = (RAWINPUT *)(((ULONG_PTR)ri + ri->header.dwSize + 7) & ~(ULONG_PTR)7);
        }
    }
    return r;
}

static int vk_is_mouse(int vk)
{
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

static int vk_blocked(int vk) { return vk_is_mouse(vk) ? block_mouse() : block_kbd(); }

static SHORT WINAPI H_GetAsyncKeyState(int vk)
{
    SHORT r = o_GetAsyncKeyState(vk);
    if (r && vk_blocked(vk)) {
        InterlockedIncrement(&c_async);
        return 0;
    }
    return r;
}

static SHORT WINAPI H_GetKeyState(int vk)
{
    SHORT r = o_GetKeyState(vk);
    if (r && vk_blocked(vk))
        return 0;
    return r;
}

static BOOL WINAPI H_GetKeyboardState(PBYTE keys)
{
    BOOL r = o_GetKeyboardState(keys);
    int i;
    if (r && keys)
        for (i = 0; i < 256; i++)
            if (vk_blocked(i))
                keys[i] = 0;
    return r;
}

static BOOL WINAPI H_GetCursorPos(LPPOINT pt)
{
    if (pt && block_mouse() && g_cfg.freezeCursor) {
        if (!g_frozenValid) {
            if (!o_GetCursorPos(&g_frozenPt))
                return FALSE;
            g_frozenValid = 1;
        }
        *pt = g_frozenPt;
        return TRUE;
    }
    return o_GetCursorPos(pt);
}

/* ------------------------------------------------------------------------ */
/* Import table patching                                                     */
/* ------------------------------------------------------------------------ */

enum { DLL_OTHER, DLL_USER32, DLL_KERNEL32, DLL_DINPUT8, DLL_XINPUT };

static int dll_class(const char *name)
{
    const char *b = basename_of(name);
    if (!_stricmp(b, "user32.dll") || !_stricmp(b, "user32"))
        return DLL_USER32;
    if (!_stricmp(b, "kernel32.dll") || !_stricmp(b, "kernel32"))
        return DLL_KERNEL32;
    if (!_stricmp(b, "dinput8.dll") || !_stricmp(b, "dinput8"))
        return DLL_DINPUT8;
    if (!_strnicmp(b, "xinput", 6))
        return DLL_XINPUT;
    return DLL_OTHER;
}

static FARPROC WINAPI H_GetProcAddress(HMODULE mod, LPCSTR name);

typedef struct {
    int dll;
    const char *name;
    WORD ordinal;   /* XInput ordinals; 0 = none */
    void *hook;
    int needsUser32;
} HookDef;

static const HookDef g_hooks[] = {
    {DLL_USER32, "GetRawInputData", 0, (void *)H_GetRawInputData, 1},
    {DLL_USER32, "GetRawInputBuffer", 0, (void *)H_GetRawInputBuffer, 1},
    {DLL_USER32, "GetAsyncKeyState", 0, (void *)H_GetAsyncKeyState, 1},
    {DLL_USER32, "GetKeyState", 0, (void *)H_GetKeyState, 1},
    {DLL_USER32, "GetKeyboardState", 0, (void *)H_GetKeyboardState, 1},
    {DLL_USER32, "GetCursorPos", 0, (void *)H_GetCursorPos, 1},
    {DLL_KERNEL32, "GetProcAddress", 0, (void *)H_GetProcAddress, 0},
    {DLL_DINPUT8, "DirectInput8Create", 0, (void *)FP_DirectInput8Create, 0},
    {DLL_XINPUT, "XInputGetState", 2, (void *)FP_XInputGetState, 0},
    {DLL_XINPUT, "XInputSetState", 3, (void *)FP_XInputSetState, 0},
    {DLL_XINPUT, "XInputGetCapabilities", 4, (void *)FP_XInputGetCapabilities, 0},
    {DLL_XINPUT, "XInputGetBatteryInformation", 7, (void *)FP_XInputGetBatteryInformation, 0},
    {DLL_XINPUT, "XInputGetKeystroke", 8, (void *)FP_XInputGetKeystroke, 0},
    {DLL_XINPUT, NULL, 100, (void *)FP_XInputGetStateEx, 0},
};
#define NHOOKS (sizeof(g_hooks) / sizeof(*g_hooks))

static const HookDef *find_hook(int dll, const char *name, WORD ordinal)
{
    size_t i;
    for (i = 0; i < NHOOKS; i++) {
        if (g_hooks[i].dll != dll)
            continue;
        if (name && g_hooks[i].name && !strcmp(g_hooks[i].name, name))
            return &g_hooks[i];
        if (!name && ordinal && g_hooks[i].ordinal == ordinal)
            return &g_hooks[i];
    }
    return NULL;
}

static int patch_module(HMODULE mod, const char *modName)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt;
    IMAGE_DATA_DIRECTORY *dir;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    int patched = 0, haveUser32 = o_GetRawInputData != NULL;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir->VirtualAddress || !dir->Size)
        return 0;

    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir->VirtualAddress); imp->Name; imp++) {
        int cls = dll_class((const char *)(base + imp->Name));
        IMAGE_THUNK_DATA *names, *iat;
        if (cls == DLL_OTHER)
            continue;
        iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        names = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk) : NULL;
        if (!names)
            continue; /* bound-only import table: can't tell the names apart */
        for (; names->u1.AddressOfData; names++, iat++) {
            const HookDef *h;
            DWORD old;
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                h = find_hook(cls, NULL, (WORD)IMAGE_ORDINAL(names->u1.Ordinal));
            else
                h = find_hook(cls, (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name, 0);
            if (!h || (h->needsUser32 && !haveUser32))
                continue;
            if ((void *)iat->u1.Function == h->hook)
                continue;
            if (VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                iat->u1.Function = (ULONG_PTR)h->hook;
                VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
                patched++;
                if (h->name)
                    log_msg("Hooked %s -> %s!%s", modName, (const char *)(base + imp->Name), h->name);
                else
                    log_msg("Hooked %s -> %s!#%u", modName, (const char *)(base + imp->Name), h->ordinal);
            }
        }
    }
    return patched;
}

static FARPROC WINAPI H_GetProcAddress(HMODULE mod, LPCSTR name)
{
    FARPROC real = GetProcAddress(mod, name);
    char path[MAX_PATH];
    const HookDef *h;
    int cls;

    if (!real || !mod || !g_primary || !g_cfg.enabled)
        return real;
    if (!GetModuleFileNameA(mod, path, MAX_PATH))
        return real;
    cls = dll_class(path);
    if (cls == DLL_OTHER || cls == DLL_KERNEL32)
        return real;
    h = HIWORD((ULONG_PTR)name) ? find_hook(cls, name, 0) : find_hook(cls, NULL, LOWORD((ULONG_PTR)name));
    if (!h)
        return real;
    if (h->needsUser32 && !resolve_user32(1))
        return real;
    return (FARPROC)h->hook;
}

/* Patch the game exe and any DLLs that live in the game's folder. */
static void patch_game_modules(int mayLoad)
{
    HMODULE mods[1024];
    DWORD needed = 0, i, n;
    char path[MAX_PATH];

    if (mayLoad)
        resolve_user32(1);
    patch_module(GetModuleHandleA(NULL), "game exe");
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed))
        return;
    n = needed / sizeof(HMODULE);
    if (n > 1024)
        n = 1024;
    for (i = 0; i < n; i++) {
        if (mods[i] == g_self || mods[i] == GetModuleHandleA(NULL))
            continue;
        if (!GetModuleFileNameA(mods[i], path, MAX_PATH))
            continue;
        if (_strnicmp(path, g_exeDir, strlen(g_exeDir)))
            continue;
        if (dll_class(path) == DLL_XINPUT || dll_class(path) == DLL_DINPUT8)
            continue; /* other copies of proxy DLLs */
        patch_module(mods[i], basename_of(path));
    }
}

/* ------------------------------------------------------------------------ */
/* Window subclassing                                                        */
/* ------------------------------------------------------------------------ */

static int raw_is_blocked(HRAWINPUT h)
{
    RAWINPUTHEADER hdr;
    UINT sz = sizeof(hdr);
    if (!o_GetRawInputData || o_GetRawInputData(h, RID_HEADER, &hdr, &sz, sizeof(RAWINPUTHEADER)) == (UINT)-1)
        return 0;
    return (hdr.dwType == RIM_TYPEMOUSE && block_mouse()) || (hdr.dwType == RIM_TYPEKEYBOARD && block_kbd());
}

static LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = (WNDPROC)GetPropA(hwnd, PROP_OLD);
    BOOL uni = IsWindowUnicode(hwnd);

    if (!old)
        return uni ? DefWindowProcW(hwnd, msg, wp, lp) : DefWindowProcA(hwnd, msg, wp, lp);

    if (block_kbd()) {
        switch (msg) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_DEADCHAR: case WM_UNICHAR:
            InterlockedIncrement(&c_msgKbd);
            return 0;
        case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            if (wp == VK_F4)
                break; /* keep Alt+F4 working */
            InterlockedIncrement(&c_msgKbd);
            return 0;
        case WM_SYSCHAR: case WM_SYSDEADCHAR:
            return 0;
        }
    }
    if (block_mouse() && msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
        InterlockedIncrement(&c_msgMouse);
        return 0;
    }
    if (msg == WM_INPUT && raw_is_blocked((HRAWINPUT)lp))
        return uni ? DefWindowProcW(hwnd, msg, wp, lp) : DefWindowProcA(hwnd, msg, wp, lp);

    if (msg == WM_NCDESTROY) {
        if (uni) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)old);
        else SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)old);
        RemovePropA(hwnd, PROP_OLD);
    }
    return uni ? CallWindowProcW(old, hwnd, msg, wp, lp) : CallWindowProcA(old, hwnd, msg, wp, lp);
}

static void park_cursor(HWND hwnd)
{
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    if (!GetMonitorInfoA(mon, &mi))
        return;
    SetCursorPos(mi.rcMonitor.right - 1, mi.rcMonitor.bottom - 1);
    g_frozenValid = 0; /* re-capture the frozen point at the parked position */
}

static BOOL CALLBACK enum_windows_cb(HWND hwnd, LPARAM lp)
{
    DWORD pid = 0;
    char cls[128];
    LONG_PTR old;
    (void)lp;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetPropA(hwnd, PROP_OLD))
        return TRUE;
    if (!GetClassNameA(hwnd, cls, sizeof(cls)))
        cls[0] = 0;
    if (!strcmp(cls, "IME") || !strcmp(cls, "MSCTFIME UI"))
        return TRUE;

    old = IsWindowUnicode(hwnd) ? GetWindowLongPtrW(hwnd, GWLP_WNDPROC) : GetWindowLongPtrA(hwnd, GWLP_WNDPROC);
    if (!old || old == (LONG_PTR)SubclassProc)
        return TRUE;
    SetPropA(hwnd, PROP_OLD, (HANDLE)old);
    if (IsWindowUnicode(hwnd))
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassProc);
    else
        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)SubclassProc);
    log_msg("Filtering keyboard/mouse messages for window class \"%s\"", cls);
    if (g_cfg.parkCursor && block_mouse())
        park_cursor(hwnd);
    return TRUE;
}

/* ------------------------------------------------------------------------ */
/* Worker thread & entry point                                               */
/* ------------------------------------------------------------------------ */

static DWORD WINAPI worker(LPVOID p)
{
    DWORD start = GetTickCount(), lastReport = start;
    (void)p;

    ensure_xinput();
    refresh_pad_slot(1);
    for (;;) {
        DWORD now = GetTickCount();
        patch_game_modules(1);
        EnumWindows(enum_windows_cb, 0);
        refresh_pad_slot(0);
        if (now - lastReport >= 30000) {
            LONG a = InterlockedExchange(&c_msgKbd, 0), b = InterlockedExchange(&c_msgMouse, 0);
            LONG c = InterlockedExchange(&c_rawKbd, 0), d = InterlockedExchange(&c_rawMouse, 0);
            LONG e = InterlockedExchange(&c_diKbd, 0), f = InterlockedExchange(&c_diMouse, 0);
            LONG g = InterlockedExchange(&c_async, 0);
            if (a | b | c | d | e | f | g)
                log_msg("Blocked in last 30s: msg kbd=%ld mouse=%ld | raw kbd=%ld mouse=%ld | "
                        "dinput kbd=%ld mouse=%ld | keystate=%ld", a, b, c, d, e, f, g);
            lastReport = now;
        }
        Sleep(now - start < 60000 ? 500 : 2000);
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    char path[MAX_PATH], *s;
    (void)reserved;

    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    g_self = inst;
    DisableThreadLibraryCalls(inst);
    InitializeCriticalSection(&g_cs);

    GetModuleFileNameA(inst, path, MAX_PATH);
    strncpy(g_selfName, basename_of(path), sizeof(g_selfName) - 1);
    for (s = g_selfName; *s; s++)
        *s = (char)tolower((unsigned char)*s);
    strcpy(g_dir, path);
    g_dir[basename_of(path) - path] = 0;

    GetModuleFileNameA(NULL, path, MAX_PATH);
    strcpy(g_exeDir, path);
    g_exeDir[basename_of(path) - path] = 0;

    read_config();

    {
        char name[64];
        _snprintf(name, sizeof(name), "Local\\FC4ForcePad_%lu", GetCurrentProcessId());
        CreateMutexA(NULL, FALSE, name);
        g_primary = GetLastError() != ERROR_ALREADY_EXISTS;
    }

    if (g_primary && g_cfg.log) {
        char logPath[MAX_PATH];
        _snprintf(logPath, sizeof(logPath), "%sForcePad.log", g_dir);
        logPath[MAX_PATH - 1] = 0;
        g_logFile = CreateFileA(logPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, NULL);
    }

    if (!g_primary || !g_cfg.enabled) {
        g_blocking = 0;
        return TRUE; /* plain pass-through proxy */
    }

    g_blocking = 1;
    log_msg("FC4 ForcePad loaded as %s in %s", g_selfName, path);
    log_msg("Config: BlockKeyboard=%d BlockMouse=%d FreezeCursor=%d ParkCursor=%d PadSlot=%d "
            "HideOtherPads=%d ToggleCombo=%d", g_cfg.blockKeyboard, g_cfg.blockMouse,
            g_cfg.freezeCursor, g_cfg.parkCursor, g_cfg.padSlot, g_cfg.hideOtherPads, g_cfg.toggleCombo);

    /* Patch the exe right away (no LoadLibrary under the loader lock), then keep
       re-checking from a worker thread for late-loaded modules / unpacked imports. */
    resolve_user32(0);
    patch_game_modules(0);
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    return TRUE;
}
