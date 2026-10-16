/*
 * xinput-motors.c -- XInput vibration for a bridged Sony pad.
 *
 * Installed into a runner as xinput1_4.dll, with the runner's own xinput1_4.dll
 * kept beside it as xinput1_4_np.dll. Every export is the original's, called
 * unchanged.
 *
 * With "Let games read controllers directly" the DualSense reaches the game as
 * a HID device, and wine's xinput reports it as a pad. A title that reads the
 * pad through XInput (Unreal's XInputInterface, CONTROL) then asks for
 * vibration with XInputSetState, and wine's xinput has nowhere to put it: the
 * pad's descriptor has no haptics collection. Adaptive triggers still work
 * because the game writes them to the pad itself.
 *
 * The controller bridge already has a single writer to the pad, the broker,
 * which merges every source and owns the Bluetooth output budget. So the
 * motors XInput asks for are sent to the broker as a DSB_MOTORS datagram: the
 * two motor values and nothing of the pad's own reports, which the broker folds
 * with every other source. Triggers, lights and the game's own writes are not
 * touched. Without a broker session (DSB_PORT/DSB_TOKEN unset) nothing is sent.
 *
 * Only a pad whose XInputGetCapabilitiesEx names the bridged pad's vendor
 * (DSB_VENDOR, 054c for a DualSense) is forwarded, so another pad's vibration
 * never lands on the bridged one. A pad another backend drives (DSB_DEVICE
 * other than dualsense) is one wine maps to XInput itself: its vibration goes
 * to the broker alone, so the broker stays the only writer to it.
 *
 * With DSB_XINPUT_PAD=1 the bridged Sony pad is also presented as an XInput
 * pad, built from its own HID input reports. The launcher sets it for a title
 * Steam does not list as supporting the PS5 controller (store category 57):
 * such a game reads only XInput (or Steam Input), and with direct controller
 * access wine gives the pad to the game as a raw Sony HID device and does not
 * also map it to XInput, so the game sees no controller at all (measured with
 * Onimusha: Way of the Sword). That pad's vibration goes to the broker like any
 * other XInput vibration. A title that speaks Sony keeps the raw device only,
 * so it never sees the same pad twice.
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

/* ------------------------------------------------------------------ log */

#define LOGFILE "C:\\notproton-pad.log"
#define LOG_MAX 400

static LONG log_lines;
static DWORD t0;

static void logf_(const char *fmt, ...)
{
    char buf[512];
    int n;
    va_list ap;
    HANDLE h;
    DWORD w;

    if (InterlockedIncrement(&log_lines) > LOG_MAX) return;
    n = snprintf(buf, sizeof(buf), "[%7lu ms] [motors] ", GetTickCount() - t0);
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

/* ------------------------------------------------------------- original */

typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XPAD;
typedef struct { DWORD dwPacketNumber; XPAD Gamepad; } XSTATE;
typedef struct { WORD wLeftMotorSpeed, wRightMotorSpeed; } XVIB;
typedef struct { BYTE Type, SubType; WORD Flags; XPAD Gamepad; XVIB Vibration; } XCAPS;
typedef struct { XCAPS Capabilities; WORD VendorId, ProductId, VersionNumber, unk1; DWORD unk2; } XCAPSEX;

/* The runner's own DLL, kept beside this one: xinput1_4_np.dll, or
 * xinput1_3_np.dll for the older interface many titles still import. */
#ifndef ORIG_DLL
#define ORIG_DLL "xinput1_4_np.dll"
#endif

static HMODULE orig;

static FARPROC orig_proc(const char *name)
{
    if (!orig)
    {
        orig = LoadLibraryA(ORIG_DLL);
        if (!orig) logf_("%s did not load (error %lu)", ORIG_DLL, GetLastError());
    }
    return orig ? GetProcAddress(orig, name) : NULL;
}

#define ORIG(type, name) static type p_; if (!p_) p_ = (type)orig_proc(name)

typedef DWORD (WINAPI *get_state_fn)(DWORD, XSTATE *);
typedef DWORD (WINAPI *set_state_fn)(DWORD, XVIB *);
typedef DWORD (WINAPI *get_caps_fn)(DWORD, DWORD, XCAPS *);
typedef DWORD (WINAPI *get_caps_ex_fn)(DWORD, DWORD, DWORD, XCAPSEX *);
typedef void (WINAPI *enable_fn)(BOOL);
typedef DWORD (WINAPI *battery_fn)(DWORD, BYTE, void *);
typedef DWORD (WINAPI *keystroke_fn)(DWORD, DWORD, void *);
typedef DWORD (WINAPI *audio_ids_fn)(DWORD, WCHAR *, UINT *, WCHAR *, UINT *);

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

static CRITICAL_SECTION lock;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static BOOL broker;          /* a session is configured and the socket is open */
/* DSB_STEAM_PAD=1: the title reads the pad through Steam Input, and its XInput
 * vibration, sent to Steam's virtual pad, goes to the bridged pad instead. */
static BOOL steam_pad;
/* DSB_VENDOR: the vendor of the pad the session bridges (054c for a DualSense). */
static WORD bridged_vendor = 0x054c;
/* DSB_DEVICE other than dualsense: wine maps the pad to XInput itself and would
 * write its motors too, so its vibration goes to the broker alone. */
static BOOL broker_only;
static SOCKET sock = INVALID_SOCKET;
static struct sockaddr_in dest;
static uint8_t token[16];
static uint32_t sequence, source;
static BYTE motor_left, motor_right;    /* last sent, 0..255 */
static LONG sent, send_errors;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Seconds since 2001-01-01, the clock the broker stamps and checks against. */
static double cf_absolute_time(void)
{
    FILETIME ft;
    ULARGE_INTEGER u;
    GetSystemTimePreciseAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (double)u.QuadPart / 1e7 - 11644473600.0 - 978307200.0;
}

static void send_motors_locked(BYTE left, BYTE right)
{
    DSBPacket p;
    int r;

    memset(&p, 0, sizeof(p));
    p.magic = DSB_MAGIC;
    p.kind = DSB_MOTORS;
    memcpy(p.token, token, 16);
    p.pid = source;
    p.sequence = ++sequence;
    p.timestamp = cf_absolute_time();
    /* The heavy left motor and the light right one, 0 to 1. */
    p.left = left / 255.0f;
    p.right = right / 255.0f;
    r = sendto(sock, (const char *)&p, sizeof(p), 0, (const struct sockaddr *)&dest, sizeof(dest));
    if (r == sizeof(p))
    {
        if (InterlockedIncrement(&sent) == 1)
            logf_("first motors sent to the broker: left %u right %u", left, right);
    }
    else if (InterlockedIncrement(&send_errors) <= 5)
        logf_("sendto -> %d, error %d", r, WSAGetLastError());
    motor_left = left;
    motor_right = right;
}

/* The broker holds a source's motors while it hears from it within two
 * seconds; a game that sets a value once and leaves it is kept alive here. */
/* Keeps this DLL loaded for the life of the process. A title (or SDL) that loads
 * XInput on demand frees it again, and the threads below would then run in
 * unmapped code. Called from an API call's first use, never from DllMain. */
static void pin_self(void)
{
    HMODULE self;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCWSTR)(void *)pin_self, &self))
        logf_("could not pin this DLL (error %lu); it must not be unloaded", GetLastError());
}

static DWORD WINAPI keepalive(void *unused)
{
    for (;;)
    {
        Sleep(500);
        EnterCriticalSection(&lock);
        if (motor_left || motor_right) send_motors_locked(motor_left, motor_right);
        LeaveCriticalSection(&lock);
    }
    return 0;
}

static BOOL CALLBACK init_broker(INIT_ONCE *o, void *param, void **ctx)
{
    char port[16], secret[64];
    DWORD n;
    long portnum;
    char *end;
    WSADATA wsa;
    int i;

    InitializeCriticalSection(&lock);
    n = GetEnvironmentVariableA("DSB_PORT", port, sizeof(port));
    if (!n || n >= sizeof(port)) { logf_("no broker session (DSB_PORT unset); vibration stays wine's"); return TRUE; }
    n = GetEnvironmentVariableA("DSB_TOKEN", secret, sizeof(secret));
    if (n != 32) { logf_("DSB_TOKEN missing or malformed"); return TRUE; }
    for (i = 0; i < 16; i++)
    {
        int hi = hexval(secret[2 * i]), lo = hexval(secret[2 * i + 1]);
        if (hi < 0 || lo < 0) { logf_("DSB_TOKEN malformed"); return TRUE; }
        token[i] = (uint8_t)(hi << 4 | lo);
    }
    portnum = strtol(port, &end, 10);
    if (end == port || *end || portnum < 1024 || portnum > 65535) { logf_("DSB_PORT malformed"); return TRUE; }
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) { logf_("WSAStartup failed"); return TRUE; }
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) { logf_("socket failed, error %d", WSAGetLastError()); return TRUE; }
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons((u_short)portnum);
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    /* Distinct from the bridge's own source, which is the Unix pid, and from the
     * other build: a process can load both xinput1_3 and xinput1_4, each with its
     * own sequence, and one id would make the broker drop one of them. */
    source = (strstr(ORIG_DLL, "1_3") ? 0xA0000000u : 0x80000000u) | GetCurrentProcessId();
    {
        char flag[8], vendor[8], device[32];
        steam_pad = GetEnvironmentVariableA("DSB_STEAM_PAD", flag, sizeof(flag)) && !strcmp(flag, "1");
        broker_only = GetEnvironmentVariableA("DSB_DEVICE", device, sizeof(device)) &&
                      strcmp(device, "dualsense");
        if (GetEnvironmentVariableA("DSB_VENDOR", vendor, sizeof(vendor)) == 4)
        {
            int v = 0;
            for (i = 0; i < 4 && v >= 0; i++)
                v = hexval(vendor[i]) < 0 ? -1 : v << 4 | hexval(vendor[i]);
            if (v > 0) bridged_vendor = (WORD)v;
        }
    }
    broker = TRUE;
    pin_self();
    CloseHandle(CreateThread(NULL, 0, keepalive, NULL, 0, NULL));
    logf_("broker session on port %ld, source %08x%s", portnum, source,
          steam_pad ? "; Steam Input pad vibration goes to the bridged pad" : "");
    return TRUE;
}

/* ------------------------------------------------- which pad is the bridged one */

/* XInputGetCapabilitiesEx tells the bridged pad by its vendor id. Wine's
 * xinput1_3 has no such export (ordinals 1-8 and 100 only), so that build asks
 * the runner's xinput1_4, which the run script keeps beside it as
 * xinput1_4_np.dll; both list the HID devices in the order Wine enumerates them
 * (inferred, not measured). */
static get_caps_ex_fn caps_ex_fn;

static BOOL CALLBACK find_caps_ex(INIT_ONCE *o, void *param, void **ctx)
{
    HMODULE other;
    caps_ex_fn = (get_caps_ex_fn)orig_proc("XInputGetCapabilitiesEx");
    if (caps_ex_fn) return TRUE;
    if ((other = LoadLibraryA("xinput1_4_np.dll")))
        caps_ex_fn = (get_caps_ex_fn)GetProcAddress(other, "XInputGetCapabilitiesEx");
    logf_(caps_ex_fn ? "%s has no XInputGetCapabilitiesEx; vendor ids come from xinput1_4_np.dll"
                     : "no XInputGetCapabilitiesEx in %s or xinput1_4_np.dll; vibration stays wine's",
          ORIG_DLL);
    return TRUE;
}

static get_caps_ex_fn caps_ex(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, find_caps_ex, NULL, NULL);
    return caps_ex_fn;
}

static int bridged[4];       /* 0 unknown, 1 the bridged pad's vendor, -1 other */
static DWORD bridged_checked[4];

static BOOL is_bridged(DWORD index)
{
    DWORD now = GetTickCount();
    if (index >= 4) return FALSE;
    if (!bridged[index] || now - bridged_checked[index] > 5000)
    {
        get_caps_ex_fn p_ = caps_ex();
        XCAPSEX caps;
        int was = bridged[index];
        memset(&caps, 0, sizeof(caps));
        if (p_ && p_(1, index, 0, &caps) == ERROR_SUCCESS)
            bridged[index] = caps.VendorId == bridged_vendor ? 1 : -1;
        else
            bridged[index] = -1;
        bridged_checked[index] = now;
        if (bridged[index] != was)
            logf_("pad %lu: vendor %04x product %04x -> %s", index, caps.VendorId, caps.ProductId,
                  bridged[index] > 0 ? "the bridged pad, vibration goes to the broker" : "not ours");
    }
    return bridged[index] > 0;
}

/* ------------------------------------------------- the pad as XInput pad */

#define BTN_DPAD_UP    0x0001
#define BTN_DPAD_DOWN  0x0002
#define BTN_DPAD_LEFT  0x0004
#define BTN_DPAD_RIGHT 0x0008
#define BTN_START      0x0010
#define BTN_BACK       0x0020
#define BTN_LTHUMB     0x0040
#define BTN_RTHUMB     0x0080
#define BTN_LSHOULDER  0x0100
#define BTN_RSHOULDER  0x0200
#define BTN_GUIDE      0x0400
#define BTN_A          0x1000
#define BTN_B          0x2000
#define BTN_X          0x4000
#define BTN_Y          0x8000

static INIT_ONCE pad_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION pad_lock;
static BOOL pad_enabled;          /* DSB_XINPUT_PAD=1 in a bridge session */
static int pad_index = -1;        /* the XInput slot the pad occupies */
static volatile LONG pad_connected;
static BYTE pad_report[64];
static DWORD pad_packet;
static WCHAR pad_want[24] = L"VID_054C&PID_0CE6";
static WORD pad_product = 0x0ce6;

/* The bridged pad's HID interface: the raw Sony device, never wine's own
 * XInput-compatible node (&IG_). */
static HANDLE open_pad(void)
{
    GUID hid;
    HDEVINFO set;
    SP_DEVICE_INTERFACE_DATA iface;
    DWORD i, size;
    HANDLE found = INVALID_HANDLE_VALUE;

    HidD_GetHidGuid(&hid);
    set = SetupDiGetClassDevsW(&hid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return found;
    iface.cbSize = sizeof(iface);
    for (i = 0; found == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, NULL, &hid, i, &iface); i++)
    {
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *detail;
        WCHAR upper[512];
        PHIDP_PREPARSED_DATA pre;
        HIDP_CAPS caps;
        HANDLE h;

        size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, NULL, 0, &size, NULL);
        if (!size || !(detail = HeapAlloc(GetProcessHeap(), 0, size))) continue;
        detail->cbSize = sizeof(*detail);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, NULL, NULL))
        {
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }
        lstrcpynW(upper, detail->DevicePath, 512);
        CharUpperW(upper);
        if (!wcsstr(upper, pad_want) || wcsstr(upper, L"&IG_"))
        {
            HeapFree(GetProcessHeap(), 0, detail);
            continue;
        }
        h = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            h = CreateFileW(detail->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE && HidD_GetPreparsedData(h, &pre))
        {
            if (HidP_GetCaps(pre, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 1 && caps.Usage == 5
                && caps.InputReportByteLength >= 11)
            {
                found = h;
                logf_("pad as XInput: reading %ls (input %u bytes)", detail->DevicePath, caps.InputReportByteLength);
            }
            HidD_FreePreparsedData(pre);
        }
        if (found != h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        HeapFree(GetProcessHeap(), 0, detail);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static DWORD WINAPI pad_reader(void *first)
{
    BYTE buf[128];
    HANDLE opened = first;
    for (;;)
    {
        HANDLE h = opened ? opened : open_pad();
        DWORD got;
        opened = NULL;
        if (h == INVALID_HANDLE_VALUE) { Sleep(1000); continue; }
        while (ReadFile(h, buf, sizeof(buf), &got, NULL))
        {
            if (got < 11 || buf[0] != 0x01) continue;
            EnterCriticalSection(&pad_lock);
            /* Bytes 7 and 12+ are a counter, timestamps and motion: they move on
             * every report and are not part of the XInput state. */
            if (memcmp(pad_report + 1, buf + 1, 6) || memcmp(pad_report + 8, buf + 8, 3)) pad_packet++;
            memcpy(pad_report, buf, got < sizeof(pad_report) ? got : sizeof(pad_report));
            LeaveCriticalSection(&pad_lock);
            {
                static LONG said;
                InterlockedExchange(&pad_connected, 1);
                if (InterlockedIncrement(&said) == 1) logf_("pad as XInput: first input report");
            }
        }
        InterlockedExchange(&pad_connected, 0);
        logf_("pad as XInput: read failed (%lu), reopening", GetLastError());
        CloseHandle(h);
        Sleep(500);
    }
    return 0;
}

static SHORT axis(BYTE v)
{
    int x = ((int)v - 128) * 257;
    return (SHORT)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
}

static void pad_state(XSTATE *state, BOOL guide)
{
    static const WORD hat[9] = { BTN_DPAD_UP, BTN_DPAD_UP | BTN_DPAD_RIGHT, BTN_DPAD_RIGHT,
                                 BTN_DPAD_DOWN | BTN_DPAD_RIGHT, BTN_DPAD_DOWN, BTN_DPAD_DOWN | BTN_DPAD_LEFT,
                                 BTN_DPAD_LEFT, BTN_DPAD_UP | BTN_DPAD_LEFT, 0 };
    BYTE r[64];
    WORD b = 0;
    DWORD packet;

    EnterCriticalSection(&pad_lock);
    memcpy(r, pad_report, sizeof(r));
    packet = pad_packet;
    LeaveCriticalSection(&pad_lock);

    b |= hat[(r[8] & 0x0f) > 8 ? 8 : (r[8] & 0x0f)];
    if (r[8] & 0x10) b |= BTN_X;
    if (r[8] & 0x20) b |= BTN_A;
    if (r[8] & 0x40) b |= BTN_B;
    if (r[8] & 0x80) b |= BTN_Y;
    if (r[9] & 0x01) b |= BTN_LSHOULDER;
    if (r[9] & 0x02) b |= BTN_RSHOULDER;
    /* XInput has one Back/View button; the touchpad click is Back too, as
     * Steam maps a PlayStation pad for an Xbox title. */
    if ((r[9] & 0x10) || (r[10] & 0x02)) b |= BTN_BACK;
    if (r[9] & 0x20) b |= BTN_START;
    if (r[9] & 0x40) b |= BTN_LTHUMB;
    if (r[9] & 0x80) b |= BTN_RTHUMB;
    if (guide && (r[10] & 0x01)) b |= BTN_GUIDE;

    memset(state, 0, sizeof(*state));
    state->dwPacketNumber = packet;
    state->Gamepad.wButtons = b;
    state->Gamepad.bLeftTrigger = r[5];
    state->Gamepad.bRightTrigger = r[6];
    state->Gamepad.sThumbLX = axis(r[1]);
    state->Gamepad.sThumbLY = (SHORT)(-1 - axis(r[2]));
    state->Gamepad.sThumbRX = axis(r[3]);
    state->Gamepad.sThumbRY = (SHORT)(-1 - axis(r[4]));
}

static BOOL CALLBACK init_pad(INIT_ONCE *o, void *param, void **ctx)
{
    char flag[8], product[8], device[32];
    ORIG(get_state_fn, "XInputGetState");
    XSTATE probe;
    int i;

    InitializeCriticalSection(&pad_lock);
    if (!GetEnvironmentVariableA("DSB_PORT", NULL, 0)) return TRUE;
    if (!GetEnvironmentVariableA("DSB_XINPUT_PAD", flag, sizeof(flag)) || strcmp(flag, "1"))
    {
        logf_("pad as XInput: off (DSB_XINPUT_PAD %s)", GetEnvironmentVariableA("DSB_XINPUT_PAD", NULL, 0) ? flag : "unset");
        return TRUE;
    }
    /* The pad is built from a DualSense's own input reports; another backend's
     * pad reaches XInput through wine. */
    if (GetEnvironmentVariableA("DSB_DEVICE", device, sizeof(device)) && strcmp(device, "dualsense"))
    {
        logf_("pad as XInput: off (DSB_DEVICE %s builds no XInput pad here)", device);
        return TRUE;
    }
    if (GetEnvironmentVariableA("DSB_PRODUCT", product, sizeof(product)) == 4
        && !_stricmp(product, "0df2"))
    {
        lstrcpyW(pad_want, L"VID_054C&PID_0DF2");
        pad_product = 0x0df2;
    }
    /* The first slot wine leaves empty. */
    for (i = 0; i < 4 && pad_index < 0; i++)
        if (!p_ || p_(i, &probe) != ERROR_SUCCESS) pad_index = i;
    if (pad_index < 0) { logf_("pad as XInput: every XInput slot is taken"); return TRUE; }
    /* Open the pad before answering: a game that asks once at start-up
     * (Devil May Cry 5 asks XInputGetCapabilities and then waits for a
     * device-arrival message) must find it connected on that first call. The
     * state reads neutral until the first report arrives. */
    pad_report[0] = 0x01;
    pad_report[1] = pad_report[2] = pad_report[3] = pad_report[4] = 0x80;
    pad_report[8] = 0x08;
    {
        HANDLE h = open_pad();
        if (h != INVALID_HANDLE_VALUE) pad_connected = 1;
        pad_enabled = TRUE;
        pin_self();
        CloseHandle(CreateThread(NULL, 0, pad_reader, h != INVALID_HANDLE_VALUE ? h : NULL, 0, NULL));
    }
    logf_("pad as XInput: on, slot %d, %ls", pad_index, pad_want);
    return TRUE;
}

static BOOL ours(DWORD index)
{
    InitOnceExecuteOnce(&pad_once, init_pad, NULL, NULL);
    return pad_enabled && (int)index == pad_index;
}

static void pad_caps(XCAPS *caps)
{
    memset(caps, 0, sizeof(*caps));
    caps->Type = 1;     /* XINPUT_DEVTYPE_GAMEPAD */
    caps->SubType = 1;  /* XINPUT_DEVSUBTYPE_GAMEPAD */
    caps->Gamepad.wButtons = 0xf7ff;
    caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 0xff;
    caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = (SHORT)0xffc0;
    caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = (SHORT)0xffc0;
    caps->Vibration.wLeftMotorSpeed = caps->Vibration.wRightMotorSpeed = 0xff;
}

/* XInputEnable(FALSE) stops vibration and holds what the game asks for until
 * it is enabled again (wine's xinput does the same with its own device). */
static BOOL vibration_disabled;
static BYTE wanted_left, wanted_right;

static void motors_to_broker(const XVIB *vib)
{
    BYTE left = vib->wLeftMotorSpeed >> 8, right = vib->wRightMotorSpeed >> 8;
    EnterCriticalSection(&lock);
    wanted_left = left;
    wanted_right = right;
    if (!vibration_disabled && (left != motor_left || right != motor_right))
        send_motors_locked(left, right);
    LeaveCriticalSection(&lock);
}

/* ---------------------------------------------------------------- exports */

DWORD WINAPI XInputSetState(DWORD index, XVIB *vib)
{
    ORIG(set_state_fn, "XInputSetState");
    DWORD r;
    static LONG calls;

    InitOnceExecuteOnce(&once, init_broker, NULL, NULL);
    if (ours(index))
    {
        static LONG said;
        if (!pad_connected) return ERROR_DEVICE_NOT_CONNECTED;
        if (InterlockedIncrement(&said) == 1) logf_("pad as XInput: first vibration request");
        {
            static LONG nonzero;
            if (vib && (vib->wLeftMotorSpeed || vib->wRightMotorSpeed) && InterlockedIncrement(&nonzero) == 1)
                logf_("pad as XInput: first vibration with motors %u/%u", vib->wLeftMotorSpeed >> 8,
                      vib->wRightMotorSpeed >> 8);
        }
        if (broker && vib) motors_to_broker(vib);
        return ERROR_SUCCESS;
    }
    if (broker && vib && broker_only && !steam_pad && is_bridged(index))
    {
        motors_to_broker(vib);
        return ERROR_SUCCESS;
    }
    r = p_ ? p_(index, vib) : ERROR_DEVICE_NOT_CONNECTED;
    if (InterlockedIncrement(&calls) == 1)
        logf_("XInputSetState(%lu) first call -> %lu", index, r);
    if (broker && vib && r == ERROR_SUCCESS && (steam_pad || is_bridged(index)))
    {
        static LONG nonzero;
        if ((vib->wLeftMotorSpeed || vib->wRightMotorSpeed) && InterlockedIncrement(&nonzero) == 1)
            logf_("pad %lu: first vibration with motors %u/%u%s", index, vib->wLeftMotorSpeed >> 8,
                  vib->wRightMotorSpeed >> 8, steam_pad ? " (Steam Input pad)" : "");
        motors_to_broker(vib);
    }
    return r;
}

DWORD WINAPI XInputGetState(DWORD index, XSTATE *state)
{
    ORIG(get_state_fn, "XInputGetState");
    DWORD r;
    static LONG said[4];
    if (ours(index))
    {
        if (!pad_connected || !state) r = ERROR_DEVICE_NOT_CONNECTED;
        else { pad_state(state, FALSE); r = ERROR_SUCCESS; }
    }
    else
        r = p_ ? p_(index, state) : ERROR_DEVICE_NOT_CONNECTED;
    if (index < 4 && InterlockedIncrement(&said[index]) == 1)
        logf_("XInputGetState(%lu) first call -> %lu%s", index, r, ours(index) ? " (the pad)" : "");
    return r;
}

DWORD WINAPI XInputGetStateEx(DWORD index, XSTATE *state)
{
    ORIG(get_state_fn, "XInputGetStateEx");
    if (ours(index))
    {
        if (!pad_connected || !state) return ERROR_DEVICE_NOT_CONNECTED;
        pad_state(state, TRUE);
        return ERROR_SUCCESS;
    }
    return p_ ? p_(index, state) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetCapabilities(DWORD index, DWORD flags, XCAPS *caps)
{
    ORIG(get_caps_fn, "XInputGetCapabilities");
    DWORD r;
    static LONG said[4];
    if (ours(index))
    {
        if (!pad_connected || !caps) r = ERROR_DEVICE_NOT_CONNECTED;
        else { pad_caps(caps); r = ERROR_SUCCESS; }
    }
    else
        r = p_ ? p_(index, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
    if (index < 4 && InterlockedIncrement(&said[index]) == 1)
        logf_("XInputGetCapabilities(%lu) first call -> %lu%s", index, r, ours(index) ? " (the pad)" : "");
    return r;
}

DWORD WINAPI XInputGetCapabilitiesEx(DWORD unk, DWORD index, DWORD flags, XCAPSEX *caps)
{
    ORIG(get_caps_ex_fn, "XInputGetCapabilitiesEx");
    if (ours(index))
    {
        if (!pad_connected || !caps) return ERROR_DEVICE_NOT_CONNECTED;
        memset(caps, 0, sizeof(*caps));
        pad_caps(&caps->Capabilities);
        caps->VendorId = 0x054c;
        caps->ProductId = pad_product;
        caps->VersionNumber = 0x0100;
        return ERROR_SUCCESS;
    }
    return p_ ? p_(unk, index, flags, caps) : ERROR_DEVICE_NOT_CONNECTED;
}

void WINAPI XInputEnable(BOOL enable)
{
    ORIG(enable_fn, "XInputEnable");
    InitOnceExecuteOnce(&once, init_broker, NULL, NULL);
    if (broker)
    {
        EnterCriticalSection(&lock);
        vibration_disabled = !enable;
        if (!enable && (motor_left || motor_right)) send_motors_locked(0, 0);
        else if (enable && (wanted_left != motor_left || wanted_right != motor_right))
            send_motors_locked(wanted_left, wanted_right);
        LeaveCriticalSection(&lock);
    }
    if (p_) p_(enable);
}

DWORD WINAPI XInputGetBatteryInformation(DWORD index, BYTE type, void *info)
{
    ORIG(battery_fn, "XInputGetBatteryInformation");
    if (ours(index))
    {
        BYTE *b = info;
        if (!pad_connected || !b) return ERROR_DEVICE_NOT_CONNECTED;
        b[0] = type == 0 ? 3 : 0;   /* gamepad: NiMH; headset: disconnected */
        b[1] = type == 0 ? 3 : 0;   /* full */
        return ERROR_SUCCESS;
    }
    return p_ ? p_(index, type, info) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetKeystroke(DWORD index, DWORD reserved, void *keystroke)
{
    ORIG(keystroke_fn, "XInputGetKeystroke");
    if (ours(index)) return pad_connected ? ERROR_EMPTY : ERROR_DEVICE_NOT_CONNECTED;
    return p_ ? p_(index, reserved, keystroke) : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD WINAPI XInputGetAudioDeviceIds(DWORD index, WCHAR *render, UINT *render_count,
                                     WCHAR *capture, UINT *capture_count)
{
    ORIG(audio_ids_fn, "XInputGetAudioDeviceIds");
    return p_ ? p_(index, render, render_count, capture, capture_count) : ERROR_DEVICE_NOT_CONNECTED;
}

/* xinput1_3 only. */
DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD index, GUID *render, GUID *capture)
{
    typedef DWORD (WINAPI *fn)(DWORD, GUID *, GUID *);
    static fn p_;
    if (!p_) p_ = (fn)orig_proc("XInputGetDSoundAudioDeviceGuids");
    if (ours(index)) return pad_connected ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
    return p_ ? p_(index, render, capture) : ERROR_DEVICE_NOT_CONNECTED;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        t0 = GetTickCount();
        DisableThreadLibraryCalls(inst);
        if (GetEnvironmentVariableA("DSB_PORT", NULL, 0)) logf_("loaded in a controller-bridge session");
    }
    return TRUE;
}
