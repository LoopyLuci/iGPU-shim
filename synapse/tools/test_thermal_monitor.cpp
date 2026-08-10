// ============================================================================
// synapse/tools/test_thermal_monitor.cpp
// Real hardware test: probes available thermal/power APIs on Windows
// and reports what the iGPU exposes for temperature.
//
// Run: VK_LAYER_PATH=<dir> VK_INSTANCE_LAYERS=VK_LAYER_SYNAPSE_iGPU_Shim
//      test_thermal_monitor.exe
// ============================================================================

#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define VK_CHECK(call) do { \
    VkResult _r = (call); \
    if (_r != VK_SUCCESS) { \
        printf("  FAIL: %s returned %d\n", #call, _r); \
        return 1; \
    } \
} while(0)

// ---- Windows thermal probing helpers ----
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <powrprof.h>
#include <setupapi.h>
#include <devguid.h>
#include <comdef.h>
#include <Wbemidl.h>
#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "wbemuuid.lib")

static std::string wmiQuery(const wchar_t* query) {
    IWbemLocator *pLoc = nullptr;
    IWbemServices *pSvc = nullptr;
    std::string result;

    HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hr)) { return "CoInitializeEx failed"; }

    hr = CoInitializeSecurity(
        NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    if (FAILED(hr)) { CoUninitialize(); return "CoInitializeSecurity failed"; }

    hr = CoCreateInstance(CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, (LPVOID*)&pLoc);
    if (FAILED(hr)) { CoUninitialize(); return "CoCreateInstance failed"; }

    hr = pLoc->ConnectServer(
        _bstr_t(L"ROOT\\WMI"), NULL, NULL, 0, NULL, 0, 0, &pSvc);
    if (FAILED(hr)) { pLoc->Release(); CoUninitialize(); return "ConnectServer failed"; }

    hr = CoSetProxyBlanket(pSvc,
        RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE);
    if (FAILED(hr)) { pSvc->Release(); pLoc->Release(); CoUninitialize(); return "SetProxyBlanket failed"; }

    IEnumWbemClassObject* pEnum = nullptr;
    hr = pSvc->ExecQuery(
        bstr_t("WQL"), bstr_t(query),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &pEnum);
    if (FAILED(hr)) { pSvc->Release(); pLoc->Release(); CoUninitialize(); return "ExecQuery failed"; }

    IWbemClassObject *pObj = nullptr;
    ULONG ret = 0;
    while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &ret) == S_OK) {
        VARIANT vt;
        hr = pObj->Get(L"CurrentTemperature", 0, &vt, nullptr, nullptr);
        if (SUCCEEDED(hr) && vt.vt == VT_UI4) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%u", vt.uiVal);
            result += buf;
        }
        hr = pObj->Get(L"InstanceName", 0, &vt, nullptr, nullptr);
        if (SUCCEEDED(hr) && vt.vt == VT_BSTR) {
            result += " (";
            result += _bstr_t(vt.bstrVal);
            result += ")";
        }
        result += "\n";
        pObj->Release();
    }

    if (pEnum) pEnum->Release();
    if (pSvc) pSvc->Release();
    if (pLoc) pLoc->Release();
    CoUninitialize();
    return result.empty() ? "No data" : result;
}
#endif

int main() {
    printf("=== Thermal Monitoring Probe (Real iGPU) ===\n\n");

    // ── Step 1: Check Vulkan instance extensions for thermal ─────────────
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Synapse Thermal";
    appInfo.apiVersion = VK_API_VERSION_1_2;

    const char* layers[] = {"VK_LAYER_SYNAPSE_iGPU_Shim"};

    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, exts.data());

    printf("Instance extensions (%u total):\n", extCount);
    bool hasThermal = false;
    for (auto& e : exts) {
        if (strstr(e.extensionName, "thermal") || strstr(e.extensionName, "temperature")) {
            printf("  [THERMAL] %s (v%u)\n", e.extensionName, e.specVersion);
            hasThermal = true;
        }
    }
    if (!hasThermal) {
        printf("  No thermal/temperature extensions available\n");
    }

    // ── Step 2: Create instance + device ────────────────────────────────
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &appInfo;
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = layers;

    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(instance, &devCount, nullptr);
    std::vector<VkPhysicalDevice> physDevs(devCount);
    vkEnumeratePhysicalDevices(instance, &devCount, physDevs.data());

    VkPhysicalDevice physDev = physDevs[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physDev, &props);
    printf("\nGPU: %s\n", props.deviceName);

    uint32_t devExtCount = 0;
    vkEnumerateDeviceExtensionProperties(physDev, nullptr, &devExtCount, nullptr);
    std::vector<VkExtensionProperties> devExts(devExtCount);
    vkEnumerateDeviceExtensionProperties(physDev, nullptr, &devExtCount, devExts.data());

    printf("Device extensions (%u total):\n", devExtCount);
    bool hasDevThermal = false;
    for (auto& e : devExts) {
        if (strstr(e.extensionName, "thermal") || strstr(e.extensionName, "temperature")) {
            printf("  [THERMAL] %s (v%u)\n", e.extensionName, e.specVersion);
            hasDevThermal = true;
        }
    }
    if (!hasDevThermal) {
        printf("  No thermal/temperature extensions available\n");
    }

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physDev, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physDev, &qfCount, qfs.data());
    int gfxQ = -1;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfxQ = (int)i; break; }
    }
    if (gfxQ < 0) { printf("FAIL: no graphics queue\n"); return 1; }

    float pri = 1.0f;
    VkDeviceQueueCreateInfo dqi{};
    dqi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    dqi.queueFamilyIndex = (uint32_t)gfxQ;
    dqi.queueCount = 1;
    dqi.pQueuePriorities = &pri;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &dqi;

    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(physDev, &dci, nullptr, &device));

    // ── Step 3: Probe Windows WMI for thermal zones ─────────────────────
#ifdef _WIN32
    printf("\nWindows WMI thermal probe:\n");
    std::string wmiResult = wmiQuery(L"SELECT * FROM MSAcpi_ThermalZoneTemperature");
    printf("  MSAcpi_ThermalZoneTemperature: %s\n",
        wmiResult.empty() ? "No data" : wmiResult.c_str());

    std::string wmiResult2 = wmiQuery(L"SELECT * FROM MSAcpi_ThermalZoneCurrentTemperature");
    printf("  MSAcpi_ThermalZoneCurrentTemperature: %s\n",
        wmiResult2.empty() ? "No data" : wmiResult2.c_str());

    std::string wmiResult3 = wmiQuery(L"SELECT * FROM Win32_TemperatureProbe");
    printf("  Win32_TemperatureProbe: %s\n",
        wmiResult3.empty() ? "No data" : wmiResult3.c_str());

    std::string wmiResult4 = wmiQuery(L"SELECT * FROM MSAcpi_ThermalZone");
    printf("  MSAcpi_ThermalZone: %s\n",
        wmiResult4.empty() ? "No data" : wmiResult4.c_str());

    // ── Step 4: Windows Power API probe ─────────────────────────────────
    printf("\nWindows Power API probe:\n");

    GUID* scheme = nullptr;
    DWORD res = PowerGetActiveScheme(nullptr, &scheme);
    if (res == ERROR_SUCCESS && scheme) {
        printf("  PowerGetActiveScheme: OK\n");
        LocalFree(scheme);
    } else {
        printf("  PowerGetActiveScheme: failed (0x%x)\n", res);
    }

    printf("  Thermal zone notification: skipped (requires GUID_THERMAL_ZONE)\n");
#endif

    // ── Step 5: Cleanup ─────────────────────────────────────────────────
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    printf("\n=== Thermal Monitoring Probe Complete ===\n");
    printf("Summary:\n");
    printf("  Vulkan thermal extensions: %s\n",
        (hasThermal || hasDevThermal) ? "Available" : "Not available on this GPU/driver");
    printf("  Temperature readable on this hardware: %s\n",
        (hasThermal || hasDevThermal) ? "YES (via Vulkan)" : "NO on this hardware");

    return 0;
}
