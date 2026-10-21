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
#include <math.h>

#define TAG "ysmod"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define YSMOD_VER "v1.5-lsp"

static char g_outdir[256] = {0};
static int  g_test_mode = 0;   // 1 = 跳过 head 绘制（验证能否精确接管某个部件）
static int  g_cfg_ib = 1;      // 替换索引缓冲
static int  g_cfg_vb = 1;      // 替换顶点缓冲

// ── mod 资源 ──────────────────────────────────────────────────────
static uint32_t *g_ib_mod[3] = {nullptr, nullptr, nullptr};   // head/body/dress
static size_t    g_ib_mod_n[3] = {0, 0, 0};
static uint8_t  *g_vb_pos = nullptr;   static size_t g_vb_pos_sz = 0;
static uint8_t  *g_vb_blend = nullptr; static size_t g_vb_blend_sz = 0;
static uint8_t  *g_vb_uv = nullptr;    static size_t g_vb_uv_sz = 0;
static int g_vb_ready = 0;

static uint8_t *load_file(const char *dir, const char *name, size_t *outsz) {
    char p[400];
    snprintf(p, sizeof(p), "%s/mod/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { LOGE("mod missing: %s", p); return nullptr; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return nullptr; }
    uint8_t *b = (uint8_t *)malloc((size_t)sz);
    if (!b) { fclose(f); return nullptr; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return nullptr; }
    fclose(f);
    *outsz = (size_t)sz;
    return b;
}

static void load_mod(const char *dir) {
    size_t sz = 0;
    uint8_t *p;
    p = load_file(dir, "head.ib", &sz);  if (p) { g_ib_mod[0] = (uint32_t *)p; g_ib_mod_n[0] = sz / 4; }
    p = load_file(dir, "body.ib", &sz);  if (p) { g_ib_mod[1] = (uint32_t *)p; g_ib_mod_n[1] = sz / 4; }
    p = load_file(dir, "dress.ib", &sz); if (p) { g_ib_mod[2] = (uint32_t *)p; g_ib_mod_n[2] = sz / 4; }
    g_vb_pos   = load_file(dir, "position.buf", &g_vb_pos_sz);
    g_vb_blend = load_file(dir, "blend.buf",    &g_vb_blend_sz);
    g_vb_uv    = load_file(dir, "texcoord.buf", &g_vb_uv_sz);
    g_vb_ready = (g_vb_pos && g_vb_blend && g_vb_uv);
    LOGI("mod: ib=%zu/%zu/%zu pos=%zu blend=%zu uv=%zu",
         g_ib_mod_n[0], g_ib_mod_n[1], g_ib_mod_n[2],
         g_vb_pos_sz, g_vb_blend_sz, g_vb_uv_sz);
}

// 通过内容判断这个顶点缓冲属于哪一路（40=position / 32=blend / 20=texcoord）
static int vb_classify(const uint8_t *p, size_t size) {
    int best = 0, bestscore = 0;
    if (size >= 40 * 16) {
        int n = (int)(size / 40); if (n > 64) n = 64;
        int s = 0;
        for (int i = 0; i < n; i++) {
            const float *f = (const float *)(p + (size_t)i * 40);
            int ok = 1;
            for (int k = 0; k < 3; k++) if (!(f[k] > -1e5f && f[k] < 1e5f)) ok = 0;
            if (ok && (fabsf(f[0]) > 2.f || fabsf(f[1]) > 2.f || fabsf(f[2]) > 2.f)) s++;
        }
        if (s * 2 > n && s > bestscore) { best = 40; bestscore = s; }
    }
    if (size >= 32 * 16) {
        int n = (int)(size / 32); if (n > 64) n = 64;
        int s = 0;
        for (int i = 0; i < n; i++) {
            const uint8_t *q = p + (size_t)i * 32;
            const uint16_t *u = (const uint16_t *)q;
            const float *w = (const float *)(q + 8);
            int ok = 1;
            for (int k = 0; k < 4; k++) if (u[k] > 8192) ok = 0;
            float sum = 0.f;
            for (int k = 0; k < 4; k++) {
                if (!(w[k] >= -0.01f && w[k] <= 1.01f)) ok = 0;
                sum += w[k];
            }
            if (ok && fabsf(sum - 1.f) < 0.05f) s++;
        }
        if (s * 2 > n && s > bestscore) { best = 32; bestscore = s; }
    }
    if (size >= 20 * 16) {
        int n = (int)(size / 20); if (n > 64) n = 64;
        int s = 0;
        for (int i = 0; i < n; i++) {
            const float *f = (const float *)(p + (size_t)i * 20);
            if (f[0] > -2.f && f[0] < 2.f && f[1] > -2.f && f[1] < 2.f) s++;
        }
        if (s * 4 > n * 3 && s > bestscore) { best = 20; bestscore = s; }
    }
    return best;
}

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
typedef void (*PFN_vkCmdBindVertexBuffers)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *, const VkDeviceSize *);
typedef void (*PFN_vkCmdBindVertexBuffers2)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *, const VkDeviceSize *, const VkDeviceSize *, const VkDeviceSize *);
typedef void (*PFN_vkBindBufferMemory)(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
typedef int (*PFN_vkCreateBuffer)(VkDevice, const void *, const void *, VkBuffer *);
typedef VkResult (*PFN_vkMapMemory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, uint32_t, void **);
typedef void (*PFN_vkUnmapMemory)(VkDevice, VkDeviceMemory);


static PFN_vkGetDeviceProcAddr   real_gdpa = nullptr;
static PFN_vkGetInstanceProcAddr real_gipa = nullptr;
static PFN_vkCmdDrawIndexed      real_draw_indexed = nullptr;
static PFN_vkCmdBindIndexBuffer  real_bind_ib = nullptr;
static PFN_vkCmdBindVertexBuffers  real_bind_vb = nullptr;
static PFN_vkCmdBindVertexBuffers2 real_bind_vb2 = nullptr;
static PFN_vkBindBufferMemory    real_bind_mem = nullptr;
static PFN_vkCreateBuffer        real_create_buf = nullptr;
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

// ── 顶点缓冲绑定跟踪 ──────────────────────────────────────────────
struct VbRec { VkCommandBuffer cb; VkBuffer buf[8]; VkDeviceSize off[8]; int used; };
static VbRec g_vbrec[256];
static int   g_vbrec_n = 0;

static VbRec *vbrec_get(VkCommandBuffer cb, int create) {
    for (int i = 0; i < g_vbrec_n; i++) if (g_vbrec[i].cb == cb) return &g_vbrec[i];
    if (!create || g_vbrec_n >= 256) return nullptr;
    VbRec *r = &g_vbrec[g_vbrec_n++];
    memset(r, 0, sizeof(*r));
    r->cb = cb;
    return r;
}


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
static uint32_t fnv16(const uint16_t *p, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}
static uint32_t fnv32(const uint32_t *p, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}
// 把 16 位索引零扩展成 32 位再按字节 hash —— 复刻 PC 端 R32_UINT 索引缓冲的 hash
static uint32_t fnv_expand(const uint8_t *p, size_t count, uint32_t esz, uint32_t h) {
    for (size_t i = 0; i < count; i++) {
        uint32_t v = (esz == 2) ? (uint32_t)((const uint16_t *)p)[i] : ((const uint32_t *)p)[i];
        h ^= (v & 0xFF);         h *= 0x01000193u;
        h ^= ((v >> 8) & 0xFF);  h *= 0x01000193u;
        h ^= ((v >> 16) & 0xFF); h *= 0x01000193u;
        h ^= ((v >> 24) & 0xFF); h *= 0x01000193u;
    }
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
struct BufEnt { VkBuffer buf; VkDevice dev; VkDeviceMemory mem; VkDeviceSize off; VkDeviceSize size; uint32_t hash; int hashed; int hashed2; };
static BufEnt g_bufent[8192];
static int    g_bufent_n = 0;

static BufEnt *lookup_buf(VkBuffer b) {
    for (int i = g_bufent_n - 1; i >= 0; i--)
        if (g_bufent[i].buf == b) return &g_bufent[i];
    return nullptr;
}

extern "C" int ys_vkCreateBuffer(VkDevice dev, const void *ci, const void *alloc, VkBuffer *out) {
    int r = real_create_buf ? real_create_buf(dev, ci, alloc, out) : -1;
    if (r == 0 && ci && out && *out) {
        VkDeviceSize size = *(const VkDeviceSize *)((const char *)ci + 24);
        pthread_mutex_lock(&g_mtx);
        if (g_bufent_n < 8192) {
            BufEnt e{}; e.buf = *out; e.dev = dev; e.size = size;
            g_bufent[g_bufent_n++] = e;
        }
        pthread_mutex_unlock(&g_mtx);
    }
    return r;
}

extern "C" void ys_vkBindBufferMemory(VkDevice dev, VkBuffer buf, VkDeviceMemory mem, VkDeviceSize off) {
    pthread_mutex_lock(&g_mtx);
    BufEnt *be = lookup_buf(buf);
    if (be) { be->dev = dev; be->mem = mem; be->off = off; }
    else if (g_bufent_n < 8192) {
        BufEnt n{}; n.buf = buf; n.dev = dev; n.mem = mem; n.off = off;
        g_bufent[g_bufent_n++] = n;
    }
    pthread_mutex_unlock(&g_mtx);
    if (real_bind_mem) real_bind_mem(dev, buf, mem, off);
}

extern "C" void ys_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                          const VkBuffer *bufs, const VkDeviceSize *offs) {
    pthread_mutex_lock(&g_mtx);
    VbRec *r = vbrec_get(cb, 1);
    if (r && bufs) for (uint32_t i = 0; i < count && first + i < 8; i++) {
        r->buf[first + i] = bufs[i];
        r->off[first + i] = offs ? offs[i] : 0;
        r->used |= 1 << (first + i);
    }
    pthread_mutex_unlock(&g_mtx);
    if (real_bind_vb) real_bind_vb(cb, first, count, bufs, offs);
}

extern "C" void ys_vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                           const VkBuffer *bufs, const VkDeviceSize *offs,
                                           const VkDeviceSize *sizes, const VkDeviceSize *strides) {
    pthread_mutex_lock(&g_mtx);
    VbRec *r = vbrec_get(cb, 1);
    if (r && bufs) for (uint32_t i = 0; i < count && first + i < 8; i++) {
        r->buf[first + i] = bufs[i];
        r->off[first + i] = offs ? offs[i] : 0;
        r->used |= 1 << (first + i);
    }
    pthread_mutex_unlock(&g_mtx);
    if (real_bind_vb2) real_bind_vb2(cb, first, count, bufs, offs, sizes, strides);
    else if (real_bind_vb) real_bind_vb(cb, first, count, bufs, offs);
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

static const uint32_t PART_FIRST[3] = {0u, 28278u, 71181u};
static const uint32_t PART_COUNT[3] = {28278u, 42903u, 2634u};

typedef int (*PFN_vkFlushMappedMemoryRanges)(VkDevice, uint32_t, const void *);
static PFN_vkFlushMappedMemoryRanges p_flush = nullptr;

static void flush_mem(VkDevice dev, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size) {
    if (!p_flush || !mem) return;
    struct Range { VkDeviceMemory m; VkDeviceSize o, s; } r{mem, off, size};
    p_flush(dev, 1, &r);
}

// 把一个部件的 mod 数据写进游戏正在用的缓冲（就地替换）
static void replace_part(VkCommandBuffer cb, int part, const uint8_t *base, const IbBind *ib, BufEnt *ie) {
    if (g_cfg_vb && g_vb_ready) {
        VbRec *r = vbrec_get(cb, 0);
        if (r) {
            int cls[8] = {0};
            for (int i = 0; i < 8; i++) {
                if (!(r->used & (1 << i)) || !r->buf[i]) continue;
                BufEnt *be = lookup_buf(r->buf[i]);
                if (!be || !be->mem || !p_map) continue;
                void *p = nullptr;
                if (p_map(be->dev, be->mem, be->off, be->size, 0, &p) != 0 || !p) continue;
                if ((size_t)r->off[i] < (size_t)be->size)
                    cls[i] = vb_classify((const uint8_t *)p + r->off[i], (size_t)be->size - (size_t)r->off[i]);
                if (p_unmap) p_unmap(be->dev, be->mem);
            }
            for (int i = 0; i < 8; i++) {
                if (!cls[i]) continue;
                const uint8_t *src = (cls[i] == 40) ? g_vb_pos : (cls[i] == 32) ? g_vb_blend : g_vb_uv;
                size_t ssz = (cls[i] == 40) ? g_vb_pos_sz : (cls[i] == 32) ? g_vb_blend_sz : g_vb_uv_sz;
                if (!src || !ssz) continue;
                BufEnt *be = lookup_buf(r->buf[i]);
                if (!be || !be->mem) continue;
                if ((size_t)r->off[i] + ssz > (size_t)be->size) { LOGE("vb%d too small %llu<%zu", i, (unsigned long long)be->size, ssz); continue; }
                void *p = nullptr;
                if (p_map(be->dev, be->mem, be->off, be->size, 0, &p) != 0 || !p) continue;
                memcpy((uint8_t *)p + r->off[i], src, ssz);
                flush_mem(be->dev, be->mem, be->off, be->size);
                if (p_unmap) p_unmap(be->dev, be->mem);
            }
            static int lg = 0;
            if (!lg) { lg = 1; LOGI("vb cls %d %d %d %d %d %d %d %d", cls[0], cls[1], cls[2], cls[3], cls[4], cls[5], cls[6], cls[7]); }
        }
    }
    if (g_cfg_ib && g_ib_mod[part] && g_ib_mod_n[part]) {
        uint16_t *dst = (uint16_t *)(base + (size_t)ib->off + (size_t)PART_FIRST[part] * 2);
        for (size_t i = 0; i < g_ib_mod_n[part]; i++) dst[i] = (uint16_t)(g_ib_mod[part][i] & 0xFFFF);
        if (ie) flush_mem(ie->dev, ie->mem, ie->off, ie->size);
        static int lg2 = 0;
        if (!lg2) { lg2 = 1; LOGI("ib replaced part=%d n=%zu", part, g_ib_mod_n[part]); }
    }
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
        if (e && e->size && e->mem) {
            uint32_t esz = (ib->type == 1) ? 4u : 2u;
            void *ptr = nullptr;
            if (p_map(e->dev, e->mem, e->off, e->size, 0, &ptr) == 0 && ptr) {
                const uint8_t *base = (const uint8_t *)ptr;
                size_t n = (size_t)e->size;

                if (!e->hashed) {
                    uint32_t a = fnv1a(base, n, 0x811C9DC5u);
                    uint32_t b = (esz == 2) ? fnv16((const uint16_t *)base, n / 2, 0x811C9DC5u)
                                            : fnv32((const uint32_t *)base, n / 4, 0x811C9DC5u);
                    e->hash = a; e->hashed = 1;
                    LOGI("[A] %08x [B] %08x size=%llu esz=%u", a, b, (unsigned long long)n, esz);
                }
                if (!e->hashed2) {
                    e->hashed2 = 1;
                    uint32_t d = fnv_expand(base, n / esz, esz, 0x811C9DC5u);
                    LOGI("[D] %08x idxs=%zu size=%llu esz=%u", d, n / esz, (unsigned long long)n, esz);
                    // dump 大索引缓冲，供离线分析 hash
                    if (g_outdir[0] && n >= 100000 && n <= 300000) {
                        char path[320];
                        snprintf(path, sizeof(path), "%s/ib_%llu_%u.bin", g_outdir,
                                 (unsigned long long)n, esz);
                        FILE *fp = fopen(path, "wb");
                        if (fp) {
                            fwrite(base, 1, n, fp);
                            fclose(fp);
                            LOGI("dumped %s", path);
                        } else {
                            LOGE("dump fail %s", path);
                        }
                    }
                }

                size_t start = (size_t)ib->off + (size_t)firstIndex * esz;
                size_t len   = (size_t)indexCount * esz;
                if (start + len <= n) {
                    const uint8_t *r = base + start;
                    uint32_t c = (esz == 2) ? fnv16((const uint16_t *)r, len / 2, 0x811C9DC5u)
                                            : fnv32((const uint32_t *)r, len / 4, 0x811C9DC5u);
                    uint32_t d2 = fnv_expand(r, len / esz, esz, 0x811C9DC5u);
                    if (hash_new(d2))
                        LOGI("[E] %08x idx=%u first=%u", d2, indexCount, firstIndex);
                    // 验证：head = first 0 / count 28278，命中就让这次 draw 空转
                    if (g_test_mode == 1 && firstIndex == 0 && indexCount == 28278) {
                        static int logged = 0;
                        if (!logged) { logged = 1; LOGI("TEST: skip head draw"); }
                        indexCount = 0;
                    }
                    // —— 模型替换：按 (firstIndex, indexCount) 定位部件 ——
                    if (n == 147630) {
                        int part = -1;
                        for (int pi = 0; pi < 3; pi++)
                            if (firstIndex == PART_FIRST[pi] && indexCount == PART_COUNT[pi]) part = pi;
                        if (part >= 0) {
                            replace_part(cb, part, base, ib, e);
                            if (g_cfg_ib && g_ib_mod_n[part]) indexCount = (uint32_t)g_ib_mod_n[part];
                        }
                    }
                }
                if (p_unmap) p_unmap(e->dev, e->mem);
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
    if (!strcmp(name, "vkCreateBuffer")) {
        if (!real_create_buf) { real_create_buf = (PFN_vkCreateBuffer)p; LOGI("intercept(i) vkCreateBuffer"); }
        return (PFN_vkVoidFunction)ys_vkCreateBuffer;
    }
    if (!strcmp(name, "vkBindBufferMemory")) {
        if (!real_bind_mem) { real_bind_mem = (PFN_vkBindBufferMemory)p; LOGI("intercept vkBindBufferMemory"); }
        return (PFN_vkVoidFunction)ys_vkBindBufferMemory;
    }
    if (!strcmp(name, "vkCmdBindVertexBuffers")) {
        if (!real_bind_vb) { real_bind_vb = (PFN_vkCmdBindVertexBuffers)p; LOGI("intercept vkCmdBindVertexBuffers"); }
        return (PFN_vkVoidFunction)ys_vkCmdBindVertexBuffers;
    }
    if (!strcmp(name, "vkCmdBindVertexBuffers2")) {
        if (!real_bind_vb2) { real_bind_vb2 = (PFN_vkCmdBindVertexBuffers2)p; LOGI("intercept vkCmdBindVertexBuffers2"); }
        return (PFN_vkVoidFunction)ys_vkCmdBindVertexBuffers2;
    }
    if (!strcmp(name, "vkFlushMappedMemoryRanges")) { if (!p_flush) p_flush = (PFN_vkFlushMappedMemoryRanges)p; return p; }
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
    if (!strcmp(name, "vkCreateBuffer")) {
        if (!real_create_buf) { real_create_buf = (PFN_vkCreateBuffer)p; LOGI("intercept(i) vkCreateBuffer"); }
        return (PFN_vkVoidFunction)ys_vkCreateBuffer;
    }
    if (!strcmp(name, "vkBindBufferMemory")) {
        if (!real_bind_mem) { real_bind_mem = (PFN_vkBindBufferMemory)p; LOGI("intercept(i) vkBindBufferMemory"); }
        return (PFN_vkVoidFunction)ys_vkBindBufferMemory;
    }
    if (!strcmp(name, "vkCmdBindVertexBuffers")) {
        if (!real_bind_vb) { real_bind_vb = (PFN_vkCmdBindVertexBuffers)p; LOGI("intercept vkCmdBindVertexBuffers"); }
        return (PFN_vkVoidFunction)ys_vkCmdBindVertexBuffers;
    }
    if (!strcmp(name, "vkCmdBindVertexBuffers2")) {
        if (!real_bind_vb2) { real_bind_vb2 = (PFN_vkCmdBindVertexBuffers2)p; LOGI("intercept vkCmdBindVertexBuffers2"); }
        return (PFN_vkVoidFunction)ys_vkCmdBindVertexBuffers2;
    }
    if (!strcmp(name, "vkFlushMappedMemoryRanges")) { if (!p_flush) p_flush = (PFN_vkFlushMappedMemoryRanges)p; return p; }
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
Java_com_ysmod_hook_Entry_start(JNIEnv *env, jclass clazz, jstring outDir) {
    if (outDir) {
        const char *s = env->GetStringUTFChars(outDir, nullptr);
        if (s) {
            strncpy(g_outdir, s, sizeof(g_outdir) - 1);
            env->ReleaseStringUTFChars(outDir, s);
        }
    }
    LOGI("%s starting (pid=%d) outdir=%s", YSMOD_VER, getpid(), g_outdir);
    {
        char cfg[320];
        snprintf(cfg, sizeof(cfg), "%s/ysmod.cfg", g_outdir);
        FILE *f = fopen(cfg, "r");
        if (f) {
            char line[128];
            while (fgets(line, sizeof(line), f)) {
                if (!strncmp(line, "test=", 5)) g_test_mode = atoi(line + 5);
                else if (!strncmp(line, "ib=", 3)) g_cfg_ib = atoi(line + 3);
                else if (!strncmp(line, "vb=", 3)) g_cfg_vb = atoi(line + 3);
            }
            fclose(f);
            LOGI("cfg test=%d ib=%d vb=%d", g_test_mode, g_cfg_ib, g_cfg_vb);
        } else {
            LOGI("cfg not found (%s)", cfg);
        }
    }
    load_mod(g_outdir);
    pthread_t t;
    if (pthread_create(&t, nullptr, worker, nullptr) == 0) pthread_detach(t);
}
