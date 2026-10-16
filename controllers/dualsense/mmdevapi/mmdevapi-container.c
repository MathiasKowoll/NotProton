/*
 * mmdevapi-container.c -- give the bridge's haptic endpoint the pad's container.
 *
 * Installed into a runner as mmdevapi.dll, with the runner's own mmdevapi.dll
 * kept beside it as mmdevapi_np.dll. Every export is the original's.
 *
 * On Windows a DualSense on a cable is one container: its HID interface and
 * its four-channel audio endpoint report the same DEVPKEY_Device_ContainerId,
 * and that is how a game finds the endpoint that drives the pad's haptics.
 * Measured with Spider-Man 2 (Wwise's AkQuadAudioHapticsSink) on CrossOver
 * Preview: the game read DEVPKEY_Device_ContainerId from every endpoint ~590
 * times, wine answered "no such value" every time, and the bridge's endpoint
 * was never opened. Wine's endpoints have no container, and its property
 * store cannot hold a VT_CLSID even if one were written to the registry.
 *
 * So, in a controller-bridge session only (DSB_PORT set), the endpoints'
 * property store is watched: when DEVPKEY_Device_ContainerId is asked of the
 * bridge's endpoint (named "... (Bluetooth Bridge)" by AudioFacade) and wine
 * has no value, the answer is the ContainerId wine gave the bridged pad in this
 * session, read from its device node. Every other read is wine's, unchanged.
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

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
    n = snprintf(buf, sizeof(buf), "[%7lu ms] [mmdevapi] ", GetTickCount());
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

static HMODULE orig;
typedef HRESULT (WINAPI *get_class_object_fn)(REFCLSID, REFIID, void **);
static get_class_object_fn real_DllGetClassObject;

static BOOL load_original(void)
{
    if (!orig)
    {
        orig = LoadLibraryA("mmdevapi_np.dll");
        if (!orig) logf_("mmdevapi_np.dll did not load (error %lu)", GetLastError());
    }
    if (orig && !real_DllGetClassObject)
        real_DllGetClassObject = (get_class_object_fn)GetProcAddress(orig, "DllGetClassObject");
    return real_DllGetClassObject != NULL;
}

/* ---------------------------------------------------------- the pad's id */

static const PROPERTYKEY container_key =
    { { 0x8c7ed206, 0x3f8a, 0x4827, { 0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c } }, 2 };
/* DEVPKEY_DeviceInterface_FriendlyName: the CoreAudio device's own name, which
 * wine keeps in the prefix. Its driver device key ({b3f8fa53-...},2) is not
 * kept there, so a store opened by a game cannot answer it. */
static const PROPERTYKEY name_key =
    { { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };

static CRITICAL_SECTION lock;
static GUID pad_guid;
static BOOL pad_known;
static DWORD pad_read_at;

/* ContainerId of the DualSense (standard or Edge) as the game sees it: the
 * HID node wine made for it this session, or its USB parent. Wine keeps older
 * nodes (BTHENUM from the Bluetooth route) listed as present, with containers
 * of their own; those are never the one the game compares against. Read again
 * after two seconds: the container changes per session. */
static BOOL container_of(const WCHAR *prefix, GUID *out, WCHAR *found_id, DWORD found_len)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA info;
    WCHAR id[512], path[600], value[64];
    DWORD i, size;
    BOOL found = FALSE;

    /* The bridge names the paired model; another Sony node left from an
     * earlier pad is not this session's. */
    WCHAR want[32] = L"VID_054C&PID_";
    char product[8];
    DWORD n = GetEnvironmentVariableA("DSB_PRODUCT", product, sizeof(product));
    if (n == 4)
    {
        MultiByteToWideChar(CP_ACP, 0, product, 4, want + 13, 5);
        want[17] = 0;
        CharUpperW(want);
    }
    else
        lstrcpyW(want, L"VID_054C&PID_");

    info.cbSize = sizeof(info);
    for (i = 0; set != INVALID_HANDLE_VALUE && !found && SetupDiEnumDeviceInfo(set, i, &info); i++)
    {
        HKEY key;
        if (!SetupDiGetDeviceInstanceIdW(set, &info, id, ARRAY_SIZE(id), NULL)) continue;
        CharUpperW(id);
        if (wcsncmp(id, prefix, wcslen(prefix))) continue;
        if (!wcsstr(id, want)) continue;
        swprintf(path, ARRAY_SIZE(path), L"System\\CurrentControlSet\\Enum\\%ls", id);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key)) continue;
        size = sizeof(value);
        if (!RegQueryValueExW(key, L"ContainerId", NULL, NULL, (BYTE *)value, &size)
            && SUCCEEDED(CLSIDFromString(value, out)))
        {
            found = TRUE;
            lstrcpynW(found_id, id, found_len);
        }
        RegCloseKey(key);
    }
    if (set != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    return found;
}

static BOOL pad_container(GUID *out)
{
    DWORD now = GetTickCount();
    BOOL ok;

    EnterCriticalSection(&lock);
    if (!pad_read_at || now - pad_read_at > 2000)
    {
        WCHAR id[512], text[40];
        GUID guid;
        BOOL found = container_of(L"HID\\", &guid, id, ARRAY_SIZE(id))
                     || container_of(L"USB\\", &guid, id, ARRAY_SIZE(id));

        pad_read_at = now;
        if (found && (!pad_known || !IsEqualGUID(&guid, &pad_guid)))
        {
            StringFromGUID2(&guid, text, ARRAY_SIZE(text));
            logf_("pad %ls, container %ls", id, text);
        }
        if (!found && pad_known) logf_("the pad's device node is gone");
        if (found) pad_guid = guid;
        pad_known = found;
    }
    ok = pad_known;
    if (ok) *out = pad_guid;
    LeaveCriticalSection(&lock);
    return ok;
}

/* ------------------------------------------------------ property store */

typedef HRESULT (WINAPI *get_value_fn)(IPropertyStore *, REFPROPERTYKEY, PROPVARIANT *);
static get_value_fn real_GetValue;

static BOOL is_bridge(IPropertyStore *store)
{
    PROPVARIANT pv;
    BOOL bridge = FALSE;
    PropVariantInit(&pv);
    if (SUCCEEDED(real_GetValue(store, &name_key, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
        bridge = wcsstr(pv.pwszVal, L"(Bluetooth Bridge)") != NULL;
    PropVariantClear(&pv);
    return bridge;
}

/* Measurement: every distinct (endpoint, key) a game reads, and what wine had. */
static void note_read(IPropertyStore *store, REFPROPERTYKEY key, HRESULT hr, const PROPVARIANT *pv)
{
    static struct { WCHAR dev[64]; GUID fmtid; DWORD pid; } seen[160];
    static LONG count;
    PROPVARIANT path;
    WCHAR dev[64] = L"?";
    LONG i, n;

    if (!key) return;
    PropVariantInit(&path);
    if (SUCCEEDED(real_GetValue(store, &name_key, &path)) && path.vt == VT_LPWSTR && path.pwszVal)
        lstrcpynW(dev, path.pwszVal, ARRAY_SIZE(dev));
    PropVariantClear(&path);
    EnterCriticalSection(&lock);
    n = count;
    for (i = 0; i < n; i++)
        if (seen[i].pid == key->pid && IsEqualGUID(&seen[i].fmtid, &key->fmtid) && !wcscmp(seen[i].dev, dev))
            break;
    if (i == n && n < (LONG)ARRAY_SIZE(seen))
    {
        WCHAR g[40];
        lstrcpyW(seen[n].dev, dev);
        seen[n].fmtid = key->fmtid;
        seen[n].pid = key->pid;
        count = n + 1;
        StringFromGUID2(&key->fmtid, g, 40);
        logf_("read %ls,%lu of [%ls] -> 0x%08lx vt %u", g, key->pid, dev, hr, pv ? pv->vt : 0);
    }
    LeaveCriticalSection(&lock);
}

static HRESULT WINAPI my_GetValue(IPropertyStore *store, REFPROPERTYKEY key, PROPVARIANT *pv)
{
    HRESULT hr = real_GetValue(store, key, pv);
    GUID container;

    note_read(store, key, hr, pv);

    if (SUCCEEDED(hr) && key && pv && pv->vt == VT_EMPTY
        && IsEqualGUID(&key->fmtid, &container_key.fmtid) && key->pid == container_key.pid
        && is_bridge(store) && pad_container(&container)
        && (pv->puuid = CoTaskMemAlloc(sizeof(GUID))))
    {
        static LONG said;
        *pv->puuid = container;
        pv->vt = VT_CLSID;
        if (InterlockedIncrement(&said) == 1)
            logf_("the bridge's haptic endpoint answered with the pad's container");
    }
    return hr;
}

static BOOL patch_slot(void **vt, int slot, void *mine, void **saved)
{
    DWORD old;
    if (vt[slot] == mine) return TRUE;
    if (!VirtualProtect(&vt[slot], sizeof(void *), PAGE_READWRITE, &old)) return FALSE;
    *saved = vt[slot];
    vt[slot] = mine;
    VirtualProtect(&vt[slot], sizeof(void *), old, &old);
    return TRUE;
}

static void watch_clients(IMMDevice *device);

/* Every endpoint's property store shares wine's one vtable: patch it once,
 * through a store opened on any endpoint. */
/* Keeps this DLL loaded for the life of the process: the property stores and
 * clients patched below point into it. Called from the first use of the
 * enumerator, never from DllMain. */
static void pin_self(void)
{
    HMODULE self;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCWSTR)(void *)pin_self, &self))
        logf_("could not pin this DLL (error %lu); it must not be unloaded", GetLastError());
}

static BOOL CALLBACK install(INIT_ONCE *once, void *param, void **ctx)
{
    IClassFactory *factory = NULL;
    IMMDeviceEnumerator *devices = NULL;
    IMMDeviceCollection *all = NULL;
    IMMDevice *device = NULL;
    IPropertyStore *store = NULL;
    char port[16];
    UINT count = 0;
    HRESULT hr;

    InitializeCriticalSection(&lock);
    if (!GetEnvironmentVariableA("DSB_PORT", port, sizeof(port))) return TRUE;
    pin_self();
    if (FAILED(hr = real_DllGetClassObject(&CLSID_MMDeviceEnumerator, &IID_IClassFactory, (void **)&factory)))
    {
        logf_("no device enumerator class (0x%08lx)", hr);
        return TRUE;
    }
    hr = IClassFactory_CreateInstance(factory, NULL, &IID_IMMDeviceEnumerator, (void **)&devices);
    IClassFactory_Release(factory);
    if (FAILED(hr)) { logf_("no device enumerator (0x%08lx)", hr); return TRUE; }
    if (SUCCEEDED(hr = IMMDeviceEnumerator_EnumAudioEndpoints(devices, eAll, DEVICE_STATEMASK_ALL, &all)))
    {
        IMMDeviceCollection_GetCount(all, &count);
        if (count && SUCCEEDED(hr = IMMDeviceCollection_Item(all, 0, &device)))
        {
            if (SUCCEEDED(hr = IMMDevice_OpenPropertyStore(device, STGM_READ, &store)))
            {
                void **vt = *(void ***)store;
                if (patch_slot(vt, 5, (void *)my_GetValue, (void **)&real_GetValue))
                    logf_("endpoint property store watched (%u endpoints)", count);
                else
                    logf_("could not patch the property store");
                IPropertyStore_Release(store);
            }
            watch_clients(device);
            IMMDevice_Release(device);
        }
        IMMDeviceCollection_Release(all);
    }
    if (FAILED(hr)) logf_("endpoint walk failed (0x%08lx)", hr);
    IMMDeviceEnumerator_Release(devices);
    return TRUE;
}

/* --------------------------------------------- measurement: the client */

/* What a game does with an audio client on the bridge's endpoint: whether
 * Initialize succeeds and whether it ever starts the stream. */
typedef HRESULT (WINAPI *activate_fn)(IMMDevice *, REFIID, DWORD, PROPVARIANT *, void **);
typedef HRESULT (WINAPI *initialize_fn)(IAudioClient *, AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME,
                                        REFERENCE_TIME, const WAVEFORMATEX *, LPCGUID);
typedef HRESULT (WINAPI *client_fn)(IAudioClient *);
typedef HRESULT (WINAPI *get_service_fn)(IAudioClient *, REFIID, void **);
typedef HRESULT (WINAPI *format_supported_fn)(IAudioClient *, AUDCLNT_SHAREMODE, const WAVEFORMATEX *,
                                              WAVEFORMATEX **);
static activate_fn real_Activate;
static initialize_fn real_Initialize;
static client_fn real_Start;
static get_service_fn real_GetService;
static format_supported_fn real_IsFormatSupported;
static void *bridge_clients[64];
static LONG bridge_next, activations, initializes, starts;

static BOOL bridge_client(void *client)
{
    int i;
    for (i = 0; i < 64; i++) if (bridge_clients[i] == client) return TRUE;
    return FALSE;
}

static HRESULT WINAPI my_Activate(IMMDevice *device, REFIID iid, DWORD ctx, PROPVARIANT *params, void **out)
{
    HRESULT hr = real_Activate(device, iid, ctx, params, out);
    IPropertyStore *store = NULL;
    BOOL bridge = FALSE;

    if (SUCCEEDED(hr) && out && *out && SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &store)))
    {
        bridge = is_bridge(store);
        IPropertyStore_Release(store);
    }
    if (bridge)
    {
        WCHAR g[40];
        LONG n = InterlockedIncrement(&activations);
        StringFromGUID2(iid, g, 40);
        if (n <= 5 || !(n % 100)) logf_("bridge endpoint activated #%ld as %ls -> 0x%08lx", n, g, hr);
        bridge_clients[InterlockedIncrement(&bridge_next) & 63] = *out;
    }
    return hr;
}

static HRESULT WINAPI my_Initialize(IAudioClient *client, AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME duration,
                                    REFERENCE_TIME period, const WAVEFORMATEX *fmt, LPCGUID session)
{
    HRESULT hr = real_Initialize(client, mode, flags, duration, period, fmt, session);
    if (bridge_client(client))
    {
        LONG n = InterlockedIncrement(&initializes);
        if (n <= 5 || !(n % 100) || FAILED(hr))
        {
            static LONG failures;
            if (SUCCEEDED(hr) || InterlockedIncrement(&failures) <= 10)
                logf_("bridge Initialize #%ld mode %d flags %08lx dur %lld: %u ch %lu Hz %u bit tag %04x -> 0x%08lx",
                      n, mode, flags, (long long)duration, fmt ? fmt->nChannels : 0, fmt ? fmt->nSamplesPerSec : 0,
                      fmt ? fmt->wBitsPerSample : 0, fmt ? fmt->wFormatTag : 0, hr);
        }
    }
    return hr;
}

static HRESULT WINAPI my_IsFormatSupported(IAudioClient *client, AUDCLNT_SHAREMODE mode, const WAVEFORMATEX *fmt,
                                           WAVEFORMATEX **closest)
{
    HRESULT hr = real_IsFormatSupported(client, mode, fmt, closest);
    static LONG said;
    if (bridge_client(client) && InterlockedIncrement(&said) <= 10)
        logf_("bridge IsFormatSupported mode %d: %u ch %lu Hz %u bit -> 0x%08lx", mode,
              fmt ? fmt->nChannels : 0, fmt ? fmt->nSamplesPerSec : 0, fmt ? fmt->wBitsPerSample : 0, hr);
    return hr;
}

static HRESULT WINAPI my_Start(IAudioClient *client)
{
    HRESULT hr = real_Start(client);
    if (bridge_client(client))
    {
        LONG n = InterlockedIncrement(&starts);
        if (n <= 5 || FAILED(hr)) logf_("bridge Start #%ld -> 0x%08lx", n, hr);
    }
    return hr;
}

static HRESULT WINAPI my_GetService(IAudioClient *client, REFIID iid, void **out)
{
    HRESULT hr = real_GetService(client, iid, out);
    static LONG said;
    if (bridge_client(client) && InterlockedIncrement(&said) <= 10)
    {
        WCHAR g[40];
        StringFromGUID2(iid, g, 40);
        logf_("bridge GetService %ls -> 0x%08lx", g, hr);
    }
    return hr;
}

static void watch_clients(IMMDevice *device)
{
    IAudioClient *client = NULL;
    void **vt = *(void ***)device;
    HRESULT hr;

    if (!patch_slot(vt, 3, (void *)my_Activate, (void **)&real_Activate)) return;
    if (FAILED(hr = real_Activate(device, &IID_IAudioClient, CLSCTX_INPROC_SERVER, NULL, (void **)&client)))
    {
        logf_("no audio client to watch (0x%08lx)", hr);
        return;
    }
    vt = *(void ***)client;
    patch_slot(vt, 3, (void *)my_Initialize, (void **)&real_Initialize);
    patch_slot(vt, 7, (void *)my_IsFormatSupported, (void **)&real_IsFormatSupported);
    patch_slot(vt, 10, (void *)my_Start, (void **)&real_Start);
    patch_slot(vt, 14, (void *)my_GetService, (void **)&real_GetService);
    IAudioClient_Release(client);
    logf_("audio clients watched");
}

/* ---------------------------------------------------------------- exports */

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    if (!load_original()) return CLASS_E_CLASSNOTAVAILABLE;
    InitOnceExecuteOnce(&once, install, NULL, NULL);
    return real_DllGetClassObject(clsid, iid, out);
}
