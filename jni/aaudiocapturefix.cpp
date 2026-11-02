// aaudiocapturefix v0.3 - Zygisk module (arm64-v8a)
//
// Experiment: make Spotify's AAudio output streams use the normal shared,
// power-saving (deep buffer) path instead of whatever it asks for, to see if
// that makes the stream visible to system audio capture (screen recording).
//
// How it works (no PLT hooking, no dlopen/dlsym wrappers):
//   1. In preAppSpecialize, for com.spotify.music only, load libaaudio.so.
//   2. Walk its in-memory ELF dynamic symbol table and rewrite st_value of
//      three exported functions so they point at our hooks. Every later
//      dlsym()/relocation lookup of those names (Oboe-style dlsym or normal
//      linking) then resolves to our code. We keep the original addresses.
//
// Hooked: AAudioStreamBuilder_setPerformanceMode / _setSharingMode / _openStream
// Not touched: capture policy (your dumps show Spotify does not set one).
//
// Runtime setting (no rebuild needed): put a number in
//   /data/adb/modules/aaudiocapturefix/mode
//     11 = POWER_SAVING (default)   10 = NONE   12 = LOW_LATENCY
//      0 = observe only, change nothing (just log what Spotify requests)
//
// Log tag: AAudioCaptureFix   (logcat -d | grep AAudioCaptureFix)

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "zygisk.hpp"

#define TAG "AAudioCaptureFix"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#ifndef AAUDIO_LIB_NAME
#define AAUDIO_LIB_NAME "libaaudio.so"
#endif

namespace {

constexpr const char *kTargetPackage = "com.spotify.music";
constexpr int32_t kPerfNone = 10;
constexpr int32_t kPerfPowerSaving = 11;
constexpr int32_t kSharingShared = 1;

int32_t g_forced_perf = kPerfPowerSaving;  // 0 = observe only

typedef void (*SetModeFn)(void *builder, int32_t mode);
typedef int32_t (*OpenFn)(void *builder, void **stream);
typedef int32_t (*GetFn)(void *stream);

SetModeFn g_real_set_perf = nullptr;
SetModeFn g_real_set_sharing = nullptr;
OpenFn g_real_open = nullptr;

GetFn g_get_perf = nullptr;
GetFn g_get_sharing = nullptr;
GetFn g_get_usage = nullptr;
GetFn g_get_content = nullptr;
GetFn g_get_capture = nullptr;
GetFn g_get_rate = nullptr;
GetFn g_get_device = nullptr;

int geti(GetFn f, void *s) { return f ? static_cast<int>(f(s)) : -1; }

// ---------------------------------------------------------------- hooks ----

void hook_set_perf(void *b, int32_t mode) {
  int32_t want = g_forced_perf ? g_forced_perf : mode;
  LOGI("setPerformanceMode(%d) -> %d", mode, want);
  g_real_set_perf(b, want);
}

void hook_set_sharing(void *b, int32_t mode) {
  int32_t want = g_forced_perf ? kSharingShared : mode;
  LOGI("setSharingMode(%d) -> %d", mode, want);
  g_real_set_sharing(b, want);
}

int32_t hook_open(void *b, void **stream) {
  if (g_forced_perf) {
    g_real_set_perf(b, g_forced_perf);
    g_real_set_sharing(b, kSharingShared);
  }
  int32_t rc = g_real_open(b, stream);
  if (rc != 0 && g_forced_perf) {
    // Never break playback: retry once with plain defaults.
    LOGE("openStream failed rc=%d with forced mode, retrying with mode NONE", rc);
    g_real_set_perf(b, kPerfNone);
    rc = g_real_open(b, stream);
  }
  if (rc == 0 && stream && *stream) {
    void *s = *stream;
    LOGI("openStream ok: perf=%d sharing=%d usage=%d content=%d capturePolicy=%d rate=%d device=%d",
         geti(g_get_perf, s), geti(g_get_sharing, s), geti(g_get_usage, s),
         geti(g_get_content, s), geti(g_get_capture, s), geti(g_get_rate, s),
         geti(g_get_device, s));
  } else {
    LOGI("openStream rc=%d", rc);
  }
  return rc;
}

// ------------------------------------------------------- ELF symbol patch --

struct Lib {
  uintptr_t bias = 0;
  Elf64_Sym *symtab = nullptr;
  const char *strtab = nullptr;
  size_t nsyms = 0;
};

size_t gnu_hash_nsyms(const uint32_t *h) {
  uint32_t nbuckets = h[0], symoffset = h[1], bloom_size = h[2];
  const uint64_t *bloom = reinterpret_cast<const uint64_t *>(h + 4);
  const uint32_t *buckets = reinterpret_cast<const uint32_t *>(bloom + bloom_size);
  const uint32_t *chain = buckets + nbuckets;
  uint32_t max_bucket = 0;
  for (uint32_t i = 0; i < nbuckets; i++)
    if (buckets[i] > max_bucket) max_bucket = buckets[i];
  if (max_bucket < symoffset) return symoffset;
  uint32_t idx = max_bucket;
  while (!(chain[idx - symoffset] & 1)) idx++;
  return static_cast<size_t>(idx) + 1;
}

// Parse the in-memory ELF of the library that contains `addr`.
bool parse_lib(void *addr, Lib &lib) {
  Dl_info info;
  if (!dladdr(addr, &info) || !info.dli_fbase) return false;
  const uintptr_t base = reinterpret_cast<uintptr_t>(info.dli_fbase);
  const auto *eh = reinterpret_cast<const Elf64_Ehdr *>(base);
  if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64)
    return false;

  const auto *ph = reinterpret_cast<const Elf64_Phdr *>(base + eh->e_phoff);
  const Elf64_Phdr *dynph = nullptr;
  bool have_load = false;
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type == PT_LOAD && !have_load) {
      lib.bias = base - (ph[i].p_vaddr - ph[i].p_offset);
      have_load = true;
    } else if (ph[i].p_type == PT_DYNAMIC) {
      dynph = &ph[i];
    }
  }
  if (!have_load || !dynph) return false;

  uintptr_t symtab = 0, strtab = 0, hash = 0, gnu_hash = 0;
  const auto *dyn = reinterpret_cast<const Elf64_Dyn *>(lib.bias + dynph->p_vaddr);
  auto fix = [&](uintptr_t v) { return v < lib.bias ? v + lib.bias : v; };
  for (; dyn->d_tag != DT_NULL; dyn++) {
    switch (dyn->d_tag) {
      case DT_SYMTAB: symtab = fix(dyn->d_un.d_ptr); break;
      case DT_STRTAB: strtab = fix(dyn->d_un.d_ptr); break;
      case DT_HASH: hash = fix(dyn->d_un.d_ptr); break;
      case DT_GNU_HASH: gnu_hash = fix(dyn->d_un.d_ptr); break;
      default: break;
    }
  }
  if (!symtab || !strtab) return false;

  size_t n = 0;
  if (gnu_hash) {
    n = gnu_hash_nsyms(reinterpret_cast<const uint32_t *>(gnu_hash));
  } else if (hash) {
    n = reinterpret_cast<const uint32_t *>(hash)[1];
  } else if (strtab > symtab) {
    n = (strtab - symtab) / sizeof(Elf64_Sym);
  }
  if (n < 2 || n > 200000) return false;

  lib.symtab = reinterpret_cast<Elf64_Sym *>(symtab);
  lib.strtab = reinterpret_cast<const char *>(strtab);
  lib.nsyms = n;
  return true;
}

// Current protection of the mapping that contains `addr` (from /proc/self/maps).
int prot_of(uintptr_t addr) {
  FILE *f = fopen("/proc/self/maps", "re");
  if (!f) return -1;
  char line[512];
  int prot = -1;
  while (fgets(line, sizeof line, f)) {
    unsigned long lo, hi;
    char perms[8] = {0};
    if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
    if (addr >= lo && addr < hi) {
      prot = 0;
      if (perms[0] == 'r') prot |= PROT_READ;
      if (perms[1] == 'w') prot |= PROT_WRITE;
      if (perms[2] == 'x') prot |= PROT_EXEC;
      break;
    }
  }
  fclose(f);
  return prot;
}

bool write_u64(uint64_t *where, uint64_t value) {
  const uintptr_t pagesz = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
  const uintptr_t addr = reinterpret_cast<uintptr_t>(where);
  const uintptr_t page = addr & ~(pagesz - 1);
  int prot = prot_of(addr);
  if (prot < 0) {
    LOGE("no mapping found for %p", static_cast<void *>(where));
    return false;
  }
  if (mprotect(reinterpret_cast<void *>(page), pagesz, prot | PROT_WRITE) != 0) {
    LOGE("mprotect(RW) failed: %s", strerror(errno));
    return false;
  }
  *where = value;
  if (mprotect(reinterpret_cast<void *>(page), pagesz, prot) != 0)
    LOGE("mprotect(restore) failed: %s", strerror(errno));
  return true;
}

// Point exported symbol `name` at `hook`; hand back the original address.
bool redirect(const Lib &lib, void *handle, const char *name, void *hook, void **real_out) {
  for (size_t i = 1; i < lib.nsyms; i++) {
    Elf64_Sym *s = &lib.symtab[i];
    if (s->st_shndx == SHN_UNDEF || s->st_value == 0) continue;
    if (strcmp(lib.strtab + s->st_name, name) != 0) continue;

    void *real = reinterpret_cast<void *>(lib.bias + s->st_value);
    if (dlsym(handle, name) != real) {
      LOGE("%s: table address %p does not match dlsym %p, skipping", name, real,
           dlsym(handle, name));
      return false;
    }
    *real_out = real;
    uint64_t newval = reinterpret_cast<uint64_t>(hook) - lib.bias;  // wraps on purpose
    if (!write_u64(reinterpret_cast<uint64_t *>(&s->st_value), newval)) return false;
    LOGI("redirected %s: %p -> %p", name, real, hook);
    return true;
  }
  LOGE("%s not found in dynamic symbol table", name);
  return false;
}

bool install_hooks(const char *libname) {
  void *h = dlopen(libname, RTLD_NOW);
  if (!h) {
    LOGE("dlopen(%s) failed: %s", libname, dlerror());
    return false;
  }
  void *probe = dlsym(h, "AAudioStreamBuilder_openStream");
  Lib lib;
  if (!probe || !parse_lib(probe, lib)) {
    LOGE("could not parse %s", libname);
    return false;
  }

  g_get_perf = reinterpret_cast<GetFn>(dlsym(h, "AAudioStream_getPerformanceMode"));
  g_get_sharing = reinterpret_cast<GetFn>(dlsym(h, "AAudioStream_getSharingMode"));
  g_get_usage = reinterpret_cast<GetFn>(dlsym(h, "AAudioStream_getUsage"));
  g_get_content = reinterpret_cast<GetFn>(dlsym(h, "AAudioStream_getContentType"));
  g_get_capture = reinterpret_cast<GetFn>(dlsym(h, "AAudioStream_getAllowedCapturePolicy"));
  g_get_rate = reinterpret_cast<GetFn>(dlsym(h, "AAudioStream_getSampleRate"));
  g_get_device = reinterpret_cast<GetFn>(dlsym(h, "AAudioStream_getDeviceId"));

  // openStream last: its hook needs the other two "real" pointers to exist.
  bool ok = redirect(lib, h, "AAudioStreamBuilder_setPerformanceMode",
                     reinterpret_cast<void *>(&hook_set_perf),
                     reinterpret_cast<void **>(&g_real_set_perf));
  ok = ok && redirect(lib, h, "AAudioStreamBuilder_setSharingMode",
                      reinterpret_cast<void *>(&hook_set_sharing),
                      reinterpret_cast<void **>(&g_real_set_sharing));
  ok = ok && redirect(lib, h, "AAudioStreamBuilder_openStream",
                      reinterpret_cast<void *>(&hook_open),
                      reinterpret_cast<void **>(&g_real_open));
  return ok;
}

bool is_target(const char *name) {
  if (!name) return false;
  size_t n = strlen(kTargetPackage);
  return strncmp(name, kTargetPackage, n) == 0 && (name[n] == '\0' || name[n] == ':');
}

void read_mode(int moddir) {
  if (moddir < 0) return;
  int fd = openat(moddir, "mode", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  char buf[16] = {0};
  ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n > 0 && buf[0] >= '0' && buf[0] <= '9') {
    int v = atoi(buf);
    if (v == 0 || v == 10 || v == 11 || v == 12) g_forced_perf = v;
  }
}

class Module : public zygisk::ModuleBase {
 public:
  void onLoad(zygisk::Api *api, JNIEnv *env) override {
    api_ = api;
    env_ = env;
  }

  void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
    const char *name = env_->GetStringUTFChars(args->nice_name, nullptr);
    bool target = is_target(name);
    if (name) env_->ReleaseStringUTFChars(args->nice_name, name);
    if (!target) {
      api_->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
      return;
    }
    read_mode(api_->getModuleDir());
    LOGI("v0.3 loaded into Spotify, forced performance mode = %d", g_forced_perf);
    if (install_hooks(AAUDIO_LIB_NAME))
      LOGI("hooks installed");
    else
      LOGE("hook installation FAILED (see lines above)");
  }

 private:
  zygisk::Api *api_ = nullptr;
  JNIEnv *env_ = nullptr;
};

}  // namespace

REGISTER_ZYGISK_MODULE(Module)

#ifdef HOST_TEST
// Lets the host test call the installer directly.
bool host_test_install(const char *lib) { return install_hooks(lib); }
void host_test_set_mode(int m) { g_forced_perf = m; }
#endif
