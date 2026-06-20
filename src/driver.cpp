// SPDX-License-Identifier: BSD-2-Clause
// Copyright © 2021 Billy Laws

#include <vulkan/vulkan.h>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <android/api-level.h>
#include <android/log.h>
#include <android_linker_ns.h>
#include "hook/kgsl.h"
#include "hook/hook_impl_params.h"
#include <adrenotools/driver.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <pwd.h>
#include <cstring>
#include <cstdlib>
#include <jni.h>
#include <atomic>
#include <stdatomic.h>
#include <pthread.h>
#include <vector>
#include <mutex>
#include <sys/resource.h>
#include <sys/system_properties.h>
#include <iostream>
#include <android/dlext.h>
#include <link.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <dirent.h>

#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, "AdrenoToolsPatch", __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, "AdrenoToolsPatch", __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AdrenoToolsPatch", __VA_ARGS__)
#define MAX_FILENAME_SCAN 512

bool adrenotools_install_hook(const char *hookLibDir, int featureFlags,
                              const char *customDriverDir, const char *customDriverName,
                              const char *fileRedirectDir)
{
    if (!linkernsbypass_load_status()) {
        ALOGE("FAILURE: linkernsbypass not loaded");
        return false;
    }
    if (!hookLibDir) {
        ALOGE("hookLibDir is required");
        return false;
    }

    adrenotools_gpu_mapping *importMapping = nullptr;
    if (featureFlags & ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT) {
        importMapping = new adrenotools_gpu_mapping{};
    }

    HookImplParams *params = new HookImplParams(
        featureFlags, nullptr, hookLibDir,
        customDriverDir ? customDriverDir : "",
        customDriverName ? customDriverName : "",
        fileRedirectDir ? fileRedirectDir : "",
        importMapping
    );

    std::string hookImplPath = std::string(hookLibDir) + "/libhook_impl.so";
    void *hookImpl = dlopen(hookImplPath.c_str(), RTLD_NOW);
    if (!hookImpl) hookImpl = dlopen("libhook_impl.so", RTLD_NOW);
    if (!hookImpl) {
        ALOGE("Could not load libhook_impl.so");
        delete params;
        return false;
    }

    auto initHookParam = reinterpret_cast<void (*)(const void *)>(dlsym(hookImpl, "init_hook_param"));
    if (!initHookParam) {
        ALOGE("init_hook_param not found");
        dlclose(hookImpl);
        delete params;
        return false;
    }
    initHookParam(params);

    std::string mainPath = std::string(hookLibDir) + "/libmain_hook.so";
    void *mainHook = dlopen(mainPath.c_str(), RTLD_GLOBAL | RTLD_NOW);
    if (!mainHook) mainHook = dlopen("libmain_hook.so", RTLD_GLOBAL | RTLD_NOW);
    if (mainHook) {
        ALOGI("libmain_hook.so loaded with RTLD_GLOBAL into app namespace");
    }

    if (featureFlags & ADRENOTOOLS_DRIVER_FILE_REDIRECT) {
        std::string fr = std::string(hookLibDir) + "/libfile_redirect_hook.so";
        dlopen(fr.c_str(), RTLD_GLOBAL | RTLD_NOW) || dlopen("libfile_redirect_hook.so", RTLD_GLOBAL | RTLD_NOW);
    }
    if (featureFlags & ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT) {
        std::string gsl = std::string(hookLibDir) + "/libgsl_alloc_hook.so";
        dlopen(gsl.c_str(), RTLD_GLOBAL | RTLD_NOW) || dlopen("libgsl_alloc_hook.so", RTLD_GLOBAL | RTLD_NOW);
    }

    ALOGI("adrenotools_install_hook SUCCESS");
    return true;
}

void *adrenotools_open_libvulkan(int dlopenFlags, int featureFlags, const char *tmpLibDir,
                                 const char *hookLibDir, const char *customDriverDir,
                                 const char *customDriverName, const char *fileRedirectDir,
                                 void **userMappingHandle)
{
    if (!linkernsbypass_load_status()) {
        ALOGE("FAILURE: Could not load linkernsbypass\n");
        return nullptr;
    }

    if (android_get_device_api_level() >= 29 && !tmpLibDir)
        tmpLibDir = nullptr;

    if (!(featureFlags & ADRENOTOOLS_DRIVER_FILE_REDIRECT) && fileRedirectDir) {
        ALOGE("FAILURE: ADRENOTOOLS_DRIVER_FILE_REDIRECT present but no file redirect folder found\n");
        return nullptr;
    }

    if (!(featureFlags & ADRENOTOOLS_DRIVER_CUSTOM) && (customDriverDir || customDriverName)) {
        ALOGE("FAILURE: ADRENOTOOLS_DRIVER_CUSTOM present but no custom driver name or folder found\n");
        return nullptr;
    }

    if (!(featureFlags & ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT) && userMappingHandle) {
        ALOGE("FAILURE: ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT present but no user mapping handle found\n");
        return nullptr;
    }

    struct stat buf{};

    if (featureFlags & ADRENOTOOLS_DRIVER_CUSTOM) {
        if (!customDriverName || !customDriverDir) {
            ALOGE("FAILURE: ADRENOTOOLS_DRIVER_CUSTOM present but no custom driver name or folder parameter was specified\n");
            return nullptr;
        }
        if (stat((std::string(customDriverDir) + customDriverName).c_str(), &buf) != 0) {
            ALOGE("FAILURE: ADRENOTOOLS_DRIVER_CUSTOM present but importable driver doesn't exist\n");
            return nullptr;
        }
    }

    if (featureFlags & ADRENOTOOLS_DRIVER_FILE_REDIRECT) {
        if (!fileRedirectDir) {
            ALOGE("FAILURE: ADRENOTOOLS_DRIVER_REDIRECT_DIR present but no folder parameter was found\n");
            return nullptr;
        }
        if (stat(fileRedirectDir, &buf) != 0) {
            ALOGE("FAILURE: ADRENOTOOLS_DRIVER_REDIRECT_DIR present but specified redirect folder doesn't exist\n");
            return nullptr;
        }
    }

    auto hookNs{android_create_namespace("adrenotools-libvulkan", hookLibDir, nullptr, ANDROID_NAMESPACE_TYPE_SHARED, nullptr, nullptr)};
    if (!linkernsbypass_link_namespace_to_default_all_libs(hookNs)) {
        return nullptr;
    }

    auto hookImpl{linkernsbypass_namespace_dlopen("libhook_impl.so", RTLD_NOW, hookNs)};
    if (!hookImpl) {
        ALOGE("FAILURE: Couldn't preload the hook implementation\n");
        return nullptr;
    }

    auto initHookParam{reinterpret_cast<void (*)(const void *)>(dlsym(hookImpl, "init_hook_param"))};
    if (!initHookParam) {
        ALOGE("FAILURE: Couldn't init hook params\n");
        return nullptr;
    }

    auto importMapping{[&]() -> adrenotools_gpu_mapping * {
        if (featureFlags & ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT) {
            adrenotools_gpu_mapping *mapping{new adrenotools_gpu_mapping{}};
            *userMappingHandle = mapping;
            return mapping;
        } else {
            ALOGW("WARN: Memory mapping flag was not specified\n");
            return nullptr;
        }
    }()};

    initHookParam(new HookImplParams(featureFlags, tmpLibDir, hookLibDir, customDriverDir, customDriverName, fileRedirectDir, importMapping));

    if (!linkernsbypass_namespace_dlopen("libmain_hook.so", RTLD_GLOBAL, hookNs)) {
        ALOGE("FAILURE: Failed to load libvulkan into the isolated namespace\n");
        return nullptr;
    }

    return linkernsbypass_namespace_dlopen_unique("/system/lib64/libvulkan.so", tmpLibDir, dlopenFlags, hookNs);
}

bool adrenotools_import_user_mem(void *handle, void *hostPtr, uint64_t size) {
    auto importMapping{reinterpret_cast<adrenotools_gpu_mapping *>(handle)};

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

    int kgslFd{open("/dev/kgsl-3d0", O_RDWR)};
    if (kgslFd < 0)
        return false;

    int ret{ioctl(kgslFd, IOCTL_KGSL_GPUOBJ_IMPORT, &userMemImport)};
    if (ret)
        goto err;

    info.id = userMemImport.id;
    ret = ioctl(kgslFd, IOCTL_KGSL_GPUOBJ_INFO, &info);
    if (ret)
        goto err;

    importMapping->host_ptr = hostPtr;
    importMapping->gpu_addr = info.gpuaddr;
    importMapping->size = size;
    importMapping->flags = 0xc2600;

    close(kgslFd);
    return true;

err:
    close(kgslFd);
    return false;
}

bool adrenotools_mem_gpu_allocate(void *handle, uint64_t *size) {
    auto mapping{reinterpret_cast<adrenotools_gpu_mapping *>(handle)};

    kgsl_gpuobj_alloc gpuobjAlloc{};
    gpuobjAlloc.size  = *size;
    gpuobjAlloc.flags = KGSL_CACHEMODE_WRITEBACK << KGSL_CACHEMODE_SHIFT
                      | KGSL_MEMFLAGS_IOCOHERENT;

    kgsl_gpuobj_info info{};

    int kgslFd{open("/dev/kgsl-3d0", O_RDWR)};
    if (kgslFd < 0)
        return false;

    int ret{ioctl(kgslFd, IOCTL_KGSL_GPUOBJ_ALLOC, &gpuobjAlloc)};
    if (ret)
        goto err;

    *size = gpuobjAlloc.mmapsize;

    info.id = gpuobjAlloc.id;

    ret = ioctl(kgslFd, IOCTL_KGSL_GPUOBJ_INFO, &info);
    if (ret)
        goto err;

    mapping->host_ptr = nullptr;
    mapping->gpu_addr = info.gpuaddr;
    mapping->size = *size;
    mapping->flags = 0xc2600;

    close(kgslFd);
    return true;

err:
    close(kgslFd);
    return false;
}

bool adrenotools_mem_cpu_map(void *handle, void *hostPtr, uint64_t size) {
    auto mapping{reinterpret_cast<adrenotools_gpu_mapping *>(handle)};

    int kgslFd{open("/dev/kgsl-3d0", O_RDWR)};
    if (kgslFd < 0)
        return false;

    mapping->host_ptr = mmap(hostPtr, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, kgslFd, mapping->gpu_addr);
    close(kgslFd);
    return mapping->host_ptr != nullptr;
}

bool adrenotools_validate_gpu_mapping(void *handle) {
    auto importMapping{reinterpret_cast<adrenotools_gpu_mapping *>(handle)};
    return importMapping->gpu_addr == ADRENOTOOLS_GPU_MAPPING_SUCCEEDED_MAGIC;
}

void adrenotools_set_turbo(bool turbo) {
    uint32_t enable{turbo ? 0U : 1U};
    kgsl_device_getproperty prop{
        .type = KGSL_PROP_PWRCTRL,
        .value = reinterpret_cast<void *>(&enable),
        .sizebytes = sizeof(enable),
    };

    int kgslFd{open("/dev/kgsl-3d0", O_RDWR)};
    if (kgslFd < 0)
        return;

    ioctl(kgslFd, IOCTL_KGSL_SETPROPERTY, &prop);
    close(kgslFd);
}

bool adrenotools_set_freedreno_env(const char *varName, const char *value) {
    if (!varName || !value || std::strlen(varName) == 0)
        return false;

    int result = setenv(varName, value, 1);
    if (result != 0) {
        ALOGE("FAILURE adrenotools_set_freedreno_env: Failed to set '%s' (errno: %d)", varName, errno);
        return false;
    }

    const char *verifyValue = std::getenv(varName);
    if (verifyValue && std::strcmp(verifyValue, value) == 0) {
        return true;
    } else {
        ALOGW("WARN adrenotools_set_freedreno_env: Verification failed for '%s'", varName);
        return false;
    }
}

static void *g_turnip_handle = NULL;
static PFN_vkGetInstanceProcAddr g_turnip_gipa = NULL;
static PFN_vkGetDeviceProcAddr g_turnip_gdpa = nullptr;
static JavaVM* g_java_vm = nullptr; 

__attribute__((always_inline))
static inline PFN_vkVoidFunction hooked_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return g_turnip_gipa(instance, pName);
}

__attribute__((always_inline))
static inline PFN_vkVoidFunction hooked_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return g_turnip_gdpa(device, pName);
}

struct CachedJNI {
    jclass  contextClass   = nullptr;
    jclass  appInfoClass   = nullptr;
    jclass  fileClass      = nullptr;
    jmethodID getAppInfo   = nullptr;
    jmethodID getCacheDir  = nullptr;
    jmethodID getAbsPath   = nullptr;
    jfieldID  nativeLibDir = nullptr;
    bool ready             = false;
};
static CachedJNI g_jni_cache;

static void prime_jni_cache(JNIEnv* env) {
    if (g_jni_cache.ready) return;

    jclass ctx  = env->FindClass("android/content/Context");
    jclass ai   = env->FindClass("android/content/pm/ApplicationInfo");
    jclass file = env->FindClass("java/io/File");

    if (!ctx || !ai || !file) return;

    g_jni_cache.contextClass   = (jclass)env->NewGlobalRef(ctx);
    g_jni_cache.appInfoClass   = (jclass)env->NewGlobalRef(ai);
    g_jni_cache.fileClass      = (jclass)env->NewGlobalRef(file);

    g_jni_cache.getAppInfo     = env->GetMethodID(ctx,  "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    g_jni_cache.getCacheDir    = env->GetMethodID(ctx,  "getCacheDir",        "()Ljava/io/File;");
    g_jni_cache.nativeLibDir   = env->GetFieldID (ai,   "nativeLibraryDir",   "Ljava/lang/String;");
    g_jni_cache.getAbsPath     = env->GetMethodID(file, "getAbsolutePath",    "()Ljava/lang/String;");

    env->DeleteLocalRef(ctx);
    env->DeleteLocalRef(ai);
    env->DeleteLocalRef(file);

    g_jni_cache.ready = true;
}

static char* get_native_library_dir(JNIEnv* env, jobject context) {
    if (!context) return nullptr;

    prime_jni_cache(env);
    if (!g_jni_cache.ready) return nullptr;

    jobject appInfo = env->CallObjectMethod(context, g_jni_cache.getAppInfo);
    if (!appInfo) return nullptr;

    jstring jPath = (jstring)env->GetObjectField(appInfo, g_jni_cache.nativeLibDir);
    env->DeleteLocalRef(appInfo);
    if (!jPath) return nullptr;

    const char* path_chars = env->GetStringUTFChars(jPath, nullptr);
    char* result = path_chars ? strdup(path_chars) : nullptr;
    if (path_chars) env->ReleaseStringUTFChars(jPath, path_chars);
    env->DeleteLocalRef(jPath);

    return result;
}

static void read_first_line(const char* path, char* out, size_t out_len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;

    ssize_t n = read(fd, out, out_len - 1);
    close(fd);

    if (n > 0) {
        // strip newline
        char* nl = (char*)memchr(out, '\n', n);
        out[nl ? (nl - out) : n] = '\0';
    }
}

void applyTurnipOptimizations() {
    char gpuName[64] = {};

    read_first_line("/sys/class/kgsl/kgsl-3d0/gpu_model",           gpuName, sizeof(gpuName));
    if (!gpuName[0]) read_first_line("/proc/device-tree/model",     gpuName, sizeof(gpuName));
    if (!gpuName[0]) read_first_line("/sys/firmware/devicetree/base/model", gpuName, sizeof(gpuName));

    if (!gpuName[0]) {
        // Unknown GPU — safe conservative fallback
#ifdef OVERCLOCK
        setenv("TU_DEBUG", "noconform,noflushall,dynamic,unaligned_store,deck_emu,forcecb", 1);
#else
        setenv("TU_DEBUG", "noconform,hiprio,noflushall,dynamic,deck_emu,forcecb", 1);
#endif
        return;
    }
	
    int gen = 0;
    for (const char* p = gpuName; *p; ++p) {
        if (*p >= '5' && *p <= '8') { gen = *p - '0'; break; }
    }

    switch (gen) {
        case 8:
            ALOGI("Adreno 8xx: sysmem + cache-coherent");
            setenv("tu_override_uncached_as_cache_coherent", "true", 1);
#ifdef OVERCLOCK
            setenv("TU_DEBUG", "noconform,sysmem,noflushall,hiprio,dynamic,unaligned_store,deck_emu", 1);
#else
            setenv("TU_DEBUG", "noconform,sysmem,hiprio,noflushall,dynamic,deck_emu", 1);
#endif
            break;

        case 7:
            ALOGI("Adreno 7xx: autotuner path");
            setenv("tu_override_uncached_as_cache_coherent", "true", 1);
#ifdef OVERCLOCK
            setenv("TU_DEBUG", "noconform,noflushall,hiprio,dynamic,unaligned_store,deck_emu", 1);
#else
            setenv("TU_DEBUG", "noconform,hiprio,noflushall,dynamic,deck_emu", 1);
#endif
            break;

        default:
            // Adreno 5xx / 6xx — sysmem is more stable on older silicon
            ALOGI("Adreno 5xx/6xx: sysmem stability path");
#ifdef OVERCLOCK
            setenv("TU_DEBUG", "noconform,sysmem,hiprio,noflushall,dynamic,unaligned_store,deck_emu", 1);
#else
            setenv("TU_DEBUG", "noconform,sysmem,hiprio,noflushall,dynamic,deck_emu", 1);
#endif
            break;
    }
}

static void init_turnip_driver(JNIEnv* env, jobject context) {
    if (g_turnip_handle != nullptr) {
        ALOGI("init_turnip_driver: Turnip already initialized");
        return;
    }
	
    char fixed_dir[512]  = {};
    char tmpdir[512]     = {};
    char cache_dir[512]  = {};

    char* native_lib_dir = get_native_library_dir(env, context);
    if (!native_lib_dir) {
        ALOGE("init_turnip_driver: could not resolve native lib dir");
        return;
    }

    snprintf(fixed_dir, sizeof(fixed_dir), "%s/", native_lib_dir);
    ALOGI("Native Lib Dir: %s", fixed_dir);
    setenv("MESA_LIBGL_DRIVERS_PATH", fixed_dir, 1);
	
    prime_jni_cache(env);
    jobject cacheFileObj = env->CallObjectMethod(context, g_jni_cache.getCacheDir);
    jstring jPath        = (jstring)env->CallObjectMethod(cacheFileObj, g_jni_cache.getAbsPath);

    const char* base_cache_path = env->GetStringUTFChars(jPath, nullptr);

    snprintf(tmpdir,    sizeof(tmpdir),    "%s/turnip_tmp/",          base_cache_path);
    snprintf(cache_dir, sizeof(cache_dir), "%s/turnip_shader_cache/", base_cache_path);
	
    struct stat st;
    if (stat(tmpdir,    &st) != 0) mkdir(tmpdir,    0775);
    if (stat(cache_dir, &st) != 0) mkdir(cache_dir, 0775);

    setenv("MESA_SHADER_CACHE_DIR", cache_dir, 1);
    
    adrenotools_install_hook(native_lib_dir, ADRENOTOOLS_DRIVER_CUSTOM,
                             native_lib_dir, "libvulkan_freedreno.so", nullptr);
    
    g_turnip_handle = adrenotools_open_libvulkan(
        RTLD_GLOBAL | RTLD_NOW,
        ADRENOTOOLS_DRIVER_CUSTOM,
        tmpdir,
        native_lib_dir,
        fixed_dir,
        "libvulkan_freedreno.so",
        NULL,
        NULL
    );

    if (!g_turnip_handle) { 
        ALOGE("adrenotools_open_libvulkan failed - falling back or check custom driver presence in native lib dir"); 
        goto cleanup; 
    }

    g_turnip_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_turnip_handle, "vkGetInstanceProcAddr");
    if (!g_turnip_gipa) {
        ALOGE("dlsym vkGetInstanceProcAddr failed");
        dlclose(g_turnip_handle); g_turnip_handle = nullptr;
        goto cleanup;
    }

    g_turnip_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(g_turnip_handle, "vkGetDeviceProcAddr");
    if (!g_turnip_gdpa) {
        ALOGE("dlsym vkGetDeviceProcAddr failed");
        dlclose(g_turnip_handle); g_turnip_handle = nullptr;
        goto cleanup;
    }

    ALOGI("Turnip loaded successfully via hook_impl (no shadowhook/bytehook). Use g_turnip_g*pa for Vulkan setup to get custom driver name/properties.");

#ifdef OVERCLOCK
    ALOGI("Overclock mode — turbo + priority boost");
    adrenotools_set_turbo(true);
    if (setpriority(PRIO_PROCESS, 0, -20) != 0) {
        ALOGI("setpriority -20 denied, trying -10");
        setpriority(PRIO_PROCESS, 0, -10);
    }
#else
    ALOGI("Standard mode");
    adrenotools_set_turbo(false);
#endif

    ALOGI("Turnip ready (hook_impl only)");

cleanup:
    env->ReleaseStringUTFChars(jPath, base_cache_path);
    env->DeleteLocalRef(cacheFileObj);
    env->DeleteLocalRef(jPath);
    free(native_lib_dir);
}

__attribute__((constructor))
static void global_atomic_init() {
    // ─── Core Mesa/Vulkan env ───
    setenv("MESA_VULKAN_ICD_SELECT",              "turnip",   1);
    setenv("MESA_VK_IGNORE_CONFORMANCE_WARNING",  "1",        1);
    setenv("MESA_VK_IGNORE_CONFORMANCE_ERRORS",   "1",        1);
    setenv("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE", "1", 1);
    setenv("MESA_SHADER_CACHE_DISABLE",           "false",    1);
    setenv("MESA_SHADER_CACHE_MAX_SIZE",          "4G",       1);
    unsetenv("MESA_DISK_CACHE_SINGLE_FILE");
    unsetenv("MESA_DISK_CACHE_READ_ONLY");

    // ─── Perf / debug ───
    setenv("GALLIUM_PRINT_OPTIONS", "0",      1);
    setenv("MESA_DEBUG",            "silent", 1);
    setenv("MESA_NO_ERROR",         "1",      1);
    setenv("MESA_GLTHREAD",         "true",   1);
    setenv("vblank_mode",           "0",      1);
    setenv("TU_ROBUST_BUFFER_ACCESS", "0",   1);
    setenv("TU_GMEM_ALLOW_OVERLAP",   "1",   1);
    setenv("TU_RENDERPASS_CACHE",     "1",   1);

    // ─── Anisotropy: 4x is a good balance for weak CPUs — 16x wastes fillrate ───
    setenv("MESA_TEXTURE_MAX_ANISOTROPY", "4", 1);

    // ─── WSI present mode ───
#ifdef OVERCLOCK
    setenv("MESA_VK_WSI_PRESENT_MODE", "immediate", 1);
#else
    setenv("MESA_VK_WSI_PRESENT_MODE", "mailbox",   1);
#endif

    // ─── Unity ───
    setenv("UNITY_DISABLE_GRAPHICS_DRIVER_CHECK",  "1",      1);
    setenv("UNITY_VULKAN_ENABLE_VALIDATION_LAYERS", "0",     1);
    setenv("UNITY_GFX_DEVICE_API",                 "vulkan", 1);

    // ─── Heap size: cap at 40% of RAM on weak devices to avoid OOM kills ───
    {
        long pages     = sysconf(_SC_PHYS_PAGES);
        long page_size = sysconf(_SC_PAGESIZE);
        long long total_mb = ((long long)pages * page_size) / (1024 * 1024);

        long heap_mb = (long)(total_mb * 40 / 100);
        if (heap_mb < 256) heap_mb = 256;

        char heap_str[16];
        snprintf(heap_str, sizeof(heap_str), "%ld", heap_mb);
        setenv("TU_OVERRIDE_HEAP_SIZE", heap_str, 1);
        ALOGI("TU_OVERRIDE_HEAP_SIZE=%s MB (40%% of %lld MB RAM)", heap_str, total_mb);
    }

    // ─── SDK / One UI detection ───
    {
        char sdk_str[8] = {};
        __system_property_get("ro.build.version.sdk", sdk_str);
        int sdk = atoi(sdk_str);
        ALOGI("Android SDK: %d", sdk);

        unsetenv("MESA_VK_VERSION_OVERRIDE");
        unsetenv("FD_DEV_FEATURES");

        char oneui_str[PROP_VALUE_MAX] = {};
        if (__system_property_get("ro.build.version.oneui", oneui_str) > 0) {
            int raw = atoi(oneui_str);
            if (raw >= 60000 && sdk >= 30) {
                int major = raw / 10000, minor = (raw % 10000) / 100;
                ALOGI("One UI %d.%d detected — enabling UBWC flag hint", major, minor);
                setenv("FD_DEV_FEATURES", "enable_tp_ubwc_flag_hint=1", 1);
            }
        }
    }

    applyTurnipOptimizations();
}

void perform_init(JavaVM* vm) {
    ALOGI("JNI_OnLoad: started");

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    }

    // ─── Cache ActivityThread lookups as globals — only done once ───
    static jclass activityThreadCls = (jclass)env->NewGlobalRef(
        env->FindClass("android/app/ActivityThread"));
    static jmethodID currentAppMid  = env->GetStaticMethodID(
        activityThreadCls, "currentApplication", "()Landroid/app/Application;");

    jobject app = env->CallStaticObjectMethod(activityThreadCls, currentAppMid);
    if (app) {
        ALOGI("JNI_OnLoad: app ready, initializing immediately");
        init_turnip_driver(env, app);
        env->DeleteLocalRef(app);
        return;
    }

    // ─── Fallback poll thread — kept lightweight ───
    std::thread([vm]() {
        JNIEnv* t_env = nullptr;
        vm->AttachCurrentThread(&t_env, nullptr);

        jclass     atCls = t_env->FindClass("android/app/ActivityThread");
        jmethodID  caMid = t_env->GetStaticMethodID(atCls, "currentApplication",
                                                      "()Landroid/app/Application;");
        jobject t_app    = nullptr;
		
        for (int i = 0; i < 20 && !t_app; ++i) {
            t_app = t_env->CallStaticObjectMethod(atCls, caMid);
            if (!t_app) std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

        if (t_app) init_turnip_driver(t_env, t_app);

        t_env->DeleteLocalRef(atCls);
        if (t_app) t_env->DeleteLocalRef(t_app);
        vm->DetachCurrentThread();
    }).detach();
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_java_vm = vm;
    perform_init(vm);
    return JNI_VERSION_1_6;
}
