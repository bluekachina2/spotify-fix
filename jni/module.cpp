// AAudio capture-policy fix for Spotify (Zygisk, API v4, arm64).
//
// Spotify never imports AAudioStreamBuilder_setAllowedCapturePolicy through its PLT;
// it resolves it at runtime with dlsym() (Oboe-style). So we:
//   A) PLT-hook dlsym() in Spotify's own libs and hand back a wrapper for the target symbol.
//      New libs are caught by also hooking dlopen()/android_dlopen_ext() and re-scanning maps
//      right after each load (so there is no timing race with System.loadLibrary).
//   B) (optional, -DUSE_INLINE_HOOK) inline-hook the function inside libaaudio.so once it is loaded.
// Both force the policy to AAUDIO_ALLOW_CAPTURE_BY_ALL.

#include <android/log.h>
#include <dlfcn.h>
#include <sys/sysmacros.h>
#include <sys/types.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <utility>

#include "zygisk.hpp"

#define TAG "AAudioCaptureFix"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

static constexpr const char *kTarget = "AAudioStreamBuilder_setAllowedCapturePolicy";
static constexpr int32_t kAllowAll = 1;  // AAUDIO_ALLOW_CAPTURE_BY_ALL (2 = SYSTEM, 3 = NONE)

typedef void (*setter_t)(void *builder, int32_t policy);

static zygisk::Api *g_api = nullptr;
static std::mutex g_mu;
static std::set<std::pair<dev_t, ino_t>> g_seen_app, g_seen_loader;

static void *(*old_dlsym)(void *, const char *) = nullptr;
static void *(*old_dlopen)(const char *, int) = nullptr;
static void *(*old_dlopen_ext)(const char *, int, const void *) = nullptr;
static setter_t g_real_setter = nullptr;

static void scan_and_hook();

// ---------------------------------------------------------------- wrapper ---
static void my_setter(void *builder, int32_t policy) {
    LOGI("setAllowedCapturePolicy(%d) -> forcing %d", policy, kAllowAll);
    if (g_real_setter) g_real_setter(builder, kAllowAll);
}

static void *my_dlsym(void *handle, const char *name) {
    void *r = old_dlsym(handle, name);
    if (name && strncmp(name, "AAudio", 6) == 0) LOGI("dlsym(%s) -> %p", name, r);  // shows what Spotify really uses
    if (r && name && strcmp(name, kTarget) == 0) {
        g_real_setter = reinterpret_cast<setter_t>(r);
        LOGI("returning wrapper for %s (real=%p)", kTarget, r);
        return reinterpret_cast<void *>(&my_setter);
    }
    return r;
}

// --------------------------------------------------- catch libs loaded later ---
static void *my_dlopen(const char *path, int flags) {
    void *r = old_dlopen(path, flags);
    if (r) scan_and_hook();
    return r;
}
static void *my_dlopen_ext(const char *path, int flags, const void *ext) {
    void *r = old_dlopen_ext(path, flags, ext);
    if (r) scan_and_hook();
    return r;
}

// ------------------------------------------------------ optional inline hook ---
#ifdef USE_INLINE_HOOK
extern "C" void A64HookFunction(void *symbol, void *replace, void **result);  // And64InlineHook
static setter_t inline_orig = nullptr;
static bool inline_done = false;
static void my_setter_inline(void *builder, int32_t policy) {
    LOGI("[inline] setAllowedCapturePolicy(%d) -> forcing %d", policy, kAllowAll);
    inline_orig(builder, kAllowAll);
}
static void try_inline_hook() {
    if (inline_done) return;
    void *h = dlopen("libaaudio.so", RTLD_NOLOAD);  // only if already loaded
    if (!h) return;
    void *fn = dlsym(h, kTarget);  // resolve by name; 0x74b0 is build-specific, never hardcode it
    if (!fn) return;
    A64HookFunction(fn, reinterpret_cast<void *>(&my_setter_inline), reinterpret_cast<void **>(&inline_orig));
    inline_done = true;
    LOGI("inline hook installed at %p", fn);
}
#else
static void try_inline_hook() {}
#endif

// ------------------------------------------------------------ maps scanning ---
static void scan_and_hook() {
    std::lock_guard<std::mutex> lock(g_mu);
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) { LOGI("cannot open /proc/self/maps"); return; }
    char line[1024];
    int added = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e, off, ino;
        unsigned maj, min;
        char perms[8];
        int pos = 0;
        if (sscanf(line, "%lx-%lx %7s %lx %x:%x %lu %n", &s, &e, perms, &off, &maj, &min, &ino, &pos) < 7) continue;
        if (!ino || pos <= 0) continue;
        char *path = line + pos;
        size_t n = strlen(path);
        while (n && (path[n - 1] == '\n' || path[n - 1] == ' ')) path[--n] = 0;
        if (path[0] != '/' || strstr(path, "aaudiocapturefix")) continue;

        auto key = std::make_pair(makedev(maj, min), static_cast<ino_t>(ino));
        bool is_app = strstr(path, "/data/app/") != nullptr;           // Spotify's own libs / apk
        bool is_loader = strstr(path, "libnativeloader.so") != nullptr; // calls android_dlopen_ext for System.loadLibrary

        if (is_app && g_seen_app.insert(key).second) {
            g_api->pltHookRegister(key.first, key.second, "dlsym", (void *)my_dlsym, (void **)&old_dlsym);
            g_api->pltHookRegister(key.first, key.second, "dlopen", (void *)my_dlopen, (void **)&old_dlopen);
            g_api->pltHookRegister(key.first, key.second, "android_dlopen_ext", (void *)my_dlopen_ext, (void **)&old_dlopen_ext);
            LOGI("hooking app lib %s", path);
            added++;
        } else if (is_loader && g_seen_loader.insert(key).second) {
            g_api->pltHookRegister(key.first, key.second, "android_dlopen_ext", (void *)my_dlopen_ext, (void **)&old_dlopen_ext);
            g_api->pltHookRegister(key.first, key.second, "dlopen", (void *)my_dlopen, (void **)&old_dlopen);
            LOGI("hooking loader %s", path);
            added++;
        }
    }
    fclose(f);
    if (added) LOGI("PLT commit (%d new files): %s", added, g_api->pltHookCommit() ? "ok" : "FAILED");
    try_inline_hook();
}

// ------------------------------------------------------------------ module ---
class Module : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override { api_ = api; env_ = env; }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        const char *name = args->nice_name ? env_->GetStringUTFChars(args->nice_name, nullptr) : nullptr;
        target_ = name && strncmp(name, "com.spotify.music", 17) == 0 && (name[17] == 0 || name[17] == ':');
        if (name) env_->ReleaseStringUTFChars(args->nice_name, name);
        if (!target_) api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);  // unload from other apps
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!target_) return;
        g_api = api_;
        LOGI("loaded into Spotify");
        scan_and_hook();
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool target_ = false;
};

REGISTER_ZYGISK_MODULE(Module)
