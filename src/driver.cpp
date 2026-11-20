// SPDX-License-Identifier: BSD-2-Clause
// Copyright © 2021 Billy Laws

#include <vulkan/vulkan.h>
#include <adrenotools/driver.h>
#include "hook/kgsl.h"
#include "hook/hook_impl_params.h"
#include "turnip_compat.h"

// When Rust is available, register pending GPU mappings with the Rust
// GpuMappingManager via the cxx bridge so hook_gsl_memory_alloc_pure_64 can
// consume them thread-safely without racing on hook_params->nextGpuMapping.
#ifdef ADRENOTOOLS_RUST_ENABLED
#include <adrenotools/adrenotools_rs.h>
#endif  // ADRENOTOOLS_RUST_ENABLED

#include <android/api-level.h>
#include <android/dlext.h>
#include <android/log.h>
#include <android_linker_ns.h>
#include <jni.h>
#include <shadowhook.h>

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  "AdrenoToolsPatch", __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN,  "AdrenoToolsPatch", __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AdrenoToolsPatch", __VA_ARGS__)

// ─── Small string / env helpers ──────────────────────────────────────────────

static bool str_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) return false;
    const size_t nlen = std::strlen(needle);
    for (const char *p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               std::tolower(static_cast<unsigned char>(p[i])) ==
                   std::tolower(static_cast<unsigned char>(needle[i]))) {
            ++i;
        }
        if (i == nlen) return true;
    }
    return false;
}

static bool str_equals_ci(const char *a, const char *b) {
    if (!a || !b) return false;
    while (*a && *b) {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

// Set default env var without wiping an explicit caller/launcher override.
static void set_default_env(const char *name, const char *value) {
    setenv(name, value, 0);
}

static bool env_is_truthy(const char *name) {
    const char *v = std::getenv(name);
    if (!v || !v[0]) return false;
    return std::strcmp(v, "1") == 0 || str_equals_ci(v, "true") ||
           str_equals_ci(v, "yes") || str_equals_ci(v, "on");
}

static std::string ensure_trailing_slash(const char *path) {
    if (!path || !path[0]) return {};
    std::string out(path);
    if (out.back() != '/') out.push_back('/');
    return out;
}

static void read_first_line(const char *path, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;

    ssize_t n = read(fd, out, out_len - 1);
    close(fd);

    if (n > 0) {
        char *nl = static_cast<char *>(std::memchr(out, '\n', static_cast<size_t>(n)));
        size_t len = nl ? static_cast<size_t>(nl - out) : static_cast<size_t>(n);
        if (len > 0 && out[len - 1] == '\r') --len;
        out[len] = '\0';
    }
}

// ─── ROM / Vulkan loader path discovery ──────────────────────────────────────

// Xiaomi's HyperOS (and some OEM builds) may place libvulkan.so under vendor/odm.
// Probe the standard system path first, then known OEM fallback locations.
static const char *find_libvulkan_path(void) {
    static const char *candidates[] = {
        "/system/lib64/libvulkan.so",
        "/system/vendor/lib64/libvulkan.so",
        "/vendor/lib64/libvulkan.so",
        "/odm/lib64/libvulkan.so",
        "/product/lib64/libvulkan.so",
        "/system_ext/lib64/libvulkan.so",
        nullptr,
    };

    for (const char **path = candidates; *path != nullptr; ++path) {
        if (access(*path, R_OK) == 0)
            return *path;
    }
    return candidates[0];
}

// Log the detected ROM (checks ro.mi.os.version.* for real HyperOS 1/2/3 builds
// as well as legacy ro.miui.ui.version.name).
static void log_detected_rom(void) {
    char hyperos[PROP_VALUE_MAX] = {};
    char miui[PROP_VALUE_MAX]    = {};
    char brand[PROP_VALUE_MAX]   = {};

    __system_property_get("ro.mi.os.version.name", hyperos);
    if (!hyperos[0])
        __system_property_get("ro.mi.os.version.code", hyperos);
    __system_property_get("ro.miui.ui.version.name", miui);
    __system_property_get("ro.product.brand", brand);

    if (hyperos[0] || std::strcmp(miui, "V816") == 0 || str_contains_ci(miui, "hyper")) {
        ALOGI("HyperOS (Xiaomi) detected (%s) — loader: %s",
              hyperos[0] ? hyperos : miui, find_libvulkan_path());
    } else if (brand[0]) {
        ALOGI("Device brand: %s — loader: %s", brand, find_libvulkan_path());
    }
}

// ─── GPU family detection (Adreno vs ARM Mali) ───────────────────────────────

static bool detect_mali_gpu_uncached() {
    // Explicit override: ADRENOTOOLS_FORCE_DRIVER=panvk|mali|turnip|adreno
    if (const char *forced = std::getenv("ADRENOTOOLS_FORCE_DRIVER")) {
        if (str_equals_ci(forced, "panvk") || str_equals_ci(forced, "mali") ||
            str_equals_ci(forced, "panfrost")) {
            return true;
        }
        if (str_equals_ci(forced, "turnip") || str_equals_ci(forced, "adreno") ||
            str_equals_ci(forced, "freedreno")) {
            return false;
        }
    }

    // Qualcomm KGSL node means Adreno, never Mali.
    if (access("/dev/kgsl-3d0", F_OK) == 0)
        return false;

    // ARM Mali kernel driver device nodes (kbase / panfrost / panthor).
    if (access("/dev/mali0", F_OK) == 0 || access("/dev/mali", F_OK) == 0)
        return true;

    // World-readable HAL properties set on ARM Mali builds (e.g. Mali-G52).
    for (const char *propName : {"ro.hardware.vulkan", "ro.hardware.egl"}) {
        char val[PROP_VALUE_MAX] = {};
        if (__system_property_get(propName, val) > 0) {
            if (str_contains_ci(val, "adreno"))
                return false;
            if (str_contains_ci(val, "mali") || str_contains_ci(val, "panfrost") ||
                str_contains_ci(val, "panvk")) {
                return true;
            }
        }
    }

    // Sysfs / device-tree fallbacks.
    char info[128] = {};
    read_first_line("/sys/class/misc/mali0/device/gpuinfo", info, sizeof(info));
    if (!info[0]) read_first_line("/proc/device-tree/model", info, sizeof(info));
    if (!info[0]) read_first_line("/sys/firmware/devicetree/base/model", info, sizeof(info));

    return str_contains_ci(info, "mali");
}

static bool isMaliGpu() {
    static std::once_flag once;
    static bool cached = false;
    std::call_once(once, []() { cached = detect_mali_gpu_uncached(); });
    return cached;
}

// ─── Public C API (bylaws/libadrenotools compatible) ─────────────────────────

void *adrenotools_open_libvulkan(int dlopenFlags,
                                 int featureFlags,
                                 const char *tmpLibDir,
                                 const char *hookLibDir,
                                 const char *customDriverDir,
                                 const char *customDriverName,
                                 const char *fileRedirectDir,
                                 void **userMappingHandle) {
    if (!linkernsbypass_load_status()) {
        ALOGE("FAILURE: Could not load linkernsbypass");
        return nullptr;
    }

    if (!hookLibDir || !hookLibDir[0]) {
        ALOGE("FAILURE: hookLibDir parameter is null or empty");
        return nullptr;
    }

    // Always use memfd on Android 10+ (API 29+): avoids SELinux W^X denials when
    // loading a patched libvulkan.so out of an app's writable cache directory.
    if (android_get_device_api_level() >= 29)
        tmpLibDir = nullptr;

    if (!(featureFlags & ADRENOTOOLS_DRIVER_FILE_REDIRECT) && fileRedirectDir) {
        ALOGE("FAILURE: fileRedirectDir supplied without ADRENOTOOLS_DRIVER_FILE_REDIRECT");
        return nullptr;
    }

    if (!(featureFlags & ADRENOTOOLS_DRIVER_CUSTOM) && (customDriverDir || customDriverName)) {
        ALOGE("FAILURE: customDriver supplied without ADRENOTOOLS_DRIVER_CUSTOM");
        return nullptr;
    }

    if (!(featureFlags & ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT) && userMappingHandle) {
        ALOGE("FAILURE: userMappingHandle supplied without ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT");
        return nullptr;
    }

    struct stat buf{};
    std::string normalizedDriverDir;

    if (featureFlags & ADRENOTOOLS_DRIVER_CUSTOM) {
        if (!customDriverName || !customDriverName[0] || !customDriverDir || !customDriverDir[0]) {
            ALOGE("FAILURE: ADRENOTOOLS_DRIVER_CUSTOM set without driver dir/name");
            return nullptr;
        }

        normalizedDriverDir = ensure_trailing_slash(customDriverDir);
        const std::string fullDriverPath = normalizedDriverDir + customDriverName;
        if (stat(fullDriverPath.c_str(), &buf) != 0) {
            ALOGE("FAILURE: Custom driver not found at %s (errno=%d)", fullDriverPath.c_str(), errno);
            return nullptr;
        }
        customDriverDir = normalizedDriverDir.c_str();
    }

    if (featureFlags & ADRENOTOOLS_DRIVER_FILE_REDIRECT) {
        if (!fileRedirectDir || !fileRedirectDir[0]) {
            ALOGE("FAILURE: ADRENOTOOLS_DRIVER_FILE_REDIRECT set without redirect dir");
            return nullptr;
        }

        if (stat(fileRedirectDir, &buf) != 0) {
            ALOGE("FAILURE: File redirect dir does not exist: %s (errno=%d)", fileRedirectDir, errno);
            return nullptr;
        }
    }

    auto hookNs = android_create_namespace(
        "adrenotools-libvulkan", hookLibDir, nullptr, ANDROID_NAMESPACE_TYPE_SHARED, nullptr, nullptr);
    if (!hookNs) {
        ALOGE("FAILURE: android_create_namespace failed for hookLibDir=%s", hookLibDir);
        return nullptr;
    }

    if (!linkernsbypass_link_namespace_to_default_all_libs(hookNs)) {
        ALOGE("FAILURE: Failed to link hook namespace to default namespace");
        return nullptr;
    }

    auto hookImpl = linkernsbypass_namespace_dlopen("libhook_impl.so", RTLD_NOW, hookNs);
    if (!hookImpl) {
        const char *err = dlerror();
        ALOGE("FAILURE: Couldn't preload libhook_impl.so from %s: %s",
              hookLibDir, err ? err : "unknown");
        return nullptr;
    }

    auto initHookParam = reinterpret_cast<void (*)(const void *)>(dlsym(hookImpl, "init_hook_param"));
    if (!initHookParam) {
        ALOGE("FAILURE: Couldn't resolve init_hook_param in libhook_impl.so");
        return nullptr;
    }

    adrenotools_gpu_mapping *importMapping = nullptr;
    if (featureFlags & ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT) {
        if (!userMappingHandle) {
            ALOGE("FAILURE: ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT set with null userMappingHandle");
            return nullptr;
        }
        importMapping = new adrenotools_gpu_mapping{};
        *userMappingHandle = importMapping;
    }

    initHookParam(new HookImplParams(
        featureFlags, tmpLibDir, hookLibDir, customDriverDir, customDriverName, fileRedirectDir, importMapping));

    if (!linkernsbypass_namespace_dlopen("libmain_hook.so", RTLD_GLOBAL, hookNs)) {
        const char *err = dlerror();
        ALOGE("FAILURE: Couldn't load libmain_hook.so from %s: %s",
              hookLibDir, err ? err : "unknown");
        // Clean up the mapping we allocated — caller gets nullptr back.
        if (importMapping) {
            delete importMapping;
            *userMappingHandle = nullptr;
        }
        return nullptr;
    }

    log_detected_rom();

    const char *vulkanPath = find_libvulkan_path();
    void *handle = linkernsbypass_namespace_dlopen_unique(vulkanPath, tmpLibDir, dlopenFlags, hookNs);
    if (!handle) {
        const char *err = dlerror();
        ALOGE("FAILURE: Failed to load unique %s: %s", vulkanPath, err ? err : "unknown");
    }
    return handle;
}

bool adrenotools_import_user_mem(void *handle, void *hostPtr, uint64_t size) {
    if (!handle || !hostPtr || size == 0) return false;
    auto importMapping = reinterpret_cast<adrenotools_gpu_mapping *>(handle);

    kgsl_gpuobj_import_useraddr addr{
        .virtaddr = reinterpret_cast<uint64_t>(hostPtr),
    };

    kgsl_gpuobj_import userMemImport{};
    userMemImport.priv     = reinterpret_cast<uint64_t>(&addr);
    userMemImport.priv_len = size;
    userMemImport.flags    = KGSL_CACHEMODE_WRITEBACK << KGSL_CACHEMODE_SHIFT
                           | KGSL_MEMFLAGS_IOCOHERENT;
    userMemImport.type     = KGSL_USER_MEM_TYPE_ADDR;

    kgsl_gpuobj_info info{};

    int kgslFd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (kgslFd < 0)
        return false;

    if (ioctl(kgslFd, IOCTL_KGSL_GPUOBJ_IMPORT, &userMemImport) != 0) {
        close(kgslFd);
        return false;
    }

    info.id = userMemImport.id;
    if (ioctl(kgslFd, IOCTL_KGSL_GPUOBJ_INFO, &info) != 0) {
        close(kgslFd);
        return false;
    }

    importMapping->host_ptr = hostPtr;
    importMapping->gpu_addr = info.gpuaddr;
    importMapping->size     = size;
    importMapping->flags    = 0xc2600;

#ifdef ADRENOTOOLS_RUST_ENABLED
    // Register the imported mapping with Rust's thread-safe GpuMappingManager so
    // hook_gsl_memory_alloc_pure_64 can consume it via ffi::try_consume_pending.
    ffi::GpuMapping rs_mapping{
        reinterpret_cast<uintptr_t>(importMapping->host_ptr),
        importMapping->gpu_addr,
        importMapping->size,
        importMapping->flags,
        ADRENOTOOLS_GPU_MAPPING_SUCCEEDED_MAGIC,
    };
    ffi::set_pending_mapping(rs_mapping);
#endif  // ADRENOTOOLS_RUST_ENABLED

    close(kgslFd);
    return true;
}

bool adrenotools_mem_gpu_allocate(void *handle, uint64_t *size) {
    if (!handle || !size || *size == 0) return false;
    auto mapping = reinterpret_cast<adrenotools_gpu_mapping *>(handle);

    kgsl_gpuobj_alloc gpuobjAlloc{};
    gpuobjAlloc.size  = *size;
    gpuobjAlloc.flags = KGSL_CACHEMODE_WRITEBACK << KGSL_CACHEMODE_SHIFT
                      | KGSL_MEMFLAGS_IOCOHERENT;

    kgsl_gpuobj_info info{};

    int kgslFd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (kgslFd < 0)
        return false;

    if (ioctl(kgslFd, IOCTL_KGSL_GPUOBJ_ALLOC, &gpuobjAlloc) != 0) {
        close(kgslFd);
        return false;
    }

    *size   = gpuobjAlloc.mmapsize;
    info.id = gpuobjAlloc.id;

    if (ioctl(kgslFd, IOCTL_KGSL_GPUOBJ_INFO, &info) != 0) {
        close(kgslFd);
        return false;
    }

    mapping->host_ptr = nullptr;
    mapping->gpu_addr = info.gpuaddr;
    mapping->size     = *size;
    mapping->flags    = 0xc2600;

#ifdef ADRENOTOOLS_RUST_ENABLED
    // Register the allocated mapping with Rust's thread-safe GpuMappingManager so
    // hook_gsl_memory_alloc_pure_64 can consume it via ffi::try_consume_pending.
    ffi::GpuMapping rs_mapping{
        reinterpret_cast<uintptr_t>(mapping->host_ptr),
        mapping->gpu_addr,
        mapping->size,
        mapping->flags,
        ADRENOTOOLS_GPU_MAPPING_SUCCEEDED_MAGIC,
    };
    ffi::set_pending_mapping(rs_mapping);
#endif  // ADRENOTOOLS_RUST_ENABLED

    close(kgslFd);
    return true;
}

bool adrenotools_mem_cpu_map(void *handle, void *hostPtr, uint64_t size) {
    if (!handle || size == 0) return false;
    auto mapping = reinterpret_cast<adrenotools_gpu_mapping *>(handle);

    // MAP_FIXED requires page-aligned address and length. Unaligned hostPtr can
    // silently unmap adjacent valid memory or corrupt the caller's buffer.
    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return false;
    if (reinterpret_cast<uintptr_t>(hostPtr) % static_cast<uintptr_t>(page_size) != 0) {
        ALOGE("adrenotools_mem_cpu_map: hostPtr %p not page-aligned (page_size=%ld)",
              hostPtr, page_size);
        return false;
    }
    const uint64_t page_size_u64 = static_cast<uint64_t>(page_size);
    if (size % page_size_u64 != 0) {
        ALOGE("adrenotools_mem_cpu_map: size %llu not page-aligned (page_size=%ld)",
              static_cast<unsigned long long>(size), page_size);
        return false;
    }

    int kgslFd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (kgslFd < 0)
        return false;

    void *mapped = mmap(hostPtr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                        kgslFd, static_cast<off_t>(mapping->gpu_addr));
    close(kgslFd);

    if (mapped == MAP_FAILED) {
        mapping->host_ptr = nullptr;
        return false;
    }

    mapping->host_ptr = mapped;
    return true;
}

bool adrenotools_validate_gpu_mapping(void *handle) {
    if (!handle) return false;
    auto importMapping = reinterpret_cast<adrenotools_gpu_mapping *>(handle);
    return importMapping->gpu_addr == ADRENOTOOLS_GPU_MAPPING_SUCCEEDED_MAGIC;
}

void adrenotools_set_turbo(bool turbo) {
    uint32_t enable = turbo ? 0U : 1U;
    kgsl_device_getproperty prop{
        .type      = KGSL_PROP_PWRCTRL,
        .value     = reinterpret_cast<void *>(&enable),
        .sizebytes = sizeof(enable),
    };

    int kgslFd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (kgslFd < 0) {
        ALOGW("adrenotools_set_turbo: cannot open /dev/kgsl-3d0 (errno %d)", errno);
        return;
    }

    if (ioctl(kgslFd, IOCTL_KGSL_SETPROPERTY, &prop) < 0) {
        ALOGW("adrenotools_set_turbo: IOCTL_KGSL_SETPROPERTY(KGSL_PROP_PWRCTRL) failed (errno %d) — this ioctl can crash the KGSL driver on some Adreno kernels", errno);
    }

    close(kgslFd);
}

bool adrenotools_set_freedreno_env(const char *varName, const char *value) {
    if (!varName || !value || std::strlen(varName) == 0)
        return false;

    if (setenv(varName, value, 1) != 0) {
        ALOGE("FAILURE adrenotools_set_freedreno_env: Failed to set '%s' (errno: %d)", varName, errno);
        return false;
    }

    const char *verifyValue = std::getenv(varName);
    if (verifyValue && std::strcmp(verifyValue, value) == 0)
        return true;

    ALOGW("WARN adrenotools_set_freedreno_env: Verification failed for '%s'", varName);
    return false;
}

// ─── Active custom Vulkan loader state & ShadowHook proxies ──────────────────
//
// Only vkGetInstanceProcAddr (gipa) and vkGetDeviceProcAddr (gdpa) are hooked:
// hooking other Vulkan symbols directly conflicts with pre-existing system/UI
// instances. Both Turnip (Adreno) and PanVK (Mali) share these two atomics.
//
// ShadowHook proxies MUST:
//   1. Be __attribute__((noinline)) so __builtin_return_address(0) is always the
//      return address pushed by ShadowHook's hub trampoline (even under -O3/LTO).
//   2. Construct SHADOWHOOK_STACK_SCOPE() at the top so the per-thread
//      recursion-guard frame is popped on EVERY return path.
//   3. Inspect SHADOWHOOK_RETURN_ADDRESS() and pass Android system UI / WebView /
//      framework callers (libhwui.so SkiaVK, Trichrome/WebView, libgui.so, etc.)
//      through to stock libvulkan.so via SHADOWHOOK_CALL_PREV() so Android's
//      SkSurface::MakeFromAHardwareBuffer window surfaces never fail on Turnip/PanVK,
//      while routing all app/game engine callers to the custom driver.

static std::atomic<PFN_vkGetInstanceProcAddr> g_active_gipa{nullptr};
static std::atomic<PFN_vkGetDeviceProcAddr>   g_active_gdpa{nullptr};
static std::atomic<bool>                      g_hook_system_callers{false};
static std::atomic<uint32_t>                  g_caller_cache_epoch{1};
static std::atomic<uintptr_t>                 g_logged_caller_keys[16]{};

static void *g_turnip_handle    = nullptr;
static void *g_panvk_handle     = nullptr;
static void *g_shadow_stub_gipa = nullptr;
static void *g_shadow_stub_gdpa = nullptr;
static std::mutex g_driver_mutex;

// Lock-free deduplicator so each calling module is logged at most once.
static bool mark_caller_logged_once(uintptr_t key) {
    if (key == 0) return false;
    for (auto &slot : g_logged_caller_keys) {
        uintptr_t cur = slot.load(std::memory_order_relaxed);
        if (cur == key) return false;
        if (cur == 0) {
            uintptr_t expected = 0;
            if (slot.compare_exchange_strong(expected, key, std::memory_order_relaxed))
                return true;
            if (expected == key) return false;
        }
    }
    return false;
}

static void reset_logged_callers_locked() {
    for (auto &slot : g_logged_caller_keys)
        slot.store(0, std::memory_order_relaxed);
    g_caller_cache_epoch.fetch_add(1, std::memory_order_release);
}

// Returns true when `path` belongs to Android OS, vendor, APEX, WebView, Play Services,
// or OEM game-overlay libraries that must keep using stock libvulkan.so (notably
// /system/lib64/libhwui.so's SkiaVK renderer, which imports vendor UBWC AHardwareBuffers
// via SkSurface::MakeFromAHardwareBuffer).
static bool is_system_or_ui_library(const char *path) {
    if (!path || !path[0])
        return false;

    // Never bypass our own private memfd loader.
    if (std::strncmp(path, "/memfd:", 7) == 0)
        return false;

    static constexpr const char *kSystemPrefixes[] = {
        "/system/",
        "/system_ext/",
        "/vendor/",
        "/odm/",
        "/oem/",
        "/product/",
        "/apex/",
    };
    for (const char *prefix : kSystemPrefixes) {
        if (std::strncmp(path, prefix, std::strlen(prefix)) == 0)
            return true;
    }

    // Bare soname fallback if an OEM linker ever omits the leading slash.
    if (path[0] != '/') {
        static constexpr const char *kSystemSonames[] = {
            "libhwui.so",
            "libgui.so",
            "libui.so",
            "librenderengine.so",
            "libvulkan.so",
            "libandroid.so",
            "libandroid_runtime.so",
            "libmediandk.so",
        };
        for (const char *soname : kSystemSonames) {
            if (std::strcmp(path, soname) == 0)
                return true;
        }
    }

    // Play-Store-updated WebView / Trichrome / Chrome, GMS Dynamite modules, and
    // Samsung GOS overlays reside under /data/ rather than /system/ and also rely
    // on stock SkiaVK + AHardwareBuffer import.
    if (str_contains_ci(path, "libwebviewchromium") ||
        str_contains_ci(path, "libmonochrome") ||
        str_contains_ci(path, "trichromelibrary") ||
        str_contains_ci(path, "android.webview") ||
        str_contains_ci(path, "com.android.chrome") ||
        str_contains_ci(path, "com.sec.android.app.sbrowser") ||
        str_contains_ci(path, "com.google.android.gms") ||
        str_contains_ci(path, "com.samsung.android.game.") ||
        str_contains_ci(path, ".gpudrivers.")) {
        return true;
    }

    return false;
}

static bool is_system_ui_thread() {
    char tname[16] = {};
    if (pthread_getname_np(pthread_self(), tname, sizeof(tname)) != 0)
        return false;
    return std::strncmp(tname, "RenderThread",    12) == 0 ||
           std::strncmp(tname, "hwuiTask",         8) == 0 ||
           std::strncmp(tname, "Chrome_InProcGp", 15) == 0 ||
           std::strncmp(tname, "VizCompositor",   13) == 0;
}

struct CallerPhdrQuery {
    uintptr_t   pc;
    uintptr_t   seg_start;
    uintptr_t   seg_end;
    const char *name;
    bool        found;
};

static int find_caller_segment_cb(struct dl_phdr_info *info, size_t /*size*/, void *data) {
    if (!info || !info->dlpi_phdr) return 0;
    auto *q = static_cast<CallerPhdrQuery *>(data);
    const uintptr_t bias = static_cast<uintptr_t>(info->dlpi_addr);

    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || (ph.p_flags & PF_X) == 0 || ph.p_memsz == 0)
            continue;
        const uintptr_t start = bias + static_cast<uintptr_t>(ph.p_vaddr);
        const uintptr_t end   = start + static_cast<uintptr_t>(ph.p_memsz);
        if (end > start && q->pc >= start && q->pc < end) {
            q->seg_start = start;
            q->seg_end   = end;
            q->name      = info->dlpi_name;
            q->found     = true;
            return 1;
        }
    }
    return 0;
}

// Trivially-constructible/destructible per-thread 4-slot segment cache so repeated
// vkGetInstanceProcAddr / vkGetDeviceProcAddr calls resolve in a couple of instructions
// without touching dl_iterate_phdr or taking any lock.
struct CallerCacheSlot {
    uintptr_t start;
    uintptr_t end;
    bool      bypass;
};

struct CallerThreadCache {
    CallerCacheSlot slots[4];
    uint32_t        next;
    uint32_t        epoch;
};

static thread_local CallerThreadCache g_tls_caller_cache{};

static bool should_bypass_custom_driver(const void *raw_ret_addr) {
    if (g_hook_system_callers.load(std::memory_order_relaxed))
        return false;
    if (!raw_ret_addr)
        return is_system_ui_thread();

    const void *clean_addr = __builtin_extract_return_addr(const_cast<void *>(raw_ret_addr));
    const uintptr_t pc     = reinterpret_cast<uintptr_t>(clean_addr);

    CallerThreadCache &cache = g_tls_caller_cache;
    const uint32_t cur_epoch = g_caller_cache_epoch.load(std::memory_order_acquire);
    if (cache.epoch != cur_epoch) {
        std::memset(&cache, 0, sizeof(cache));
        cache.epoch = cur_epoch;
    }

    for (const auto &slot : cache.slots) {
        if (pc >= slot.start && pc < slot.end)
            return slot.bypass;
    }

    uintptr_t   seg_start = pc & ~static_cast<uintptr_t>(0xfffu);
    uintptr_t   seg_end   = seg_start + 0x1000u;
    uintptr_t   log_key   = 0;
    const char *mod_name  = nullptr;

    CallerPhdrQuery q{pc, 0, 0, nullptr, false};
    dl_iterate_phdr(find_caller_segment_cb, &q);
    if (q.found) {
        seg_start = q.seg_start;
        seg_end   = q.seg_end;
        log_key   = q.seg_start;
        if (q.name && q.name[0])
            mod_name = q.name;
    }

    // Fall back to dladdr() if dlpi_name was empty or the segment wasn't in phdr.
    if (!mod_name) {
        Dl_info dli{};
        if (dladdr(clean_addr, &dli) != 0 && dli.dli_fname && dli.dli_fname[0]) {
            mod_name = dli.dli_fname;
            if (log_key == 0)
                log_key = reinterpret_cast<uintptr_t>(dli.dli_fbase);
        }
    }

    bool bypass = false;
    if (mod_name) {
        bypass = is_system_or_ui_library(mod_name);
        if (mark_caller_logged_once(log_key)) {
            ALOGI("%s custom driver for caller: %s",
                  bypass ? "Bypassing" : "Routing to", mod_name);
        }
    } else {
        // Anonymous / JIT code frame — only bypass if executing on an Android UI thread.
        bypass = is_system_ui_thread();
    }

    if (seg_end > seg_start) {
        CallerCacheSlot &dst = cache.slots[cache.next & 3u];
        dst.start            = seg_start;
        dst.end              = seg_end;
        dst.bypass           = bypass;
        cache.next           = (cache.next + 1u) & 3u;
    }

    return bypass;
}

__attribute__((noinline))
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hooked_vkGetInstanceProcAddr(
    VkInstance instance, const char *pName) {
    SHADOWHOOK_STACK_SCOPE();
    const void *caller = SHADOWHOOK_RETURN_ADDRESS();
    auto fn = g_active_gipa.load(std::memory_order_acquire);
    if (fn && !should_bypass_custom_driver(caller))
        return fn(instance, pName);
    return SHADOWHOOK_CALL_PREV(hooked_vkGetInstanceProcAddr, instance, pName);
}

__attribute__((noinline))
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hooked_vkGetDeviceProcAddr(
    VkDevice device, const char *pName) {
    SHADOWHOOK_STACK_SCOPE();
    const void *caller = SHADOWHOOK_RETURN_ADDRESS();
    auto fn = g_active_gdpa.load(std::memory_order_acquire);
    if (fn && !should_bypass_custom_driver(caller))
        return fn(device, pName);
    return SHADOWHOOK_CALL_PREV(hooked_vkGetDeviceProcAddr, device, pName);
}

static bool ensure_shadowhook_ready() {
    int err = shadowhook_init(SHADOWHOOK_MODE_SHARED, false);
    if (err != SHADOWHOOK_ERRNO_OK) {
        ALOGE("shadowhook_init failed (%d): %s", err, shadowhook_to_errmsg(err));
        return false;
    }
    return true;
}

static bool populate_active_dispatch(void *loader_handle) {
    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(loader_handle, "vkGetInstanceProcAddr"));
    auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        dlsym(loader_handle, "vkGetDeviceProcAddr"));

    if (!gipa || !gdpa) {
        ALOGE("Failed to resolve vkGetInstanceProcAddr/vkGetDeviceProcAddr from custom loader");
        return false;
    }

    g_active_gipa.store(gipa, std::memory_order_release);
    g_active_gdpa.store(gdpa, std::memory_order_release);
    return true;
}

// Must be called with g_driver_mutex held.
static void clear_vulkan_hooks_locked() {
    if (g_shadow_stub_gipa) {
        shadowhook_unhook(g_shadow_stub_gipa);
        g_shadow_stub_gipa = nullptr;
    }
    if (g_shadow_stub_gdpa) {
        shadowhook_unhook(g_shadow_stub_gdpa);
        g_shadow_stub_gdpa = nullptr;
    }
}

static void *hook_one_vulkan_sym(void *sys_vk,
                                 const char *name,
                                 void *proxy,
                                 void *custom_target) {
    void *stub = nullptr;
    if (sys_vk) {
        void *sys_sym = dlsym(sys_vk, name);
        if (!sys_sym) {
            ALOGE("System libvulkan.so missing symbol %s", name);
            return nullptr;
        }
        if (sys_sym == custom_target) {
            ALOGW("Skipping hook on %s: system symbol aliases custom loader", name);
            return nullptr;
        }
        stub = shadowhook_hook_sym_addr_2(
            sys_sym, proxy, nullptr, SHADOWHOOK_HOOK_WITH_SHARED_MODE);
    } else {
        stub = shadowhook_hook_sym_name_2(
            "libvulkan.so", name, proxy, nullptr, SHADOWHOOK_HOOK_WITH_SHARED_MODE);
    }

    if (!stub) {
        int err = shadowhook_get_errno();
        if (err == SHADOWHOOK_ERRNO_PENDING) {
            ALOGI("ShadowHook pending for %s", name);
        } else {
            ALOGE("ShadowHook failed for %s (%d): %s",
                  name, err, shadowhook_to_errmsg(err));
        }
    }
    return stub;
}

// Hook only vkGetInstanceProcAddr & vkGetDeviceProcAddr on system libvulkan.so
// by exact address so private memfd/patched handles are never touched.
// Must be called with g_driver_mutex held.
static bool install_vulkan_hooks_locked() {
    if (!ensure_shadowhook_ready())
        return false;

    const bool hook_sys = env_is_truthy("ADRENOTOOLS_HOOK_SYSTEM") ||
                          env_is_truthy("ADRENOTOOLS_HOOK_HWUI");
    g_hook_system_callers.store(hook_sys, std::memory_order_relaxed);
    reset_logged_callers_locked();

    clear_vulkan_hooks_locked();

    void *sys_vk = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!sys_vk)
        sys_vk = dlopen(find_libvulkan_path(), RTLD_NOW | RTLD_LOCAL);

    g_shadow_stub_gipa = hook_one_vulkan_sym(
        sys_vk,
        "vkGetInstanceProcAddr",
        reinterpret_cast<void *>(hooked_vkGetInstanceProcAddr),
        reinterpret_cast<void *>(g_active_gipa.load(std::memory_order_relaxed)));

    g_shadow_stub_gdpa = hook_one_vulkan_sym(
        sys_vk,
        "vkGetDeviceProcAddr",
        reinterpret_cast<void *>(hooked_vkGetDeviceProcAddr),
        reinterpret_cast<void *>(g_active_gdpa.load(std::memory_order_relaxed)));

    return g_shadow_stub_gipa != nullptr && g_shadow_stub_gdpa != nullptr;
}

// ─── JNI & path resolution helpers ───────────────────────────────────────────

static bool check_and_clear_jni_exception(JNIEnv *env) {
    if (env && env->ExceptionCheck()) {
        env->ExceptionClear();
        return true;
    }
    return false;
}

struct CachedJNI {
    jclass    contextClass = nullptr;
    jclass    appInfoClass = nullptr;
    jclass    fileClass    = nullptr;
    jmethodID getAppInfo   = nullptr;
    jmethodID getCacheDir  = nullptr;
    jmethodID getAbsPath   = nullptr;
    jfieldID  nativeLibDir = nullptr;
    bool      ready        = false;
};

static CachedJNI  g_jni_cache;
static std::mutex g_jni_cache_mutex;

static bool prime_jni_cache(JNIEnv *env) {
    if (!env) return false;

    std::lock_guard<std::mutex> lock(g_jni_cache_mutex);
    if (g_jni_cache.ready) return true;

    jclass ctx = env->FindClass("android/content/Context");
    if (check_and_clear_jni_exception(env) || !ctx) return false;

    jclass ai = env->FindClass("android/content/pm/ApplicationInfo");
    if (check_and_clear_jni_exception(env) || !ai) {
        env->DeleteLocalRef(ctx);
        return false;
    }

    jclass file = env->FindClass("java/io/File");
    if (check_and_clear_jni_exception(env) || !file) {
        env->DeleteLocalRef(ctx);
        env->DeleteLocalRef(ai);
        return false;
    }

    jmethodID getAppInfo   = env->GetMethodID(ctx,  "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    check_and_clear_jni_exception(env);
    jmethodID getCacheDir  = env->GetMethodID(ctx,  "getCacheDir",        "()Ljava/io/File;");
    check_and_clear_jni_exception(env);
    jfieldID  nativeLibDir = env->GetFieldID (ai,   "nativeLibraryDir",   "Ljava/lang/String;");
    check_and_clear_jni_exception(env);
    jmethodID getAbsPath   = env->GetMethodID(file, "getAbsolutePath",    "()Ljava/lang/String;");
    check_and_clear_jni_exception(env);

    if (getAppInfo && getCacheDir && nativeLibDir && getAbsPath) {
        g_jni_cache.contextClass = static_cast<jclass>(env->NewGlobalRef(ctx));
        g_jni_cache.appInfoClass = static_cast<jclass>(env->NewGlobalRef(ai));
        g_jni_cache.fileClass    = static_cast<jclass>(env->NewGlobalRef(file));
        g_jni_cache.getAppInfo   = getAppInfo;
        g_jni_cache.getCacheDir  = getCacheDir;
        g_jni_cache.nativeLibDir = nativeLibDir;
        g_jni_cache.getAbsPath   = getAbsPath;
        g_jni_cache.ready        = true;
    }

    env->DeleteLocalRef(ctx);
    env->DeleteLocalRef(ai);
    env->DeleteLocalRef(file);

    return g_jni_cache.ready;
}

static std::string jstring_to_std(JNIEnv *env, jstring jstr) {
    if (!env || !jstr) return {};
    const char *chars = env->GetStringUTFChars(jstr, nullptr);
    if (!chars) {
        check_and_clear_jni_exception(env);
        return {};
    }
    std::string out(chars);
    env->ReleaseStringUTFChars(jstr, chars);
    return out;
}

static std::string get_native_library_dir_jni(JNIEnv *env, jobject context) {
    if (!env || !context || !prime_jni_cache(env)) return {};

    jobject appInfo = env->CallObjectMethod(context, g_jni_cache.getAppInfo);
    if (check_and_clear_jni_exception(env) || !appInfo) return {};

    jstring jPath = static_cast<jstring>(env->GetObjectField(appInfo, g_jni_cache.nativeLibDir));
    env->DeleteLocalRef(appInfo);
    if (check_and_clear_jni_exception(env) || !jPath) return {};

    std::string result = jstring_to_std(env, jPath);
    env->DeleteLocalRef(jPath);
    return result;
}

// Pure-native fallback via dladdr() if Context isn't ready yet.
static std::string get_self_library_dir() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void *>(&adrenotools_open_libvulkan), &info) == 0 ||
        !info.dli_fname) {
        return {};
    }
    if (std::strstr(info.dli_fname, "!/") != nullptr) {
        ALOGE("libadrenotools.so was loaded unextracted from APK (%s); "
              "set android:extractNativeLibs=\"true\" / useLegacyPackaging = true",
              info.dli_fname);
        return {};
    }
    const char *slash = std::strrchr(info.dli_fname, '/');
    if (!slash || slash == info.dli_fname) return {};
    return std::string(info.dli_fname, static_cast<size_t>(slash - info.dli_fname));
}

static std::string get_cache_dir_jni(JNIEnv *env, jobject context) {
    if (!env || !context || !prime_jni_cache(env)) return {};

    jobject cacheFile = env->CallObjectMethod(context, g_jni_cache.getCacheDir);
    if (check_and_clear_jni_exception(env) || !cacheFile) return {};

    jstring jPath = static_cast<jstring>(env->CallObjectMethod(cacheFile, g_jni_cache.getAbsPath));
    env->DeleteLocalRef(cacheFile);
    if (check_and_clear_jni_exception(env) || !jPath) return {};

    std::string result = jstring_to_std(env, jPath);
    env->DeleteLocalRef(jPath);
    return result;
}

// ─── Per-GPU environment tuning ──────────────────────────────────────────────

// PanVK / ARM Mali (e.g. Mali-G52) environment configuration.
// Only sets Panfrost/PanVK overrides when actually initializing for Mali so
// Adreno devices never inherit MESA_LOADER_DRIVER_OVERRIDE=panfrost.
static void applyPanvkOptimizations() {
    char gpuInfo[128] = {};
    read_first_line("/sys/class/misc/mali0/device/gpuinfo", gpuInfo, sizeof(gpuInfo));

    set_default_env("PAN_MESA_DEBUG", "kbase");
    set_default_env("PAN_EXPERIMENTAL_KBASE_GL", "1");
    set_default_env("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1");
    set_default_env("MESA_LOADER_DRIVER_OVERRIDE", "panfrost");
    set_default_env("GALLIUM_DRIVER", "panfrost");
    set_default_env("PAN_VK_DEBUG", "sync");

    if (gpuInfo[0])
        ALOGI("PanVK: Mali GPU environment configured (%s)", gpuInfo);
    else
        ALOGI("PanVK: Mali GPU environment configured");
}

// ─── Turnip & PanVK initialization ───────────────────────────────────────────

enum class DriverKind {
    Turnip,
    PanVK,
};

static void init_custom_driver(JNIEnv *env, jobject context, DriverKind kind) {
    std::lock_guard<std::mutex> lock(g_driver_mutex);

    const bool   is_panvk     = (kind == DriverKind::PanVK);
    const char  *label        = is_panvk ? "PanVK" : "Turnip";
    const char  *icd_select   = is_panvk ? "panvk" : "turnip";
    const char  *default_so   = is_panvk ? "libvulkan_panfrost.so" : "libvulkan_freedreno.so";
    const char  *tmp_subdir   = is_panvk ? "panvk_tmp" : "turnip_tmp";
    const char  *cache_subdir = is_panvk ? "panvk_shader_cache" : "turnip_shader_cache";
    void       *&slot_handle  = is_panvk ? g_panvk_handle : g_turnip_handle;

    if (is_panvk)
        applyPanvkOptimizations();

    // If this driver is already loaded, refresh the active dispatch and re-hook.
    if (slot_handle != nullptr) {
        ALOGI("%s: reusing existing loader handle and refreshing hooks", label);
        if (populate_active_dispatch(slot_handle))
            install_vulkan_hooks_locked();
        return;
    }

    // Resolve hook directory: ADRENOTOOLS_HOOK_DIR -> Context.nativeLibraryDir -> dladdr().
    std::string hook_dir;
    if (const char *env_hook = std::getenv("ADRENOTOOLS_HOOK_DIR"); env_hook && env_hook[0]) {
        hook_dir = env_hook;
        while (hook_dir.size() > 1 && hook_dir.back() == '/') hook_dir.pop_back();
    } else {
        hook_dir = get_native_library_dir_jni(env, context);
        if (hook_dir.empty())
            hook_dir = get_self_library_dir();
    }

    if (hook_dir.empty()) {
        ALOGE("%s: could not resolve native library directory", label);
        return;
    }

    // Resolve driver directory: ADRENOTOOLS_DRIVER_DIR -> ADRENOTOOLS_DRIVER_PATH -> hook_dir/.
    const char *env_drv_dir = std::getenv("ADRENOTOOLS_DRIVER_DIR");
    if (!env_drv_dir || !env_drv_dir[0])
        env_drv_dir = std::getenv("ADRENOTOOLS_DRIVER_PATH");
    std::string driver_dir = ensure_trailing_slash(
        (env_drv_dir && env_drv_dir[0]) ? env_drv_dir : hook_dir.c_str());

    // Resolve driver library name: ADRENOTOOLS_DRIVER_NAME -> default Turnip/PanVK soname.
    const char *env_drv_name = std::getenv("ADRENOTOOLS_DRIVER_NAME");
    const char *driver_name  = (env_drv_name && env_drv_name[0]) ? env_drv_name : default_so;

    ALOGI("%s: hookDir=%s driver=%s%s", label, hook_dir.c_str(), driver_dir.c_str(), driver_name);

    setenv("MESA_LIBGL_DRIVERS_PATH", driver_dir.c_str(), 1);
    setenv("MESA_VULKAN_ICD_SELECT",  icd_select,         1);

    // Set up shader cache & legacy (< API 29) tmp directory when Context is available.
    std::string tmp_dir;
    std::string base_cache = get_cache_dir_jni(env, context);
    if (!base_cache.empty()) {
        tmp_dir               = base_cache + "/" + tmp_subdir + "/";
        std::string cache_dir = base_cache + "/" + cache_subdir + "/";

        struct stat st{};
        if (stat(tmp_dir.c_str(), &st) != 0)   mkdir(tmp_dir.c_str(),   0775);
        if (stat(cache_dir.c_str(), &st) != 0) mkdir(cache_dir.c_str(), 0775);

        set_default_env("MESA_SHADER_CACHE_DIR", cache_dir.c_str());
    }

    // Use RTLD_LOCAL so default-namespace dlsym("libvulkan.so") stays distinct
    // and our ShadowHook proxies route system calls cleanly into this handle.
    void *handle = adrenotools_open_libvulkan(
        RTLD_LOCAL | RTLD_NOW,
        ADRENOTOOLS_DRIVER_CUSTOM,
        tmp_dir.empty() ? nullptr : tmp_dir.c_str(),
        hook_dir.c_str(),
        driver_dir.c_str(),
        driver_name,
        nullptr,
        nullptr
    );

    if (!handle) {
        ALOGE("%s: adrenotools_open_libvulkan failed", label);
        return;
    }

    if (!populate_active_dispatch(handle)) {
        dlclose(handle);
        return;
    }

    slot_handle = handle;

    ALOGI("%s loaded — installing Vulkan ShadowHooks", label);
    if (!install_vulkan_hooks_locked()) {
        ALOGE("%s: required ShadowHook installation failed", label);
        dlclose(handle);
        return;
    }

    if (!is_panvk) {
#ifdef OVERCLOCK
        ALOGI("Overclock mode — priority boost (turbo disabled by default to prevent GPU reboots)");
        if (setpriority(PRIO_PROCESS, 0, -20) != 0) {
            ALOGI("setpriority -20 denied, trying -10");
            setpriority(PRIO_PROCESS, 0, -10);
        }
#else
        ALOGI("Standard mode");
#endif
    }
    // NOTE: adrenotools_set_turbo() is intentionally NOT called automatically.
    // IOCTL_KGSL_SETPROPERTY with KGSL_PROP_PWRCTRL can crash the KGSL driver
    // on some Adreno kernels, causing a full device reboot. Call it manually
    // via the public API only if you need it and have tested on your device.

    ALOGI("%s hooks active (system UI bypass: %s)",
          label,
          g_hook_system_callers.load(std::memory_order_relaxed) ? "off" : "on");
}

static void init_turnip_driver(JNIEnv *env, jobject context) {
    init_custom_driver(env, context, DriverKind::Turnip);
}

static void init_panvk_driver(JNIEnv *env, jobject context) {
    init_custom_driver(env, context, DriverKind::PanVK);
}

static void init_detected_driver(JNIEnv *env, jobject context) {
    if (isMaliGpu()) {
        ALOGI("Mali GPU detected — initializing PanVK driver");
        init_panvk_driver(env, context);
    } else {
        init_turnip_driver(env, context);
    }
}

// ─── Library constructor & JNI / native-dlopen bootstrap ─────────────────────

static void schedule_native_jni_init();

__attribute__((constructor))
static void global_atomic_init() {
    // ─── Core Mesa/Vulkan env ───
    // MESA_VULKAN_ICD_SELECT is set per-driver in init_custom_driver() so PanVK
    // on Mali isn't overridden by a Turnip default (or vice versa).
    set_default_env("MESA_VK_IGNORE_CONFORMANCE_WARNING",          "1");
    set_default_env("MESA_VK_IGNORE_CONFORMANCE_ERRORS",           "1");
    set_default_env("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE",  "1");
    set_default_env("MESA_SHADER_CACHE_DISABLE",                   "false");
    set_default_env("MESA_SHADER_CACHE_MAX_SIZE",                  "4G");
    unsetenv("MESA_DISK_CACHE_SINGLE_FILE");
    unsetenv("MESA_DISK_CACHE_READ_ONLY");

    // ─── Perf / debug ───
    // Leave MESA_DEBUG unset by default so release builds stay quiet during normal
    // rendering while still logging genuine TU/PanVK startup errors to logcat.
    set_default_env("GALLIUM_PRINT_OPTIONS",       "0");
    set_default_env("MESA_NO_ERROR",               "1");
    set_default_env("MESA_GLTHREAD",               "true");
    set_default_env("vblank_mode",                 "0");
    set_default_env("MESA_TEXTURE_MAX_ANISOTROPY", "4");

    // ─── WSI present mode ───
#ifdef OVERCLOCK
    set_default_env("MESA_VK_WSI_PRESENT_MODE", "immediate");
#else
    set_default_env("MESA_VK_WSI_PRESENT_MODE", "mailbox");
#endif

    // ─── Unity ───
    set_default_env("UNITY_DISABLE_GRAPHICS_DRIVER_CHECK",  "1");
    set_default_env("UNITY_VULKAN_ENABLE_VALIDATION_LAYERS", "0");
    set_default_env("UNITY_GFX_DEVICE_API",                 "vulkan");

    char sdk_str[PROP_VALUE_MAX] = {};
    __system_property_get("ro.build.version.sdk", sdk_str);
    int sdk = std::atoi(sdk_str);
    ALOGI("Android SDK: %d", sdk);

    if (isMaliGpu()) {
        applyPanvkOptimizations();
    } else {
        // A related Samsung Adreno 650 device has been reported to hit a
        // fatal NoC error with Turnip UBWC. This is a mitigation, not proof of
        // the root cause on every device. Preserve explicit TU_DEBUG settings.
        char gpu_model[128] = {};
        read_first_line("/sys/class/kgsl/kgsl-3d0/gpu_model", gpu_model, sizeof(gpu_model));
        if (set_adreno_650_turnip_default(gpu_model))
            ALOGW("Adreno 650: defaulting TU_DEBUG=noubwc as a Turnip compatibility workaround");
        else
            ALOGI("Adreno: leaving Turnip tuning unchanged (GPU model: %s)",
                  gpu_model[0] ? gpu_model : "unknown");
    }

    // Defer ShadowHook + JNI initialization until after dlopen() releases the
    // linker lock. If loaded via native dlopen(), ART never invokes JNI_OnLoad,
    // so this fallback picks up the running JavaVM automatically.
    if (!env_is_truthy("ADRENOTOOLS_DISABLE")) {
        schedule_native_jni_init();
    } else {
        ALOGI("ADRENOTOOLS_DISABLE set — skipping automatic Vulkan hook init");
    }
}

static jobject query_current_application(JNIEnv *env) {
    if (!env) return nullptr;

    jclass atCls = env->FindClass("android/app/ActivityThread");
    if (check_and_clear_jni_exception(env) || !atCls) return nullptr;

    jmethodID caMid = env->GetStaticMethodID(
        atCls, "currentApplication", "()Landroid/app/Application;");
    if (check_and_clear_jni_exception(env) || !caMid) {
        env->DeleteLocalRef(atCls);
        return nullptr;
    }

    jobject app = env->CallStaticObjectMethod(atCls, caMid);
    env->DeleteLocalRef(atCls);
    if (check_and_clear_jni_exception(env)) return nullptr;
    return app;
}

static void perform_init(JavaVM *vm) {
    if (env_is_truthy("ADRENOTOOLS_DISABLE")) {
        ALOGI("perform_init: skipped (ADRENOTOOLS_DISABLE)");
        return;
    }

    ALOGI("perform_init: started");

    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    }

    if (jobject app = query_current_application(env)) {
        ALOGI("perform_init: Application context ready, initializing immediately");
        init_detected_driver(env, app);
        env->DeleteLocalRef(app);
        return;
    }

    // Application not bound yet (e.g. attachBaseContext / early native load).
    // Poll for up to 5 seconds (200 * 25 ms), then fall back to dladdr() path.
    std::thread([vm]() {
        JNIEnv *t_env = nullptr;
        if (vm->AttachCurrentThread(&t_env, nullptr) != JNI_OK)
            return;

        jobject t_app = nullptr;
        for (int i = 0; i < 200 && !t_app; ++i) {
            t_app = query_current_application(t_env);
            if (!t_app)
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        if (t_app) {
            init_detected_driver(t_env, t_app);
            t_env->DeleteLocalRef(t_app);
        } else {
            ALOGW("perform_init: Application context timed out — falling back to dladdr()");
            init_detected_driver(t_env, nullptr);
        }

        vm->DetachCurrentThread();
    }).detach();
}

// Ensure automatic init executes at most once across JNI_OnLoad and native fallback.
static std::once_flag g_init_once;

static void run_init_once(JavaVM *vm) {
    std::call_once(g_init_once, [vm]() {
        perform_init(vm);
    });
}

extern "C" JNIEXPORT void JNICALL adrenotools_init_panvk_driver(JavaVM *vm) {
    if (!vm) return;

    JNIEnv *env = nullptr;
    bool attached = false;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }

    jobject app = query_current_application(env);
    ALOGI("adrenotools_init_panvk_driver: loading PanVK driver for Mali GPU");
    init_panvk_driver(env, app);
    if (app) env->DeleteLocalRef(app);

    if (attached) vm->DetachCurrentThread();
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void * /*reserved*/) {
    run_init_once(vm);
    return JNI_VERSION_1_6;
}

// ─── Native-dlopen fallback ──────────────────────────────────────────────────
// ART only invokes JNI_OnLoad for System.load/loadLibrary. When loaded via
// dlopen(), DT_NEEDED, or LD_PRELOAD, look up the existing JavaVM directly.
static JavaVM *find_created_java_vm() {
    using GetVMsFn = jint (*)(JavaVM **, jsize, jsize *);

    GetVMsFn fn = reinterpret_cast<GetVMsFn>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!fn) {
        for (const char *lib : {"libnativehelper.so", "libart.so"}) {
            void *h = dlopen(lib, RTLD_NOW | RTLD_NOLOAD);
            if (!h) h = dlopen(lib, RTLD_NOW);
            if (!h) continue;
            fn = reinterpret_cast<GetVMsFn>(dlsym(h, "JNI_GetCreatedJavaVMs"));
            if (fn) break;
        }
    }
    if (!fn) return nullptr;

    JavaVM *vm  = nullptr;
    jsize count = 0;
    if (fn(&vm, 1, &count) != JNI_OK || count < 1) return nullptr;
    return vm;
}

// Runs on a detached thread so it naturally waits for the dynamic loader lock
// held during our ELF constructor to be released before calling dlopen().
static void schedule_native_jni_init() {
    std::thread([]() {
        JavaVM *vm = nullptr;
        for (int i = 0; i < 200 && !(vm = find_created_java_vm()); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));

        if (!vm) {
            ALOGW("native-load fallback: no JavaVM found — initializing via dladdr() only");
            init_detected_driver(nullptr, nullptr);
            return;
        }

        JNIEnv *env = nullptr;
        bool attached = false;
        if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
                ALOGE("native-load fallback: AttachCurrentThread failed");
                return;
            }
            attached = true;
        }

        ALOGI("native-load fallback: running init (JNI_OnLoad may not be called)");
        run_init_once(vm);

        if (attached) vm->DetachCurrentThread();
    }).detach();
}
