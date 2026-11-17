Forked Verison of libanderotools design to run on native Android apps

for some changes you neee to uninstall the app fully instead of updating

credits to Pipetto-crypto for fixes and patches

Credits To gamenative performance/WinNative source code for loading and some fixes

Credits To MrPurple for env loader (didn't even use it btw)

### Adreno Tools 
A library for applying rootless Adreno GPU driver modifications/replacements. Currently supports loading custom GPU drivers such as [turnip](https://docs.mesa3d.org/android.html#building-using-the-android-ndk),  enabling BCn textures and redirecting file operations to allow accessing shader dumps and modification of the [driver config file](https://gist.github.com/bylaws/04130932e2634d1c6a2a9729e3940d60) without root.

### Testing

Tested on [Rustudio](https://github.com/TouseefX/rustudio)

FPS went from 20 fps to 120 fps (refresh rate limit) On N20 and S20 Ultra 5G Snapdragon
and this was a desktop renderer, and it beated the stock driver

hadn't been tested for Mail GPUs yet

#### Documentation
API is documented in the `include/adrenotools` headers.

#### Support and Requirements
Android 12+
Arm64

Vulkan 1.3+ and driver must match your vulkan verison unless you're in the newer Android (14-16) or your gpu can support it

Adero 620+

8GB of RAM (Recommended) but not required only for video ram

Please create an issue if support for anything else is desired.

Recommend [Native Android Drivers](https://github.com/TGMGT/Banners-Turnip/releases)

And [Mr Purple Driver if your goal is Performance](https://github.com/MrPurple666/purple-turnip/releases)

And Freedeno Drivers

### FAQ

#### Is there an example project?

There is a simple bare-bones project [AdrenoToolsTest](https://github.com/auto21011/AdrenoToolsTest) demonstrating how to get libadrenotools working.

#### How do I use this to update the drivers on my phone? Where's the apk?

You don't. This library is **not** for installing into Android and is **not** for end users.
This library is aimed at other developers.

Each individual app must explicitly make use of libadrenotools in order to load custom drivers into an app / game.

# Patching a APK

#### A. For Any App You Have Source For (No Root, No Shizuku, Swappable Drivers)
You don't need Shizuku or APK rebuilds to swap drivers:
- Keep `libadrenotools.so` + the `lib*_hook.so` files in `lib/arm64-v8a`, and add an in-app **"Import Driver (.zip)"** picker (the same pattern emulators like Eden/Winlator/Vita3K use).
- Unzip any AdrenoTools driver package (MrPurple, Banners, K11MCH1, PanVK, etc.) into `filesDir/drivers/<name>/`, `chmod 0700`, read its `meta.json` for `libraryName`, and pass that folder as `customDriverDir` to `adrenotools_open_libvulkan()`.
- Users can install/switch drivers on the fly with zero root.

#### B. For Third-Party Apps on Non-Root + Shizuku
Since Android blocks Shizuku from injecting into untouched release apps, you have two choices:
1. **A Shizuku One-Tap Injector App / Script (Most reliable — [KaosGFX](https://github.com/ulolol/Ka0SGFX) style) [1](https://github.com/ulolol/Ka0SGFX):**
   - Picks any installed app, injects a tiny `android:appComponentFactory` bootstrap DEX + `libadrenotools-native` (works even on pure `NativeActivity` or Unity/Unreal apps that have no custom `Application` class), sets `extractNativeLibs="true"`, re-signs, and installs via Shizuku.
   - To let users swap drivers **without re-patching every time**, the injected bootstrap can pull the selected driver `.so` from a shared folder or DocumentProvider into the app's own `codeCacheDir` on launch before calling `adrenotools_open_libvulkan()`.
   - Works on offline games, indie ports, benchmarks, and unprotected apps. **Won't** work on apps with strict anti-tamper/signature checks (like the official Roblox client) [1](https://github.com/TouseefX/libadrenotools-native).
2. **A Virtual-Space Launcher APK (Zero APK patching, no root):**
   - Wrap a userspace container engine (like a [BlackBox](https://github.com/ALEX5402/NewBlackbox) fork) [1](https://github.com/ALEX5402/NewBlackbox), load `libadrenotools-native` inside your host process first, and launch untouched installed APKs inside it.
   - Keeps original APK signatures intact, but runs with separate app data, has shaky Android 15–16 compatibility [4](https://github.com/ZENINXOP/BlackBox), and still gets blocked by anti-cheat (like Roblox/Hyperion).

#### C. For Untouched Third-Party Apps Without Virtual Containers
- Requires **root** (KernelSU / Magisk / APatch) — either via a Zygisk/LSPosed module or swapping the driver in Android's `GraphicsEnvironment` carrier namespace (`ro.gfx.driver.0`), which leaves the APK and its signature 100% original [1](https://github.com/ulolol/Ka0SGFX).
