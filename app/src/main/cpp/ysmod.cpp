// ysmod.cpp — 原神 Android 模型 mod 加载器（Zygisk 模块）
// v0.7: M3 —— 跟踪索引缓冲绑定 + 计算 FNV-1a 32 hash（对齐 3DMigoto）
#include <sys/types.h>
#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <stdint.h>

#define TAG "ysmod"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define YSMOD_VER "v0.7-lsp"

typedef struct VkDevice_T        *VkDevice;
typedef struct VkInstance_T      *VkInstance;
typedef struct VkCommandBuffer_T *VkCommandBuffer;
typedef struct VkBuffer_T        *VkBuffer;
typedef struct VkDeviceMemory_T  *VkDeviceMemory;
typedef uint64_t VkDeviceSize;
typedef int      VkResult;
typedef void (*PFN_vkVoidFunction)(void);

typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice, const char *);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char *);

typedef void (*PFN_vkCmdDrawIndexed)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t, uint32_t);
typedef void (*PFN_vkCmdBindIndexBuffer)(VkCommandBuffer, VkBuffer, VkDeviceSize, int);
typedef void (*PFN_vkBindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
typedef VkResult (*PFN_vkMapMemory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, uint32_t, void **);
typedef void (*PFN_vkUnmapMemory)(VkDevice, VkDeviceMemory);


static PFN_vkGetDeviceProcAddr   real_gdpa = nullptr;
static PFN_vkGetInstanceProcAddr real_gipa = nullptr;
static PFN_vkCmdDrawIndexed      real_draw_indexed = nullptr;
static PFN_vkCmdBindIndexBuffer  real_bind_ib = nullptr;
static PFN_vkBindBufferMemory    real_bind_mem = nullptr;
static PFN_vkMapMemory           p_map = nullptr;
static PFN_vkUnmapMemory         p_unmap = nullptr;

// buffer -> (device, memory, offset)
struct BufInfo { VkDevice dev; VkDeviceMemory mem; VkDeviceSize off; };
static BufInfo g_bufs[4096];
static int     g_bufs_n = 0;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

// cmd buffer -> 当前绑定的 IB
struct IbBind { VkCommandBuffer cb; VkBuffer buf; VkDeviceSize off; int type; };
static IbBind g_ib[256];
static int    g_ib_n = 0;

static bool pcrel(uint32_t w) {
    if ((w & 0x7C000000u) == 0x14000000u) return true;
    if ((w & 0x9F000000u) == 0x10000000u) return true;
    if ((w & 0x9F000000u) == 0x90000000u) return true;
    if ((w & 0x3B000000u) == 0x18000000u) return true;
    if ((w & 0x7E000000u) == 0x34000000u) return true;
    if ((w & 0x7E000000u) == 0x36000000u) return true;
    return false;
}

static void *follow_branch(void *p, const char *what) {
    for (int i = 0; i < 8; i++) {
        uint32_t w = *(uint32_t *)p;
        if ((w & 0xFC000000u) == 0x14000000u) {
            int32_t off = (int32_t)(w & 0x03FFFFFFu);
            if (off & (1 << 25)) off -= (1 << 26);
            p = (char *)p + ((intptr_t)off << 2);
            continue;
        }
        break;
    }
    (void)what;
    return p;
}

static void *install_hook(void *target, void *hook, const char *what) {
    if (!target || !hook) { LOGE("%s: null ptr", what); return nullptr; }
    uint32_t *p = (uint32_t *)target;
    for (int i = 0; i < 4; i++)
        if (pcrel(p[i])) { LOGE("%s: prologue PC-relative (%08x)", what, p[i]); return nullptr; }

    void *t = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) { LOGE("%s: mmap fail", what); return nullptr; }
    memcpy(t, target, 16);
    uint32_t *q = (uint32_t *)((char *)t + 16);
    q[0] = 0x58000050u; q[1] = 0xD61F0200u;
    *(uint64_t *)(q + 2) = (uint64_t)((char *)target + 16);
    __builtin___clear_cache((char *)t, (char *)t + 32);

    uintptr_t pg = (uintptr_t)target & ~(uintptr_t)0xFFF;
    if (mprotect((void *)pg, 8192, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("%s: mprotect fail", what); return nullptr;
    }
    p[0] = 0x58000050u; p[1] = 0xD61F0200u;
    *(uint64_t *)(p + 2) = (uint64_t)hook;
    __builtin___clear_cache((char *)target, (char *)target + 16);
    LOGI("%s: hooked %p (tramp %p)", what, target, t);
    return t;
}

// ── FNV-1a 32（3DMigoto 用的索引缓冲 hash）──────────────────────────
static uint32_t fnv1a(const uint8_t *p, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}

static BufInfo *find_buf(VkBuffer b) {
    for (int i = g_bufs_n - 1; i >= 0; i--) if (g_bufs[i].dev && (VkBuffer)(uintptr_t)i == b) {}
    // 线性查找（表很小）
    for (int i = 0; i < g_bufs_n; i++)
        if (g_bufs[i].mem && g_bufs[i].dev) {
            // 用 mem 字段区分：这里用 buf 指针存在 off 的伴生表
        }
    return nullptr;
}

// 简化：单独存 buffer 指针
struct BufEnt { VkBuffer buf; VkDevice dev; VkDeviceMemory mem; VkDeviceSize off; };
static BufEnt g_bufent[8192];
static int    g_bufent_n = 0;

static BufEnt *lookup_buf(VkBuffer b) {
    for (int i = g_bufent_n - 1; i >= 0; i--)
        if (g_bufent[i].buf == b) return &g_bufent[i];
    return nullptr;
}

extern "C" void ys_vkBindBufferMemory(VkDevice dev, VkBuffer buf, VkDeviceMemory mem, VkDeviceSize off) {
    if (g_bufent_n < 8192) {
        pthread_mutex_lock(&g_mtx);
        g_bufent[g_bufent_n++] = BufEnt{buf, dev, mem, off};
        pthread_mutex_unlock(&g_mtx);
    }
    if (real_bind_mem) real_bind_mem(dev, buf, mem, off);
}

extern "C" void ys_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, int type) {
    pthread_mutex_lock(&g_mtx);
    bool found = false;
    for (int i = 0; i < g_ib_n; i++)
        if (g_ib[i].cb == cb) { g_ib[i] = IbBind{cb, buf, off, type}; found = true; break; }
    if (!found && g_ib_n < 256) g_ib[g_ib_n++] = IbBind{cb, buf, off, type};
    pthread_mutex_unlock(&g_mtx);
    if (real_bind_ib) real_bind_ib(cb, buf, off, type);
}

static uint32_t g_hashes[512];
static int      g_hashes_n = 0;
static uint64_t g_draws = 0;

static bool hash_new(uint32_t h) {
    for (int i = 0; i < g_hashes_n; i++) if (g_hashes[i] == h) return false;
    if (g_hashes_n < 512) { g_hashes[g_hashes_n++] = h; return true; }
    return false;
}

static IbBind *find_ib(VkCommandBuffer cb) {
    for (int i = 0; i < g_ib_n; i++) if (g_ib[i].cb == cb) return &g_ib[i];
    return nullptr;
}

extern "C" void ys_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t indexCount, uint32_t instanceCount,
                                    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    g_draws++;
    IbBind *ib = find_ib(cb);
    if (ib && ib->buf && p_map) {
        BufEnt *e = lookup_buf(ib->buf);
        if (e) {
            uint32_t esz = (ib->type == 1) ? 4 : 2;
            VkDeviceSize start = e->off + ib->off + (VkDeviceSize)firstIndex * esz;
            VkDeviceSize len   = (VkDeviceSize)indexCount * esz;
            void *ptr = nullptr;
            if (p_map(e->dev, e->mem, start, len, 0, &ptr) == 0 && ptr) {
                uint32_t h = fnv1a((const uint8_t *)ptr, (size_t)len, 0x811C9DC5u);
                if (p_unmap) p_unmap(e->dev, e->mem);
                if (hash_new(h))
                    LOGI("[hash] %08x indexCount=%u firstIndex=%u vtxOff=%d",
                         h, indexCount, firstIndex, vertexOffset);
            }
        }
    }
    if (real_draw_indexed) real_draw_indexed(cb, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

extern "C" PFN_vkVoidFunction ys_vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    if (!real_gdpa) return nullptr;
    PFN_vkVoidFunction p = real_gdpa(dev, name);
    if (!p || !name) return p;
    if (!strcmp(name, "vkCmdDrawIndexed")) {
        if (!real_draw_indexed) { real_draw_indexed = (PFN_vkCmdDrawIndexed)p; LOGI("intercept vkCmdDrawIndexed"); }
        return (PFN_vkVoidFunction)ys_vkCmdDrawIndexed;
    }
    if (!strcmp(name, "vkCmdBindIndexBuffer")) {
        if (!real_bind_ib) { real_bind_ib = (PFN_vkCmdBindIndexBuffer)p; LOGI("intercept vkCmdBindIndexBuffer"); }
        return (PFN_vkVoidFunction)ys_vkCmdBindIndexBuffer;
    }
    if (!strcmp(name, "vkBindBufferMemory")) {
        if (!real_bind_mem) { real_bind_mem = (PFN_vkBindBufferMemory)p; LOGI("intercept vkBindBufferMemory"); }
        return (PFN_vkVoidFunction)ys_vkBindBufferMemory;
    }
    if (!strcmp(name, "vkMapMemory"))   { if (!p_map)   p_map   = (PFN_vkMapMemory)p;   return p; }
    if (!strcmp(name, "vkUnmapMemory")) { if (!p_unmap) p_unmap = (PFN_vkUnmapMemory)p; return p; }
    return p;
}


extern "C" PFN_vkVoidFunction ys_vkGetInstanceProcAddr(VkInstance inst, const char *name) {
    if (!real_gipa) return nullptr;
    PFN_vkVoidFunction p = real_gipa(inst, name);
    if (!p || !name) return p;
    if (!strcmp(name, "vkCmdDrawIndexed")) {
        if (!real_draw_indexed) { real_draw_indexed = (PFN_vkCmdDrawIndexed)p; LOGI("intercept(i) vkCmdDrawIndexed"); }
        return (PFN_vkVoidFunction)ys_vkCmdDrawIndexed;
    }
    if (!strcmp(name, "vkCmdBindIndexBuffer")) {
        if (!real_bind_ib) { real_bind_ib = (PFN_vkCmdBindIndexBuffer)p; LOGI("intercept(i) vkCmdBindIndexBuffer"); }
        return (PFN_vkVoidFunction)ys_vkCmdBindIndexBuffer;
    }
    if (!strcmp(name, "vkBindBufferMemory")) {
        if (!real_bind_mem) { real_bind_mem = (PFN_vkBindBufferMemory)p; LOGI("intercept(i) vkBindBufferMemory"); }
        return (PFN_vkVoidFunction)ys_vkBindBufferMemory;
    }
    if (!strcmp(name, "vkMapMemory"))   { if (!p_map)   p_map   = (PFN_vkMapMemory)p;   return p; }
    if (!strcmp(name, "vkUnmapMemory")) { if (!p_unmap) p_unmap = (PFN_vkUnmapMemory)p; return p; }
    return p;
}

static bool lib_loaded(const char *name) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512]; bool found = false;
    while (fgets(line, sizeof(line), f)) if (strstr(line, name)) { found = true; break; }
    fclose(f);
    return found;
}

static void *worker(void *) {
    for (int i = 0; i < 240; i++) {
        if (lib_loaded("libvulkan.so")) break;
        usleep(100 * 1000);
    }
    void *h = dlopen("libvulkan.so", RTLD_NOW | RTLD_NOLOAD);
    if (!h) h = dlopen("libvulkan.so", RTLD_NOW);
    if (!h) { LOGE("cannot open libvulkan.so"); return nullptr; }

    void *gdpa = dlsym(h, "vkGetDeviceProcAddr");

    if (gdpa) {
        void *real = follow_branch(gdpa, "gdpa");
        void *t = install_hook(real, (void *)ys_vkGetDeviceProcAddr, "vkGetDeviceProcAddr");
        if (t) real_gdpa = (PFN_vkGetDeviceProcAddr)t;
    }
    void *gipa = dlsym(h, "vkGetInstanceProcAddr");
    if (gipa) {
        void *real = follow_branch(gipa, "gipa");
        void *t = install_hook(real, (void *)ys_vkGetInstanceProcAddr, "vkGetInstanceProcAddr");
        if (t) real_gipa = (PFN_vkGetInstanceProcAddr)t;
    }
    LOGI("ready");
    return nullptr;
}


// ─── JNI 入口：由 LSPosed 模块在目标进程里调用 ───
extern "C" JNIEXPORT void JNICALL
Java_com_ysmod_hook_Entry_start(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    LOGI("%s starting (pid=%d)", YSMOD_VER, getpid());
    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t);
}
