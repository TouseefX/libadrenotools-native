Forked Version of libadrenotools designed to run on native Android apps

For some changes you need to uninstall the app fully instead of updating.

Credits to Pipetto-crypto for fixes and patches

Credits to gamenative performance/WinNative source code for loading and some fixes

Credits to MrPurple for env loader (didn't even use it btw)

## Adreno Tools

A library for applying rootless Adreno GPU driver modifications/replacements. Currently supports loading custom GPU drivers such as [turnip](https://docs.mesa3d.org/android.html#building-using-the-android-ndk), enabling BCn textures and redirecting file operations to allow accessing shader dumps and modification of the [driver config file](https://gist.github.com/bylaws/04130932e2634d1c6a2a9729e3940d60) without root.

### Hybrid C++/Rust Architecture

libadrenotools uses a hybrid C++/Rust design where crash-prone memory management has been moved to a memory-safe Rust layer via the `cxx` crate (`#[cxx::bridge]`):

| Component | Language | Responsibility |
|-----------|----------|----------------|
| ShadowHook, JNI, android linker namespaces, `adrenotools_open_libvulkan` | C++ | Driver loading, hooking, Vulkan interop |
| GPU mapping management | Rust (`GpuMappingManager`) | Thread-safe pending mapping, no data races |
| GPU mailbox / doorbell (Mali G52) | Rust | `mailbox_open/read/write/close/ring_doorbell` |
| Thread-safe global state | Rust | Active dispatch, hook system callers, caller cache |

The C++ side calls Rust through a compile-time-checked cxx FFI boundary. `hook_gsl_memory_alloc_pure_64` delegates to `GpuMappingManager::try_consume_pending()`, eliminating the data races on `hook_params->nextGpuMapping` that previously caused intermittent GPU crashes and full device reboots.

### Supported Features

- **Turnip (Adreno)** — Custom Adreno driver loading with per-generation optimizations
- **PanVK (ARM Mali G52)** — Mali GPU detection, PanVK driver loading, mailbox/doorbell register access
- **Hyper OS (Xiaomi)** — Vulkan path discovery under `/system/vendor`, `/vendor`, `/odm`, `/product`
- **GPU mailbox** — ARM Mali G52 doorbell/STATUS/IRQ_CLEAR register access
- **Turbo control** — Opt-in only via `adrenotools_set_turbo()` (can crash KGSL on some kernels)

### Turnip compatibility

For detected Adreno 6xx GPUs, the automatic loader defaults `TU_DEBUG` to
`noconform,noubwc,nolrz`. Disabling UBWC is an experimental mitigation for a
reported Turnip NoC fault on a related Galaxy S20 FE; disabling LRZ is intended
to avoid rendering issues reported in Rustudio. Neither is verified for every
device or driver, and disabling these optimizations may reduce performance.

An existing `TU_DEBUG` is respected, including an empty value. These settings
apply to Turnip and do not disable UBWC across the whole system. Other GPU
generations keep their existing defaults.

### Testing

Tested on [Rustudio](https://github.com/TouseefX/rustudio)

FPS went from 20 fps to 120 fps (refresh rate limit) on N20 and S20 Ultra 5G Snapdragon in the highest settings — this was a desktop renderer and it beat the stock driver.

Testing place: a Baseplate (yes, I'm not kidding).

### CI Build

GitHub Actions builds for `arm64-v8a` (Android NDK r29, API 31):
- `libadrenotools-arm64-v8a` — 3 `.so` shared libraries
- `libadrenotools-rs-staticlib` — `libadrenotools_rs.a` Rust static library

### Documentation

API is documented in the `include/adrenotools` headers.

### Support and Requirements

- Android 12+
- Arm64
- Vulkan 1.3+ and driver must match your Vulkan version (unless on Android 14–16 or your GPU supports it)
- Adreno 620+
- 8GB of RAM (recommended, but not required — only for video RAM)

Please create an issue if support for anything else is desired.

#### Recommended Drivers

- [nihui Mesa Turnip Android Driver](https://github.com/nihui/mesa-turnip-android-driver)
- [JimVulkan Mali PanVK](https://github.com/JimVulkan/mali-panvk)
- Freedeno Drivers

### FAQ

#### Is there an example project?

There is a simple bare-bones project [AdrenoToolsTest](https://github.com/auto21011/AdrenoToolsTest) demonstrating how to get libadrenotools working.

#### How do I use this to update the drivers on my phone? Where's the APK?

You don't. This library is **not** for installing into Android and is **not** for end users. This library is aimed at other developers.

Each individual app must explicitly make use of libadrenotools in order to load custom drivers into an app/game.

# Patching an APK

### A. For Any App You Have Source For (No Root, No Shizuku, Swappable Drivers)

You don't need Shizuku or APK rebuilds to swap drivers:

- Keep `libadrenotools.so` + the `lib*_hook.so` files in `lib/arm64-v8a`, and add an in-app **"Import Driver (.zip)"** picker (the same pattern emulators like Eden/Winlator/Vita3K use).
- Unzip any AdrenoTools driver package (MrPurple, Banners, K11MCH1, PanVK, etc.) into `filesDir/drivers/<name>/`, `chmod 0700`, read its `meta.json` for `libraryName`, and pass that folder as `customDriverDir` to `adrenotools_open_libvulkan()`.
- Users can install/switch drivers on the fly with zero root.

### B. For Third-Party Apps on Non-Root + Shizuku

Since Android blocks Shizuku from injecting into untouched release apps, you have two choices:

1. **A Shizuku One-Tap Injector App / Script (Most reliable — [KaosGFX](https://github.com/ulolol/Ka0SGFX) style):**
   - Picks any installed app, injects a tiny `android:appComponentFactory` bootstrap DEX + `libadrenotools-native` (works even on pure `NativeActivity` or Unity/Unreal apps that have no custom `Application` class), sets `extractNativeLibs="true"`, re-signs, and installs via Shizuku.
   - To let users swap drivers without re-patching every time, the injected bootstrap can pull the selected driver `.so` from a shared folder or DocumentProvider into the app's own `codeCacheDir` on launch before calling `adrenotools_open_libvulkan()`.
   - Works on offline games, indie ports, benchmarks, and unprotected apps. Won't work on apps with strict anti-tamper/signature checks (like the official Roblox client).

2. **A Virtual-Space Launcher APK (Zero APK patching, no root):**
   - Wrap a userspace container engine (like a [BlackBox](https://github.com/ALEX5402/NewBlackbox) fork), load `libadrenotools-native` inside your host process first, and launch untouched installed APKs inside it.
   - Keeps original APK signatures intact, but runs with separate app data, has shaky Android 15–16 compatibility, and still gets blocked by anti-cheat (like Roblox/Hyperion).

### C. For Untouched Third-Party Apps Without Virtual Containers

Requires **root** (KernelSU / Magisk / APatch) — either via a Zygisk/LSPosed module or swapping the driver in Android's `GraphicsEnvironment` carrier namespace (`ro.gfx.driver.0`), which leaves the APK and its signature 100% original.
