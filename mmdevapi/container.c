/*
 * Wine's mmdevapi, plus the one answer it cannot give: the ContainerId of a
 * DualSense's own audio endpoint.
 *
 * On Windows a DualSense on a cable is one container: its HID device and its
 * four-channel audio endpoint report the same DEVPKEY_Device_ContainerId, and
 * games that play haptics as audio find the pad's endpoint that way. Wine's
 * endpoints have no container, and its property store cannot hold a VT_CLSID,
 * so those games never open the endpoint and the pad's haptics stay silent.
 *
 * A runner carries this as x86_64-windows/mmdevapi.dll, with Wine's own kept
 * beside it as mmdevapi_np.dll without its builtin signature (Wine maps a file
 * that has one by its export name, mmdevapi.dll: this DLL). Every export calls
 * Wine's. The endpoints' property store gets one hook: when the container of
 * an active four-channel endpoint named after a DualSense is asked for and
 * Wine has none, the answer is the container of that model's connected HID
 * device. Every other read is Wine's, unchanged.
 */

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propsys.h>
#include <setupapi.h>
#include <wchar.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static const PROPERTYKEY container_key = /* DEVPKEY_Device_ContainerId */
    { { 0x8c7ed206, 0x3f8a, 0x4827, { 0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c } }, 2 };
static const PROPERTYKEY name_key = /* DEVPKEY_DeviceInterface_FriendlyName */
    { { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };
static const PROPERTYKEY format_key = /* PKEY_AudioEngine_DeviceFormat */
    { { 0xf19f064d, 0x082c, 0x4e27, { 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c } }, 0 };
static const PROPERTYKEY guid_key = /* PKEY_AudioEndpoint_GUID */
    { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 4 };

typedef HRESULT (WINAPI *get_class_object_fn)(REFCLSID, REFIID, void **);
typedef HRESULT (WINAPI *get_value_fn)(IPropertyStore *, REFPROPERTYKEY, PROPVARIANT *);
static get_class_object_fn real_DllGetClassObject;
static get_value_fn real_GetValue;

/* Wine's own mmdevapi. Wine loads a builtin by name only when the prefix has a
 * file of that name, so it is loaded by path, from the directories Wine
 * itself searches for builtins (WINEDLLDIR0, 1, ...). */
static HMODULE wine_mmdevapi(void)
{
    static HMODULE module;
    WCHAR var[16], dir[MAX_PATH], path[MAX_PATH + 40];
    DWORD i, n;

    for (i = 0; !module; i++)
    {
        swprintf(var, ARRAY_SIZE(var), L"WINEDLLDIR%lu", i);
        if (!(n = GetEnvironmentVariableW(var, dir, ARRAY_SIZE(dir)))) break;
        if (n >= ARRAY_SIZE(dir)) continue;
        if (!wcsncmp(dir, L"\\??\\", 4)) dir[1] = '\\';
        swprintf(path, ARRAY_SIZE(path), L"%ls\\x86_64-windows\\mmdevapi_np.dll", dir);
        if (!(module = LoadLibraryW(path)))
        {
            swprintf(path, ARRAY_SIZE(path), L"%ls\\mmdevapi_np.dll", dir);
            module = LoadLibraryW(path);
        }
    }
    return module;
}

static void *wine_export(const char *name)
{
    HMODULE module = wine_mmdevapi();
    return module ? (void *)GetProcAddress(module, name) : NULL;
}

static BOOL same_key(REFPROPERTYKEY a, const PROPERTYKEY *b)
{
    return IsEqualGUID(&a->fmtid, &b->fmtid) && a->pid == b->pid;
}

/* Whether a render endpoint is there this session: Wine keeps the endpoints
 * of devices seen before, marked not present. */
static BOOL active(IPropertyStore *store)
{
    PROPVARIANT guid;
    WCHAR key[160];
    DWORD state = 0, size = sizeof(state);

    PropVariantInit(&guid);
    if (SUCCEEDED(real_GetValue(store, &guid_key, &guid)) && guid.vt == VT_LPWSTR && guid.pwszVal)
    {
        swprintf(key, ARRAY_SIZE(key),
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\%ls", guid.pwszVal);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"DeviceState", RRF_RT_REG_DWORD, NULL, &state, &size))
            state = 0;
    }
    PropVariantClear(&guid);
    return state == DEVICE_STATE_ACTIVE;
}

/* The pad an endpoint belongs to: 0x0df2 (Edge) or 0x0ce6, or 0 when the
 * endpoint is not a DualSense's four-channel output present this session. */
static WORD pad_model(IPropertyStore *store)
{
    PROPVARIANT name, format;
    WORD model = 0;

    PropVariantInit(&name);
    PropVariantInit(&format);
    if (SUCCEEDED(real_GetValue(store, &name_key, &name)) && name.vt == VT_LPWSTR && name.pwszVal
        && wcsstr(name.pwszVal, L"DualSense")
        && SUCCEEDED(real_GetValue(store, &format_key, &format)) && format.vt == VT_BLOB
        && format.blob.cbSize >= sizeof(WAVEFORMATEX)
        && ((WAVEFORMATEX *)format.blob.pBlobData)->nChannels == 4 && active(store))
        model = wcsstr(name.pwszVal, L"Edge") ? 0x0df2 : 0x0ce6;
    PropVariantClear(&name);
    PropVariantClear(&format);
    return model;
}

/* The container of the model's live HID device: Wine keeps the nodes of pads
 * seen before and may list them as present, but enables the HID interface of
 * a connected one only, and that interface is what a game opens. */
static const GUID hid_interface = /* GUID_DEVINTERFACE_HID */
    { 0x4d1e55b2, 0xf16f, 0x11cf, { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };

static BOOL find_container(WORD model, GUID *out)
{
    HDEVINFO set = SetupDiGetClassDevsW(&hid_interface, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    SP_DEVICE_INTERFACE_DATA iface;
    SP_DEVINFO_DATA info;
    WCHAR id[512], want[24], path[600], value[64];
    DWORD i, size;
    BOOL found = FALSE;

    if (set == INVALID_HANDLE_VALUE) return FALSE;
    iface.cbSize = sizeof(iface);
    info.cbSize = sizeof(info);
    swprintf(want, ARRAY_SIZE(want), L"VID_054C&PID_%04X", model);
    for (i = 0; !found && SetupDiEnumDeviceInterfaces(set, NULL, &hid_interface, i, &iface); i++)
    {
        HKEY key;
        WCHAR *c;

        SetupDiGetDeviceInterfaceDetailW(set, &iface, NULL, 0, NULL, &info);
        if (!SetupDiGetDeviceInstanceIdW(set, &info, id, ARRAY_SIZE(id), NULL)) continue;
        for (c = id; *c; c++) *c = towupper(*c);
        if (!wcsstr(id, want)) continue;
        swprintf(path, ARRAY_SIZE(path), L"System\\CurrentControlSet\\Enum\\%ls", id);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key)) continue;
        size = sizeof(value);
        found = !RegQueryValueExW(key, L"ContainerId", NULL, NULL, (BYTE *)value, &size)
                && SUCCEEDED(CLSIDFromString(value, out));
        RegCloseKey(key);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

/* Games ask for containers hundreds of times; the HID interfaces are walked
 * again after two seconds, since a pad plugged in again gets a new container. */
static SRWLOCK lock = SRWLOCK_INIT;
static struct { WORD model; BOOL found; DWORD at; GUID container; } cache[2];

static BOOL pad_container(WORD model, GUID *out)
{
    int slot = model == 0x0df2;
    DWORD now = GetTickCount();
    BOOL found;

    AcquireSRWLockExclusive(&lock);
    if (cache[slot].model != model || now - cache[slot].at > 2000)
    {
        cache[slot].model = model;
        cache[slot].at = now;
        cache[slot].found = find_container(model, &cache[slot].container);
    }
    found = cache[slot].found;
    if (found) *out = cache[slot].container;
    ReleaseSRWLockExclusive(&lock);
    return found;
}

static HRESULT WINAPI hooked_GetValue(IPropertyStore *store, REFPROPERTYKEY key, PROPVARIANT *pv)
{
    HRESULT hr = real_GetValue(store, key, pv);
    GUID container;
    WORD model;

    if (SUCCEEDED(hr) && key && pv && pv->vt == VT_EMPTY && same_key(key, &container_key)
        && (model = pad_model(store)) && pad_container(model, &container)
        && (pv->puuid = CoTaskMemAlloc(sizeof(GUID))))
    {
        *pv->puuid = container;
        pv->vt = VT_CLSID;
    }
    return hr;
}

/* Every endpoint's property store shares Wine's one vtable, so one store,
 * opened on any endpoint, is enough to hook them all. The hook points into
 * this DLL, so it is pinned first. */
static void install(void)
{
    IClassFactory *factory = NULL;
    IMMDeviceEnumerator *devices = NULL;
    IMMDeviceCollection *all = NULL;
    IMMDevice *device = NULL;
    IPropertyStore *store = NULL;
    HMODULE self;
    UINT count = 0;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCWSTR)(void *)install, &self))
        return;
    if (FAILED(real_DllGetClassObject(&CLSID_MMDeviceEnumerator, &IID_IClassFactory, (void **)&factory)))
        return;
    if (SUCCEEDED(IClassFactory_CreateInstance(factory, NULL, &IID_IMMDeviceEnumerator, (void **)&devices)))
    {
        if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(devices, eAll, DEVICE_STATEMASK_ALL, &all)))
        {
            if (SUCCEEDED(IMMDeviceCollection_GetCount(all, &count)) && count
                && SUCCEEDED(IMMDeviceCollection_Item(all, 0, &device)))
            {
                if (SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &store)))
                {
                    void **vt = *(void ***)store;
                    DWORD old;
                    if (vt[5] != (void *)hooked_GetValue
                        && VirtualProtect(&vt[5], sizeof(void *), PAGE_READWRITE, &old))
                    {
                        real_GetValue = (get_value_fn)vt[5];
                        vt[5] = (void *)hooked_GetValue;
                        VirtualProtect(&vt[5], sizeof(void *), old, &old);
                    }
                    IPropertyStore_Release(store);
                }
                IMMDevice_Release(device);
            }
            IMMDeviceCollection_Release(all);
        }
        IMMDeviceEnumerator_Release(devices);
    }
    IClassFactory_Release(factory);
}

/* The hook goes in on the first call. Creating the enumerator for it may call
 * back in here, which then goes straight to Wine's. */
HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    static LONG installing;

    if (!real_DllGetClassObject)
    {
        get_class_object_fn wine = wine_export("DllGetClassObject");
        if (!wine || wine == DllGetClassObject) return CLASS_E_CLASSNOTAVAILABLE;
        real_DllGetClassObject = wine;
    }
    if (!InterlockedCompareExchange(&installing, 1, 0)) install();
    return real_DllGetClassObject(clsid, iid, out);
}

#define CALL_WINE(ret, name, fail, params, args)                    \
    ret WINAPI name params                                          \
    {                                                               \
        ret (WINAPI *wine) params = wine_export(#name);             \
        return wine ? wine args : fail;                             \
    }
CALL_WINE(HRESULT, ActivateAudioInterfaceAsync, E_FAIL,
          (const WCHAR *a, REFIID b, PROPVARIANT *c, IActivateAudioInterfaceCompletionHandler *d,
           IActivateAudioInterfaceAsyncOperation **e), (a, b, c, d, e))
CALL_WINE(HRESULT, DllRegisterServer, E_FAIL, (void), ())
CALL_WINE(HRESULT, DllUnregisterServer, E_FAIL, (void), ())
CALL_WINE(LRESULT, DriverProc, 0, (DWORD_PTR a, HANDLE b, UINT c, LPARAM d, LPARAM e), (a, b, c, d, e))
CALL_WINE(DWORD, auxMessage, 0, (UINT a, UINT b, DWORD_PTR c, DWORD_PTR d, DWORD_PTR e), (a, b, c, d, e))
CALL_WINE(DWORD, midMessage, 0, (UINT a, UINT b, DWORD_PTR c, DWORD_PTR d, DWORD_PTR e), (a, b, c, d, e))
CALL_WINE(DWORD, modMessage, 0, (UINT a, UINT b, DWORD_PTR c, DWORD_PTR d, DWORD_PTR e), (a, b, c, d, e))

/* Once used, this DLL is pinned: it never unloads. */
HRESULT WINAPI DllCanUnloadNow(void)
{
    return S_FALSE;
}

/* Wine's ordinals 2 to 16 are stubs; they stay exported, and do nothing. */
#define ORDINAL(n) HRESULT WINAPI ordinal##n(void) { return E_NOTIMPL; }
ORDINAL(2) ORDINAL(3) ORDINAL(4) ORDINAL(5) ORDINAL(6) ORDINAL(7) ORDINAL(8) ORDINAL(9)
ORDINAL(10) ORDINAL(11) ORDINAL(12) ORDINAL(13) ORDINAL(14) ORDINAL(15) ORDINAL(16)
