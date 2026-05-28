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
#include <shadowhook.h>
#include <atomic>
#include <stdatomic.h>
#include <pthread.h>
#include <vector>
#include <mutex>
#include <bytehook.h>
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

void *adrenotools_open_libvulkan(int dlopenFlags, int featureFlags, const char *tmpLibDir, const char *hookLibDir, const char *customDriverDir, const char *customDriverName, const char *fileRedirectDir, void **userMappingHandle) {
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

#ifdef DLOPEN_HOOK
static void* (*real_dlopen)(const char*, int) = nullptr;
static thread_local bool g_in_hook = false;
#endif

static PFN_vkVoidFunction hooked_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return g_turnip_gipa(instance, pName);
}

static PFN_vkVoidFunction hooked_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return g_turnip_gdpa(device, pName);
}
#ifdef DLOPEN_HOOK
static bool safe_contains(const char* haystack, const char* needle) {
    if (!haystack || !needle || !*needle) 
        return false;
    
    size_t count = 0;
    while (haystack[count] != '\0' && count < MAX_FILENAME_SCAN) {
        size_t i = 0;
        while (haystack[count + i] == needle[i] && needle[i] != '\0' && i < 64) {
            i++;
        }
        if (needle[i] == '\0') {
            return true;
        }
        count++;
    }
    return false;
}

static void* hooked_dlopen(const char* filename, int flags) {
    if (g_in_hook) {
        return real_dlopen(filename, flags);
    }

    if (!filename) {
        return real_dlopen(filename, flags);
    }

    g_in_hook = true;
	
    void* result = NULL;
    bool is_relevant = safe_contains(filename, "vulkan") ||
                       safe_contains(filename, "adreno");

    if (is_relevant && g_turnip_handle) {
        if (safe_contains(filename, "vulkan_adreno") ||
            safe_contains(filename, "libvulkan.so")) {
            
            result = g_turnip_handle;
        }
    }
	
    if (result == NULL) {
        result = real_dlopen(filename, flags);
    }

    g_in_hook = false;
    return result;
}
#endif
static char* get_native_library_dir(JNIEnv* env, jobject context) {
    char* native_libdir = nullptr;

    if (context != nullptr) {
        jclass contextClass = env->FindClass("android/content/Context");
        jmethodID getAppInfo = env->GetMethodID(contextClass, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
        jobject appInfo = env->CallObjectMethod(context, getAppInfo);

        jclass appInfoClass = env->GetObjectClass(appInfo);
        jfieldID fieldId = env->GetFieldID(appInfoClass, "nativeLibraryDir", "Ljava/lang/String;");
        jstring jPath = (jstring)env->GetObjectField(appInfo, fieldId);

        if (jPath) {
            const char* path_chars = env->GetStringUTFChars(jPath, nullptr);
            if (path_chars) {
                native_libdir = strdup(path_chars);
                env->ReleaseStringUTFChars(jPath, path_chars);
            }
			env->DeleteLocalRef(jPath);
        }

        env->DeleteLocalRef(contextClass);
        env->DeleteLocalRef(appInfo);
        env->DeleteLocalRef(appInfoClass);
    }

    return native_libdir;
}

void applyTurnipOptimizations() {
    std::string gpuName = "";
	
    std::ifstream kgslFile("/sys/class/kgsl/kgsl-3d0/gpu_model");
    if (kgslFile.is_open()) {
        std::getline(kgslFile, gpuName);
        kgslFile.close();
	}
	
    if (gpuName.empty()) {
        std::ifstream dtFile("/proc/device-tree/model");
        if (dtFile.is_open()) {
            std::getline(dtFile, gpuName);
            dtFile.close();
		}
    }
	
    if (gpuName.empty()) {
        std::ifstream devTreeFile("/sys/firmware/devicetree/base/model");
        if (devTreeFile.is_open()) {
            std::getline(devTreeFile, gpuName);
            devTreeFile.close();
        }
    }
	
    if (gpuName.empty()) {
        #ifdef OVERCLOCK
            setenv("TU_DEBUG", "noconform,noflushall,dynamic,unaligned_store,deck_emu,forcecb", 1);
        #else
            setenv("TU_DEBUG", "noconform,hiprio,noflushall,dynamic,deck_emu,forcecb", 1);
        #endif
		return;
    }
	
    char firstDigit = (!gpuName.empty()) ? gpuName[0] : '0';

    bool isAdreno8 = (firstDigit == '8');
    bool isAdreno7 = (firstDigit == '7');
    
    if (isAdreno8) {
		ALOGI("Applying Flags For Adreno 8 (forcing Sssmem to prevent page faults)");
		setenv("tu_override_uncached_as_cache_coherent", "true", 1);
#ifdef OVERCLOCK
		setenv("TU_DEBUG", "noconform,sysmem,noflushall,hiprio,dynamic,unaligned_store,deck_emu", 1);
#else
		setenv("TU_DEBUG", "noconform,sysmem,hiprio,noflushall,dynamic,deck_emu", 1);
#endif
	} else if (isAdreno7) {
		ALOGI("Applying Flags For Adreno 7 (Uses Autotuner)");
		setenv("tu_override_uncached_as_cache_coherent", "true", 1);
#ifdef OVERCLOCK
		setenv("TU_DEBUG", "noconform,noflushall,hiprio,dynamic,unaligned_store,deck_emu", 1);
#else
		setenv("TU_DEBUG", "noconform,hiprio,noflushall,dynamic,deck_emu", 1);
#endif
	} else {
		ALOGI("Applying Flags For Adreno 6 (Using Sysmem for Stability)");
#ifdef OVERCLOCK
		setenv("TU_DEBUG", "noconform,sysmem,hiprio,noflushall,dynamic,unaligned_store,deck_emu", 1);
#else
		setenv("TU_DEBUG", "noconform,sysmem,hiprio,noflushall,dynamic,deck_emu", 1);
#endif
	}
}
#ifdef DLOPEN_HOOK
bool my_caller_filter(const char *caller_path_name, void *arg) {
    if (!caller_path_name) return false;
	
    return strstr(caller_path_name, "libvulkan.so") ||
           strstr(caller_path_name, "libroblox.so") ||
           strstr(caller_path_name, "libUE4.so")      ||
           strstr(caller_path_name, "libUnreal.so")   ||
           strstr(caller_path_name, "libunity.so")    ||
           strstr(caller_path_name, "libmain.so");
}
#endif
static void* g_shadow_stub_gipa = nullptr;
static void* g_shadow_stub_gdpa = nullptr;
#ifdef DLOPEN_HOOK
static bytehook_stub_t g_bytehook_stub_dlopen = nullptr;
#endif

static void init_turnip_driver(JNIEnv* env, jobject context) {
    if (g_turnip_handle != nullptr) {
        ALOGI("init_turnip_driver: already initialized, refreshing hooks");
#ifdef DLOPEN_HOOK
         if (g_bytehook_stub_dlopen) {
            bytehook_unhook(g_bytehook_stub_dlopen);
            g_bytehook_stub_dlopen = nullptr;
        }
#endif
        if (g_shadow_stub_gipa) {
            shadowhook_unhook(g_shadow_stub_gipa);
            g_shadow_stub_gipa = nullptr;
        }
		
        if (g_shadow_stub_gdpa) {
            shadowhook_unhook(g_shadow_stub_gdpa);
            g_shadow_stub_gdpa = nullptr;
        }
		
#ifdef DLOPEN_HOOK
        g_bytehook_stub_dlopen = bytehook_hook_partial(my_caller_filter, NULL, NULL, "dlopen", (void*)hooked_dlopen, NULL, NULL);
#endif
        g_shadow_stub_gipa = shadowhook_hook_sym_name("libvulkan.so", "vkGetInstanceProcAddr", (void*)hooked_vkGetInstanceProcAddr, NULL);
        g_shadow_stub_gdpa = shadowhook_hook_sym_name("libvulkan.so", "vkGetDeviceProcAddr", (void*)hooked_vkGetDeviceProcAddr, NULL);


        return;
    }

    char* native_lib_dir = get_native_library_dir(env, context);

    char fixed_dir[512];
	int ret = 0;
    snprintf(fixed_dir, sizeof(fixed_dir), "%s/", native_lib_dir);
    __android_log_print(ANDROID_LOG_ERROR, "AdrenoToolsPatch", "Native Lib Dir: %s", fixed_dir);
    
    setenv("MESA_LIBGL_DRIVERS_PATH", fixed_dir, 1);
    
    jclass contextClass = env->GetObjectClass(context);
    jmethodID getCacheDir = env->GetMethodID(contextClass, "getCacheDir", "()Ljava/io/File;");
    jobject cacheFileObj = env->CallObjectMethod(context, getCacheDir);
    jclass fileClass = env->GetObjectClass(cacheFileObj);
    jmethodID getAbsolutePath = env->GetMethodID(fileClass, "getAbsolutePath", "()Ljava/lang/String;");
    jstring jPath = (jstring)env->CallObjectMethod(cacheFileObj, getAbsolutePath);

    const char* base_cache_path = env->GetStringUTFChars(jPath, nullptr);

    char tmpdir[512];
    snprintf(tmpdir, sizeof(tmpdir), "%s/turnip_tmp/", base_cache_path);
    mkdir(tmpdir, 0775);

    char cache_dir[512];
    snprintf(cache_dir, sizeof(cache_dir), "%s/turnip_shader_cache/", base_cache_path);
    mkdir(cache_dir, 0775);
    
    setenv("MESA_SHADER_CACHE_DIR", cache_dir, 1);

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
        ALOGE("Failed to load Turnip via adrenotools");
        goto cleanup;
    }

    g_turnip_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_turnip_handle, "vkGetInstanceProcAddr");
    if (!g_turnip_gipa) {
        ALOGE("Failed to get vkGetInstanceProcAddr from Turnip");
		dlclose(g_turnip_handle);
        g_turnip_handle = nullptr;
        goto cleanup;
    }

    g_turnip_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(g_turnip_handle, "vkGetDeviceProcAddr");
    if (!g_turnip_gdpa) {
        ALOGE("Failed to get vkGetDeviceProcAddr from Turnip");
		dlclose(g_turnip_handle);
        g_turnip_handle = nullptr;
        goto cleanup;
    }

    ALOGI("Turnip loaded, setting up hooks...");
#ifdef DLOPEN_HOOK
    ALOGI("Installing dlopen hooks for Vulkan redirection...");

    g_bytehook_stub_dlopen = bytehook_hook_partial(my_caller_filter, NULL, NULL, "dlopen", (void*)hooked_dlopen, NULL, NULL);
#endif

    ALOGI("Installing GIPA And GPIA hooks");
    
    g_shadow_stub_gipa = shadowhook_hook_sym_name("libvulkan.so", "vkGetInstanceProcAddr", (void*)hooked_vkGetInstanceProcAddr, NULL);
    g_shadow_stub_gdpa = shadowhook_hook_sym_name("libvulkan.so", "vkGetDeviceProcAddr", (void*)hooked_vkGetDeviceProcAddr, NULL);
	
	#ifdef OVERCLOCK
	    ALOGI("Enabling Overclock make sure you have a fan cooler");
	    adrenotools_set_turbo(true);
	    ret = setpriority(PRIO_PROCESS, 0, -20);
        if (ret != 0) {
            ALOGI("setpriority to -20 failed (no root), trying -10");
            setpriority(PRIO_PROCESS, 0, -10); // usually allowed without root
	    }
	#else
	    ALOGI("using stranded mode");
	    adrenotools_set_turbo(false);
	#endif

    ALOGI("Turnip hooks installed successfully");

cleanup:
    env->ReleaseStringUTFChars(jPath, base_cache_path);
    env->DeleteLocalRef(contextClass);
    env->DeleteLocalRef(cacheFileObj);
    env->DeleteLocalRef(fileClass);
    free(native_lib_dir);
}

__attribute__((constructor))
static void global_atomic_init() {
    setenv("MESA_VULKAN_ICD_SELECT", "turnip", 1);
    setenv("MESA_VK_IGNORE_CONFORMANCE_WARNING", "1", 1);
	setenv("MESA_VK_IGNORE_CONFORMANCE_ERRORS", "1", 1);
    setenv("MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE", "1", 1);
	setenv("MESA_SHADER_CACHE_DISABLE", "false", 1);
    setenv("MESA_SHADER_CACHE_MAX_SIZE", "4G", 1);
	unsetenv("MESA_DISK_CACHE_SINGLE_FILE");
	unsetenv("MESA_DISK_CACHE_READ_ONLY");
	
    setenv("GALLIUM_PRINT_OPTIONS", "0", 1);
    setenv("MESA_DEBUG", "silent", 1);
	setenv("MESA_NO_ERROR", "1", 1);
	setenv("MESA_GLTHREAD", "true", 1);
	setenv("vblank_mode", "0", 1);
	setenv("TU_ROBUST_BUFFER_ACCESS", "0", 1);
	
	setenv("TU_GMEM_ALLOW_OVERLAP", "1", 1);
	setenv("TU_RENDERPASS_CACHE", "1", 1);
	setenv("MESA_TEXTURE_MAX_ANISOTROPY", "4", 1);

	#ifdef OVERCLOCK
	    setenv("MESA_VK_WSI_PRESENT_MODE", "immediate", 1);
	#else
	    setenv("MESA_VK_WSI_PRESENT_MODE", "mailbox", 1);
	#endif
    
    setenv("UNITY_DISABLE_GRAPHICS_DRIVER_CHECK", "1", 1);
    setenv("UNITY_VULKAN_ENABLE_VALIDATION_LAYERS", "0", 1);
	setenv("UNITY_GFX_DEVICE_API", "vulkan", 1);

	long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    long long total_ram_bytes = (long long)pages * page_size;
    
    long heap_size_mb = (total_ram_bytes / (1024 * 1024)) / 2;
    
    if (heap_size_mb < 256) {
        heap_size_mb = 256;
    }

    char heap_str[16];
    snprintf(heap_str, sizeof(heap_str), "%ld", heap_size_mb);
    
    setenv("TU_OVERRIDE_HEAP_SIZE", heap_str, 1);
    ALOGI("Set TU_OVERRIDE_HEAP_SIZE to %s MB based on system RAM", heap_str);

	char sdk_str[8] = {};
    __system_property_get("ro.build.version.sdk", sdk_str);
    int sdk = atoi(sdk_str);

    ALOGI("Android SDK: %d", sdk);

    unsetenv("MESA_VK_VERSION_OVERRIDE");
	unsetenv("FD_DEV_FEATURES");

	char oneui_str[PROP_VALUE_MAX] = {0};
    bool is_affected_oneui = false;
    
    if (__system_property_get("ro.build.version.oneui", oneui_str) > 0) {
        int raw_version = atoi(oneui_str);
		
        if (raw_version >= 60000) {
            is_affected_oneui = true;
            int major = raw_version / 10000;
            int minor = (raw_version % 10000) / 100;
            ALOGI("Targeted One UI version detected: %d.%d", major, minor);
        }
	}

	if (is_affected_oneui && sdk >= 30) {
        setenv("FD_DEV_FEATURES", "enable_tp_ubwc_flag_hint=1", 1);
        ALOGI("One UI 6.0+: UBWC flag hint enabled to prevent texture glitches");
    }

	applyTurnipOptimizations();

#ifdef DLOPEN_HOOK
	real_dlopen = reinterpret_cast<decltype(real_dlopen)>(dlsym(RTLD_DEFAULT, "dlopen"));
#endif

    shadowhook_init(SHADOWHOOK_MODE_SHARED, false);
	bytehook_init(BYTEHOOK_MODE_MANUAL, false);
}

void perform_init(JavaVM* vm) {
	ALOGI("JNI_OnLoad: started");
	
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    }

    static jclass activityThreadCls = (jclass)env->NewGlobalRef(env->FindClass("android/app/ActivityThread"));
    static jmethodID currentAppMid = env->GetStaticMethodID(activityThreadCls, "currentApplication", "()Landroid/app/Application;");

    jobject app = env->CallStaticObjectMethod(activityThreadCls, currentAppMid);

    if (app) {
        ALOGI("JNI_OnLoad: Initializing Turnip immediately");
        init_turnip_driver(env, app);
    } else {
        std::thread([vm]() {
            JNIEnv* t_env = nullptr;
            vm->AttachCurrentThread(&t_env, nullptr);

            jclass atCls = t_env->FindClass("android/app/ActivityThread");
            jmethodID caMid = t_env->GetStaticMethodID(atCls, "currentApplication", "()Landroid/app/Application;");

            jobject t_app = nullptr;
            for (int i = 0; i < 10 && !t_app; ++i) {
               t_app = t_env->CallStaticObjectMethod(atCls, caMid);
               if (!t_app) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (t_app) init_turnip_driver(t_env, t_app);
               t_env->DeleteLocalRef(atCls);
               if (t_app) t_env->DeleteLocalRef(t_app);

           vm->DetachCurrentThread();
        }).detach();
    }
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    g_java_vm = vm;
    perform_init(vm);
    return JNI_VERSION_1_6;
}
