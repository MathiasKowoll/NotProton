/*
 * mmdevapi-container.c -- give the bridge's haptic endpoint the pad's container.
 *
 * Staged into a prefix as mmdevapi.dll, with the runner's own mmdevapi.dll
 * kept beside it as mmdevapi_np.dll. Every export but DllGetClassObject is a
 * forwarder to the original.
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
     * earlier pad is not this session's. A session that waited for its pad
     * does not know the model (DSB_PRODUCT unset) and takes either one. */
    WCHAR want[2][24] = { L"VID_054C&PID_0CE6", L"VID_054C&PID_0DF2" };
    int wants = 2;
    char product[8];
    DWORD n = GetEnvironmentVariableA("DSB_PRODUCT", product, sizeof(product));
    if (n == 4)
    {
        MultiByteToWideChar(CP_ACP, 0, product, 4, want[0] + 13, 5);
        want[0][17] = 0;
        CharUpperW(want[0]);
        wants = 1;
    }

    info.cbSize = sizeof(info);
    for (i = 0; set != INVALID_HANDLE_VALUE && !found && SetupDiEnumDeviceInfo(set, i, &info); i++)
    {
        HKEY key;
        if (!SetupDiGetDeviceInstanceIdW(set, &info, id, ARRAY_SIZE(id), NULL)) continue;
        CharUpperW(id);
        if (wcsncmp(id, prefix, wcslen(prefix))) continue;
        if (!wcsstr(id, want[0]) && (wants < 2 || !wcsstr(id, want[1]))) continue;
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
    BOOL ok, first, refresh;

    /* The first reading holds the lock, so no caller answers before there is
     * one; later ones walk the device tree without it, so a game reading other
     * properties never waits on the walk. One caller takes each refresh. */
    EnterCriticalSection(&lock);
    first = !pad_read_at;
    refresh = first || now - pad_read_at > 2000;
    if (refresh) pad_read_at = now | 1;
    if (refresh && !first) LeaveCriticalSection(&lock);
    if (refresh)
    {
        WCHAR id[512], text[40];
        GUID guid;
        BOOL found = container_of(L"HID\\", &guid, id, ARRAY_SIZE(id))
                     || container_of(L"USB\\", &guid, id, ARRAY_SIZE(id));

        if (!first) EnterCriticalSection(&lock);
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

static HRESULT WINAPI my_GetValue(IPropertyStore *store, REFPROPERTYKEY key, PROPVARIANT *pv)
{
    HRESULT hr = real_GetValue(store, key, pv);
    GUID container;

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

/* Every endpoint's property store shares wine's one vtable: patch it once,
 * through a store opened on any endpoint. */
/* Keeps this DLL loaded for the life of the process: the property store patched
 * below points into it. Called from the first use of the
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
            IMMDevice_Release(device);
        }
        IMMDeviceCollection_Release(all);
    }
    if (FAILED(hr)) logf_("endpoint walk failed (0x%08lx)", hr);
    IMMDeviceEnumerator_Release(devices);
    return TRUE;
}

/* ---------------------------------------------------------------- exports */

HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    if (!load_original()) return CLASS_E_CLASSNOTAVAILABLE;
    InitOnceExecuteOnce(&once, install, NULL, NULL);
    return real_DllGetClassObject(clsid, iid, out);
}
