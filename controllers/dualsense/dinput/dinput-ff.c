/*
 * dinput-ff.c -- DirectInput force feedback for a bridged Sony pad.
 *
 * Installed into a prefix as dinput8.dll, with the original kept beside it as
 * dinput8_np.dll. Every export is the original's.
 *
 * With "Let games read controllers directly" the DualSense reaches a game as a
 * raw HID device, and a title that picks its controller through DirectInput
 * (Devil May Cry 5: DINPUT8 + XINPUT1_3, XInput only for Xbox-class devices)
 * reads it as a generic joystick. Wine's DirectInput offers force feedback only
 * for a HID device with a PID collection, which the pad does not have, so the
 * game is told the pad cannot vibrate and never asks.
 *
 * In a controller-bridge session (DSB_PORT set) the device objects are watched,
 * and for the bridged pad only (DSB_VENDOR, 054c for a DualSense, by DIPROP_VIDPID):
 *   - GetCapabilities reports DIDC_FORCEFEEDBACK, and EnumObjects marks the X
 *     and Y axes as force-feedback actuators;
 *   - EnumEffects/GetEffectInfo offer constant, ramp and periodic effects;
 *   - CreateEffect returns an effect object of ours. Its magnitude, gain and
 *     duration become the two motors: an effect on X drives the big left
 *     motor, one on Y the small right one, one on both drives both;
 *   - the motors go to the broker as a DSB_MOTORS datagram, like an XInput
 *     title's vibration does.
 * Every other device and every other call is the original's, unchanged.
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#define INITGUID
#include <winsock2.h>
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ log */

#define LOGFILE "C:\\notproton-pad.log"
#define LOG_MAX 400

static LONG log_lines;

static void logf_(const char *fmt, ...)
{
    char buf[512];
    int n;
    va_list ap;
    HANDLE h;
    DWORD w;

    if (InterlockedIncrement(&log_lines) > LOG_MAX) return;
    n = snprintf(buf, sizeof(buf), "[%7lu ms] [dinput] ", GetTickCount());
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\r'; buf[n++] = '\n';
    h = CreateFileA(LOGFILE, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, buf, n, &w, NULL);
    CloseHandle(h);
}

/* --------------------------------------------------------------- broker */

/* controllers/dualsense/protocol.h, restated for a PE build. */
#define DSB_MAGIC 0x31425344u
#define DSB_MOTORS 5
#pragma pack(push, 1)
typedef struct {
    uint32_t magic, kind;
    uint8_t token[16];
    uint32_t pid, sequence;
    double timestamp;
    float left, right;
    uint8_t report[64];
} DSBPacket;
#pragma pack(pop)

static BOOL broker;
static SOCKET sock = INVALID_SOCKET;
static struct sockaddr_in dest;
static uint8_t token[16];
static uint32_t sequence, source;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static double cf_absolute_time(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimePreciseAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (double)u.QuadPart / 1e7 - 11644473600.0 - 978307200.0;
}

static void send_motors(BYTE left, BYTE right)
{
    DSBPacket p;
    static LONG sent, errors;
    memset(&p, 0, sizeof(p));
    p.magic = DSB_MAGIC;
    p.kind = DSB_MOTORS;
    memcpy(p.token, token, 16);
    p.pid = source;
    p.sequence = ++sequence;
    p.timestamp = cf_absolute_time();
    p.left = left / 255.0f;   /* the heavy motor, 0 to 1 */
    p.right = right / 255.0f; /* the light one */
    if (sendto(sock, (const char *)&p, sizeof(p), 0, (const struct sockaddr *)&dest, sizeof(dest)) == sizeof(p))
    {
        if ((left || right) && InterlockedIncrement(&sent) == 1)
            logf_("first motors sent to the broker: left %u right %u", left, right);
    }
    else if (InterlockedIncrement(&errors) <= 5)
        logf_("sendto failed, error %d", WSAGetLastError());
}

static BOOL open_broker(void)
{
    char port[16], secret[64];
    DWORD n;
    long portnum;
    char *end;
    WSADATA wsa;
    int i;

    n = GetEnvironmentVariableA("DSB_PORT", port, sizeof(port));
    if (!n || n >= sizeof(port)) return FALSE;
    if (GetEnvironmentVariableA("DSB_TOKEN", secret, sizeof(secret)) != 32) return FALSE;
    for (i = 0; i < 16; i++)
    {
        int hi = hexval(secret[2 * i]), lo = hexval(secret[2 * i + 1]);
        if (hi < 0 || lo < 0) return FALSE;
        token[i] = (uint8_t)(hi << 4 | lo);
    }
    portnum = strtol(port, &end, 10);
    if (end == port || *end || portnum < 1024 || portnum > 65535) return FALSE;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return FALSE;
    if ((sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == INVALID_SOCKET) return FALSE;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((u_short)portnum);
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    /* Distinct from the bridge (Unix pid) and xinput1_x (0x80000000|pid). */
    source = 0xc0000000u | GetCurrentProcessId();
    return TRUE;
}

/* ------------------------------------------------------------- effects */

#define MAX_EFFECTS 32

typedef struct effect {
    IDirectInputEffect iface;
    LONG ref;
    GUID guid;
    void *device;        /* the device object it belongs to */
    BOOL used;
    BOOL playing;
    DWORD started, duration_ms, gain;  /* duration 0 = until stopped */
    BOOL left, right;    /* which motors */
    LONG magnitude;      /* 0..10000 */
    LONG ramp_start, ramp_end;
    BOOL ramp;
} effect;

static CRITICAL_SECTION lock;
static effect effects[MAX_EFFECTS];
static LONG device_gain = 10000;
static BOOL actuators_on = TRUE;

static effect *impl(IDirectInputEffect *iface)
{
    return CONTAINING_RECORD(iface, effect, iface);
}

static LONG clampl(LONG v, LONG lo, LONG hi) { return v < lo ? lo : v > hi ? hi : v; }

/* Reads the type-specific block and the axes of a DIEFFECT. */
static void apply(effect *e, const DIEFFECT *p, DWORD flags)
{
    if (!p) return;
    if (flags & DIEP_GAIN) e->gain = clampl(p->dwGain, 0, 10000);
    if (flags & DIEP_DURATION)
        e->duration_ms = p->dwDuration == INFINITE ? 0 : (p->dwDuration + 999) / 1000;
    if ((flags & DIEP_AXES) && p->rgdwAxes && p->cAxes)
    {
        DWORD i;
        BOOL l = FALSE, r = FALSE;
        for (i = 0; i < p->cAxes; i++)
        {
            /* Axes may be given as offsets or as object ids. */
            DWORD a = p->rgdwAxes[i];
            if (a == DIJOFS_X || (p->dwFlags & DIEFF_OBJECTIDS && DIDFT_GETINSTANCE(a) == 0)) l = TRUE;
            else r = TRUE;
        }
        e->left = l || p->cAxes > 1;
        e->right = r || p->cAxes > 1;
    }
    if ((flags & DIEP_TYPESPECIFICPARAMS) && p->lpvTypeSpecificParams)
    {
        if (IsEqualGUID(&e->guid, &GUID_ConstantForce) && p->cbTypeSpecificParams >= sizeof(DICONSTANTFORCE))
        {
            e->magnitude = clampl(labs(((const DICONSTANTFORCE *)p->lpvTypeSpecificParams)->lMagnitude), 0, 10000);
            e->ramp = FALSE;
        }
        else if (IsEqualGUID(&e->guid, &GUID_RampForce) && p->cbTypeSpecificParams >= sizeof(DIRAMPFORCE))
        {
            const DIRAMPFORCE *f = p->lpvTypeSpecificParams;
            e->ramp_start = clampl(labs(f->lStart), 0, 10000);
            e->ramp_end = clampl(labs(f->lEnd), 0, 10000);
            e->ramp = TRUE;
        }
        else if (p->cbTypeSpecificParams >= sizeof(DIPERIODIC))
        {
            const DIPERIODIC *f = p->lpvTypeSpecificParams;
            e->magnitude = clampl((LONG)f->dwMagnitude + labs(f->lOffset), 0, 10000);
            e->ramp = FALSE;
        }
    }
}

static LONG level(const effect *e, DWORD now)
{
    LONG m;
    if (!e->used || !e->playing || !actuators_on) return 0;
    if (e->duration_ms && now - e->started >= e->duration_ms) return 0;
    if (e->ramp)
    {
        LONG span = e->duration_ms ? (LONG)e->duration_ms : 1;
        LONG t = e->duration_ms ? (LONG)(now - e->started) : 0;
        m = e->ramp_start + (e->ramp_end - e->ramp_start) * t / span;
    }
    else
        m = e->magnitude;
    return m * (LONG)e->gain / 10000 * device_gain / 10000;
}

static DWORD WINAPI motor_thread(void *unused)
{
    BYTE last_l = 0, last_r = 0;
    DWORD last_send = 0;
    for (;;)
    {
        DWORD now = GetTickCount();
        LONG l = 0, r = 0;
        int i;
        BYTE bl, br;

        EnterCriticalSection(&lock);
        for (i = 0; i < MAX_EFFECTS; i++)
        {
            LONG v = level(&effects[i], now);
            if (effects[i].left) l = max(l, v);
            if (effects[i].right) r = max(r, v);
        }
        LeaveCriticalSection(&lock);
        bl = (BYTE)(l * 255 / 10000);
        br = (BYTE)(r * 255 / 10000);
        /* On change, and every half second while moving: the broker drops a
         * source's motors when it stops hearing from it. */
        if (bl != last_l || br != last_r || ((bl || br) && now - last_send >= 500))
        {
            send_motors(bl, br);
            last_l = bl;
            last_r = br;
            last_send = now;
        }
        Sleep(16);
    }
    return 0;
}

static HRESULT WINAPI fx_QueryInterface(IDirectInputEffect *iface, REFIID iid, void **out)
{
    if (IsEqualGUID(iid, &IID_IUnknown) || IsEqualGUID(iid, &IID_IDirectInputEffect))
    {
        *out = iface;
        IDirectInputEffect_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI fx_AddRef(IDirectInputEffect *iface)
{
    return InterlockedIncrement(&impl(iface)->ref);
}

static ULONG WINAPI fx_Release(IDirectInputEffect *iface)
{
    effect *e = impl(iface);
    LONG ref = InterlockedDecrement(&e->ref);
    if (!ref)
    {
        EnterCriticalSection(&lock);
        e->used = e->playing = FALSE;
        LeaveCriticalSection(&lock);
    }
    return ref;
}

static HRESULT WINAPI fx_Initialize(IDirectInputEffect *iface, HINSTANCE inst, DWORD version, REFGUID guid)
{
    return DI_OK;
}

static HRESULT WINAPI fx_GetEffectGuid(IDirectInputEffect *iface, GUID *guid)
{
    *guid = impl(iface)->guid;
    return DI_OK;
}

static HRESULT WINAPI fx_GetParameters(IDirectInputEffect *iface, DIEFFECT *p, DWORD flags)
{
    effect *e = impl(iface);
    if (!p) return E_POINTER;
    if (flags & DIEP_GAIN) p->dwGain = e->gain;
    if (flags & DIEP_DURATION) p->dwDuration = e->duration_ms ? e->duration_ms * 1000 : INFINITE;
    return DI_OK;
}

static HRESULT WINAPI fx_SetParameters(IDirectInputEffect *iface, const DIEFFECT *p, DWORD flags)
{
    effect *e = impl(iface);
    EnterCriticalSection(&lock);
    apply(e, p, flags);
    if (flags & DIEP_START)
    {
        e->playing = TRUE;
        e->started = GetTickCount();
    }
    LeaveCriticalSection(&lock);
    return DI_OK;
}

static HRESULT WINAPI fx_Start(IDirectInputEffect *iface, DWORD iterations, DWORD flags)
{
    effect *e = impl(iface);
    static LONG said;
    EnterCriticalSection(&lock);
    if (flags & DIES_SOLO)
    {
        int i;
        for (i = 0; i < MAX_EFFECTS; i++)
            if (effects[i].device == e->device) effects[i].playing = FALSE;
    }
    e->playing = TRUE;
    e->started = GetTickCount();
    LeaveCriticalSection(&lock);
    if (InterlockedIncrement(&said) <= 3)
        logf_("effect started: magnitude %ld gain %lu duration %lu ms motors %s%s", e->magnitude, e->gain,
              e->duration_ms, e->left ? "L" : "", e->right ? "R" : "");
    return DI_OK;
}

static HRESULT WINAPI fx_Stop(IDirectInputEffect *iface)
{
    EnterCriticalSection(&lock);
    impl(iface)->playing = FALSE;
    LeaveCriticalSection(&lock);
    return DI_OK;
}

static HRESULT WINAPI fx_GetEffectStatus(IDirectInputEffect *iface, DWORD *status)
{
    effect *e = impl(iface);
    DWORD now = GetTickCount();
    if (!status) return E_POINTER;
    EnterCriticalSection(&lock);
    *status = e->playing && (!e->duration_ms || now - e->started < e->duration_ms) ? DIEGES_PLAYING : 0;
    LeaveCriticalSection(&lock);
    return DI_OK;
}

static HRESULT WINAPI fx_Download(IDirectInputEffect *iface) { return DI_OK; }
static HRESULT WINAPI fx_Unload(IDirectInputEffect *iface) { return fx_Stop(iface); }
static HRESULT WINAPI fx_Escape(IDirectInputEffect *iface, DIEFFESCAPE *esc) { return DIERR_UNSUPPORTED; }

static IDirectInputEffectVtbl fx_vtbl = {
    fx_QueryInterface, fx_AddRef, fx_Release, fx_Initialize, fx_GetEffectGuid, fx_GetParameters,
    fx_SetParameters, fx_Start, fx_Stop, fx_GetEffectStatus, fx_Download, fx_Unload, fx_Escape,
};

/* --------------------------------------------------------- the device */

/* IDirectInputDevice8 slots; the A and W tables share the layout. */
enum { S_GETCAPS = 3, S_ENUMOBJECTS = 4, S_GETPROP = 5, S_SETPROP = 6, S_CREATEEFFECT = 18, S_ENUMEFFECTS = 19,
       S_GETEFFECTINFO = 20, S_GETFFSTATE = 21, S_SENDFFCMD = 22, S_ENUMCREATED = 23, S_SLOTS = 24 };

typedef struct table {
    void **vt;
    BOOL wide;
    void *real[S_SLOTS];
} table;

static table tables[4];

static table *table_of(void *device)
{
    void **vt = *(void ***)device;
    int i;
    for (i = 0; i < 4; i++) if (tables[i].vt == vt) return &tables[i];
    return NULL;
}

#define REAL(dev, slot, type) ((type)table_of(dev)->real[slot])

typedef HRESULT (WINAPI *getprop_fn)(void *, REFGUID, DIPROPHEADER *);

/* The bridged pad's vendor (DSB_VENDOR, 054c for a DualSense) by the device's
 * own DIPROP_VIDPID. */
static BOOL bridged_pad(void *device)
{
    static int vendor = -1;
    DIPROPDWORD p;
    table *t = table_of(device);
    if (!t) return FALSE;
    if (vendor < 0)
    {
        char text[8];
        int v = 0, i;
        if (GetEnvironmentVariableA("DSB_VENDOR", text, sizeof(text)) == 4)
            for (i = 0; i < 4 && v >= 0; i++)
                v = hexval(text[i]) < 0 ? -1 : v << 4 | hexval(text[i]);
        vendor = v > 0 ? v : 0x054c;
    }
    memset(&p, 0, sizeof(p));
    p.diph.dwSize = sizeof(p);
    p.diph.dwHeaderSize = sizeof(p.diph);
    p.diph.dwHow = DIPH_DEVICE;
    if (FAILED(((getprop_fn)t->real[S_GETPROP])(device, DIPROP_VIDPID, &p.diph))) return FALSE;
    return LOWORD(p.dwData) == vendor;
}

typedef HRESULT (WINAPI *getcaps_fn)(void *, DIDEVCAPS *);
static HRESULT WINAPI my_GetCapabilities(void *device, DIDEVCAPS *caps)
{
    HRESULT hr = REAL(device, S_GETCAPS, getcaps_fn)(device, caps);
    if (SUCCEEDED(hr) && caps && bridged_pad(device))
    {
        static LONG said;
        caps->dwFlags |= DIDC_FORCEFEEDBACK;
        if (caps->dwSize >= sizeof(DIDEVCAPS))
        {
            caps->dwFFSamplePeriod = 1000;
            caps->dwFFMinTimeResolution = 1000;
            caps->dwFFDriverVersion = 1;
        }
        if (InterlockedIncrement(&said) == 1) logf_("Sony pad: force feedback offered");
    }
    return hr;
}

/* EnumObjects: X and Y become force-feedback actuators. The callback is
 * wrapped per call; the original passes our context through. */
typedef struct { void *cb; void *ctx; BOOL wide; } enum_ctx;

static BOOL CALLBACK obj_cb_w(const DIDEVICEOBJECTINSTANCEW *o, void *c)
{
    enum_ctx *e = c;
    DIDEVICEOBJECTINSTANCEW copy = *o;
    if ((copy.dwType & DIDFT_AXIS) && (copy.dwOfs == DIJOFS_X || copy.dwOfs == DIJOFS_Y))
    {
        copy.dwFlags |= DIDOI_FFACTUATOR;
        copy.dwType |= DIDFT_FFACTUATOR;
    }
    return ((LPDIENUMDEVICEOBJECTSCALLBACKW)e->cb)(&copy, e->ctx);
}

static BOOL CALLBACK obj_cb_a(const DIDEVICEOBJECTINSTANCEA *o, void *c)
{
    enum_ctx *e = c;
    DIDEVICEOBJECTINSTANCEA copy = *o;
    if ((copy.dwType & DIDFT_AXIS) && (copy.dwOfs == DIJOFS_X || copy.dwOfs == DIJOFS_Y))
    {
        copy.dwFlags |= DIDOI_FFACTUATOR;
        copy.dwType |= DIDFT_FFACTUATOR;
    }
    return ((LPDIENUMDEVICEOBJECTSCALLBACKA)e->cb)(&copy, e->ctx);
}

typedef HRESULT (WINAPI *enumobj_fn)(void *, void *, void *, DWORD);
static HRESULT WINAPI my_EnumObjects(void *device, void *cb, void *ctx, DWORD flags)
{
    table *t = table_of(device);
    enum_ctx e = { cb, ctx, t->wide };
    if (!cb || !bridged_pad(device)) return ((enumobj_fn)t->real[S_ENUMOBJECTS])(device, cb, ctx, flags);
    /* Asked for actuators only: hand over the axes and mark X and Y. */
    if (flags & DIDFT_FFACTUATOR) flags = (flags & ~DIDFT_FFACTUATOR) | DIDFT_AXIS;
    return ((enumobj_fn)t->real[S_ENUMOBJECTS])(device, t->wide ? (void *)obj_cb_w : (void *)obj_cb_a, &e, flags);
}

typedef HRESULT (WINAPI *setprop_fn)(void *, REFGUID, const DIPROPHEADER *);
/* Predefined properties are small integers cast to REFGUID. */
static BOOL is_prop(REFGUID prop, REFGUID which) { return (ULONG_PTR)prop == (ULONG_PTR)which; }
static HRESULT WINAPI my_GetProperty(void *device, REFGUID prop, DIPROPHEADER *h)
{
    if (h && is_prop(prop, DIPROP_FFGAIN) && bridged_pad(device)) { ((DIPROPDWORD *)h)->dwData = device_gain; return DI_OK; }
    if (h && is_prop(prop, DIPROP_FFLOAD) && bridged_pad(device)) { ((DIPROPDWORD *)h)->dwData = 0; return DI_OK; }
    if (h && is_prop(prop, DIPROP_AUTOCENTER) && bridged_pad(device))
    {
        ((DIPROPDWORD *)h)->dwData = DIPROPAUTOCENTER_OFF;
        return DI_OK;
    }
    return REAL(device, S_GETPROP, getprop_fn)(device, prop, h);
}

static HRESULT WINAPI my_SetProperty(void *device, REFGUID prop, const DIPROPHEADER *h)
{
    if (h && is_prop(prop, DIPROP_FFGAIN) && bridged_pad(device))
    {
        device_gain = clampl(((const DIPROPDWORD *)h)->dwData, 0, 10000);
        return DI_OK;
    }
    if (h && is_prop(prop, DIPROP_AUTOCENTER) && bridged_pad(device)) return DI_OK;
    return REAL(device, S_SETPROP, setprop_fn)(device, prop, h);
}

static const struct { const GUID *guid; DWORD type; const char *name; } kinds[] = {
    { &GUID_ConstantForce, DIEFT_CONSTANTFORCE, "Constant" },
    { &GUID_RampForce, DIEFT_RAMPFORCE, "Ramp" },
    { &GUID_Sine, DIEFT_PERIODIC, "Sine" },
    { &GUID_Square, DIEFT_PERIODIC, "Square" },
    { &GUID_Triangle, DIEFT_PERIODIC, "Triangle" },
    { &GUID_SawtoothUp, DIEFT_PERIODIC, "SawtoothUp" },
    { &GUID_SawtoothDown, DIEFT_PERIODIC, "SawtoothDown" },
};

static void fill_info(void *info, BOOL wide, int k)
{
    DWORD params = DIEP_DURATION | DIEP_GAIN | DIEP_AXES | DIEP_DIRECTION | DIEP_TYPESPECIFICPARAMS | DIEP_STARTDELAY;
    if (wide)
    {
        DIEFFECTINFOW *i = info;
        i->guid = *kinds[k].guid;
        i->dwEffType = kinds[k].type | DIEFT_FFATTACK | DIEFT_FFFADE;
        i->dwStaticParams = i->dwDynamicParams = params;
        MultiByteToWideChar(CP_ACP, 0, kinds[k].name, -1, i->tszName, MAX_PATH);
    }
    else
    {
        DIEFFECTINFOA *i = info;
        i->guid = *kinds[k].guid;
        i->dwEffType = kinds[k].type | DIEFT_FFATTACK | DIEFT_FFFADE;
        i->dwStaticParams = i->dwDynamicParams = params;
        lstrcpynA(i->tszName, kinds[k].name, MAX_PATH);
    }
}

typedef HRESULT (WINAPI *enumfx_fn)(void *, void *, void *, DWORD);
static HRESULT WINAPI my_EnumEffects(void *device, void *cb, void *ctx, DWORD type)
{
    table *t = table_of(device);
    int k;
    if (!bridged_pad(device)) return ((enumfx_fn)t->real[S_ENUMEFFECTS])(device, cb, ctx, type);
    if (!cb) return DIERR_INVALIDPARAM;
    for (k = 0; k < (int)ARRAYSIZE(kinds); k++)
    {
        DIEFFECTINFOW w;
        DIEFFECTINFOA a;
        BOOL go;
        if (DIEFT_GETTYPE(type) && DIEFT_GETTYPE(type) != kinds[k].type) continue;
        if (t->wide) { memset(&w, 0, sizeof(w)); w.dwSize = sizeof(w); fill_info(&w, TRUE, k);
                       go = ((LPDIENUMEFFECTSCALLBACKW)cb)(&w, ctx); }
        else { memset(&a, 0, sizeof(a)); a.dwSize = sizeof(a); fill_info(&a, FALSE, k);
               go = ((LPDIENUMEFFECTSCALLBACKA)cb)(&a, ctx); }
        if (go == DIENUM_STOP) break;
    }
    return DI_OK;
}

typedef HRESULT (WINAPI *fxinfo_fn)(void *, void *, REFGUID);
static HRESULT WINAPI my_GetEffectInfo(void *device, void *info, REFGUID guid)
{
    table *t = table_of(device);
    int k;
    if (!bridged_pad(device)) return ((fxinfo_fn)t->real[S_GETEFFECTINFO])(device, info, guid);
    for (k = 0; k < (int)ARRAYSIZE(kinds); k++)
        if (IsEqualGUID(guid, kinds[k].guid)) { fill_info(info, t->wide, k); return DI_OK; }
    return DIERR_DEVICENOTREG;
}

typedef HRESULT (WINAPI *createfx_fn)(void *, REFGUID, const DIEFFECT *, IDirectInputEffect **, IUnknown *);
static HRESULT WINAPI my_CreateEffect(void *device, REFGUID guid, const DIEFFECT *p, IDirectInputEffect **out,
                                      IUnknown *outer)
{
    int i, k;
    effect *e = NULL;
    if (!bridged_pad(device)) return REAL(device, S_CREATEEFFECT, createfx_fn)(device, guid, p, out, outer);
    if (!out) return E_POINTER;
    for (k = 0; k < (int)ARRAYSIZE(kinds); k++) if (IsEqualGUID(guid, kinds[k].guid)) break;
    if (k == (int)ARRAYSIZE(kinds))
    {
        logf_("effect type not offered: {%08lx-...}", guid->Data1);
        return DIERR_DEVICENOTREG;
    }
    EnterCriticalSection(&lock);
    for (i = 0; i < MAX_EFFECTS && !e; i++)
        if (!effects[i].used) e = &effects[i];
    if (e)
    {
        memset(e, 0, sizeof(*e));
        e->iface.lpVtbl = &fx_vtbl;
        e->ref = 1;
        e->guid = *guid;
        e->device = device;
        e->used = TRUE;
        e->gain = 10000;
        e->left = e->right = TRUE;
        apply(e, p, DIEP_ALLPARAMS);
    }
    LeaveCriticalSection(&lock);
    if (!e) return DIERR_OUTOFMEMORY;
    {
        static LONG said;
        if (InterlockedIncrement(&said) <= 8)
            logf_("effect created: %s, axes %lu, magnitude %ld", kinds[k].name, p ? p->cAxes : 0, e->magnitude);
    }
    *out = &e->iface;
    return DI_OK;
}

typedef HRESULT (WINAPI *ffstate_fn)(void *, DWORD *);
static HRESULT WINAPI my_GetForceFeedbackState(void *device, DWORD *state)
{
    if (!bridged_pad(device)) return REAL(device, S_GETFFSTATE, ffstate_fn)(device, state);
    if (!state) return E_POINTER;
    *state = DIGFFS_POWERON | (actuators_on ? DIGFFS_ACTUATORSON : DIGFFS_ACTUATORSOFF);
    return DI_OK;
}

typedef HRESULT (WINAPI *ffcmd_fn)(void *, DWORD);
static HRESULT WINAPI my_SendForceFeedbackCommand(void *device, DWORD cmd)
{
    int i;
    if (!bridged_pad(device)) return REAL(device, S_SENDFFCMD, ffcmd_fn)(device, cmd);
    EnterCriticalSection(&lock);
    if (cmd & (DISFFC_STOPALL | DISFFC_RESET))
        for (i = 0; i < MAX_EFFECTS; i++) if (effects[i].device == device) effects[i].playing = FALSE;
    if (cmd & DISFFC_RESET)
        for (i = 0; i < MAX_EFFECTS; i++) if (effects[i].device == device) effects[i].used = FALSE;
    if (cmd & DISFFC_SETACTUATORSOFF) actuators_on = FALSE;
    if (cmd & DISFFC_SETACTUATORSON) actuators_on = TRUE;
    LeaveCriticalSection(&lock);
    return DI_OK;
}

typedef HRESULT (WINAPI *enumcreated_fn)(void *, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, void *, DWORD);
static HRESULT WINAPI my_EnumCreatedEffectObjects(void *device, LPDIENUMCREATEDEFFECTOBJECTSCALLBACK cb, void *ctx,
                                                  DWORD flags)
{
    int i;
    if (!bridged_pad(device)) return REAL(device, S_ENUMCREATED, enumcreated_fn)(device, cb, ctx, flags);
    if (!cb) return DIERR_INVALIDPARAM;
    for (i = 0; i < MAX_EFFECTS; i++)
        if (effects[i].used && effects[i].device == device && cb(&effects[i].iface, ctx) == DIENUM_STOP) break;
    return DI_OK;
}

static void patch(void **vt, int slot, void *mine, void **saved)
{
    DWORD old;
    if (vt[slot] == mine) return;
    *saved = vt[slot];
    if (!VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old)) return;
    vt[slot] = mine;
    VirtualProtect(&vt[slot], sizeof(void *), old, &old);
}

static void watch_device(void *device, BOOL wide)
{
    void **vt = *(void ***)device;
    table *t;
    int i;

    EnterCriticalSection(&lock);
    for (i = 0; i < 4 && tables[i].vt && tables[i].vt != vt; i++) ;
    if (i == 4 || tables[i].vt == vt) { LeaveCriticalSection(&lock); return; }
    t = &tables[i];
    t->vt = vt;
    t->wide = wide;
    patch(vt, S_GETCAPS, my_GetCapabilities, &t->real[S_GETCAPS]);
    patch(vt, S_ENUMOBJECTS, my_EnumObjects, &t->real[S_ENUMOBJECTS]);
    patch(vt, S_GETPROP, my_GetProperty, &t->real[S_GETPROP]);
    patch(vt, S_SETPROP, my_SetProperty, &t->real[S_SETPROP]);
    patch(vt, S_CREATEEFFECT, my_CreateEffect, &t->real[S_CREATEEFFECT]);
    patch(vt, S_ENUMEFFECTS, my_EnumEffects, &t->real[S_ENUMEFFECTS]);
    patch(vt, S_GETEFFECTINFO, my_GetEffectInfo, &t->real[S_GETEFFECTINFO]);
    patch(vt, S_GETFFSTATE, my_GetForceFeedbackState, &t->real[S_GETFFSTATE]);
    patch(vt, S_SENDFFCMD, my_SendForceFeedbackCommand, &t->real[S_SENDFFCMD]);
    patch(vt, S_ENUMCREATED, my_EnumCreatedEffectObjects, &t->real[S_ENUMCREATED]);
    LeaveCriticalSection(&lock);
    logf_("device table watched (%s)", wide ? "W" : "A");
}

/* IDirectInput8: 3 CreateDevice. A and W have their own tables. */
typedef HRESULT (WINAPI *createdev_fn)(void *, REFGUID, void **, IUnknown *);
static createdev_fn real_CreateDeviceW, real_CreateDeviceA;

static HRESULT WINAPI my_CreateDeviceW(void *di, REFGUID guid, void **out, IUnknown *outer)
{
    HRESULT hr = real_CreateDeviceW(di, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out)
    {
        watch_device(*out, TRUE);
        if (bridged_pad(*out)) logf_("the bridged pad opened through DirectInput (W)");
    }
    return hr;
}

static HRESULT WINAPI my_CreateDeviceA(void *di, REFGUID guid, void **out, IUnknown *outer)
{
    HRESULT hr = real_CreateDeviceA(di, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out)
    {
        watch_device(*out, FALSE);
        if (bridged_pad(*out)) logf_("the bridged pad opened through DirectInput (A)");
    }
    return hr;
}

/* ---------------------------------------------------------------- exports */

static HMODULE orig;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init(INIT_ONCE *o, void *param, void **ctx)
{
    InitializeCriticalSection(&lock);
    orig = LoadLibraryA("dinput8_np.dll");
    if (!orig) logf_("dinput8_np.dll did not load (error %lu)", GetLastError());
    else
    {
        char path[MAX_PATH] = "";
        GetModuleFileNameA(orig, path, sizeof(path));
        logf_("original loaded from %s (%p, ours %p)", path, orig, GetModuleHandleA(NULL) ? (void *)&DirectInput8Create : NULL);
    }
    broker = open_broker();
    if (broker)
    {
        CloseHandle(CreateThread(NULL, 0, motor_thread, NULL, 0, NULL));
        logf_("controller-bridge session: Sony pad force feedback goes to the broker");
    }
    else if (GetEnvironmentVariableA("DSB_PORT", NULL, 0))
        logf_("bridge session present but the broker socket did not open");
    return TRUE;
}

typedef HRESULT (WINAPI *create_fn)(HINSTANCE, DWORD, REFIID, void **, IUnknown *);

HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID iid, void **out, IUnknown *outer)
{
    static create_fn real;
    HRESULT hr;
    InitOnceExecuteOnce(&once, init, NULL, NULL);
    if (!real && orig) real = (create_fn)GetProcAddress(orig, "DirectInput8Create");
    if (real == (create_fn)DirectInput8Create)
    {
        logf_("the original resolved to this DLL itself; refusing to recurse");
        real = NULL;
    }
    if (!real)
    {
        logf_("DirectInput8Create: no original to call");
        return DIERR_GENERIC;
    }
    hr = real(inst, version, iid, out, outer);
    {
        static LONG said;
        if (InterlockedIncrement(&said) <= 3)
            logf_("DirectInput8Create(version %lx, %s) -> 0x%08lx", version,
                  IsEqualGUID(iid, &IID_IDirectInput8W) ? "W" : IsEqualGUID(iid, &IID_IDirectInput8A) ? "A" : "other", hr);
    }
    if (broker && SUCCEEDED(hr) && out && *out)
    {
        void **vt = *(void ***)*out;
        if (IsEqualGUID(iid, &IID_IDirectInput8W)) patch(vt, 3, my_CreateDeviceW, (void **)&real_CreateDeviceW);
        else if (IsEqualGUID(iid, &IID_IDirectInput8A)) patch(vt, 3, my_CreateDeviceA, (void **)&real_CreateDeviceA);
    }
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(inst);
    return TRUE;
}
