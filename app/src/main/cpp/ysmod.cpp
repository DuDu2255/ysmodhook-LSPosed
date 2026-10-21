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
#define YSMOD_VER "v3.2-lsp"

static char g_outdir[256] = {0};
static int  g_test_mode = 0;   // 1 = 跳过 head 绘制（验证能否精确接管某个部件）
static int  g_cfg_ib = 1;      // 替换索引缓冲
static int  g_cfg_vb = 1;      // 替换顶点缓冲
static int  g_enabled = 1;     // 模块开关（设置界面）
static int  g_ib_size = 147630; // 目标合并索引缓冲大小，0 = 不限制
static int  g_cfg_probe = 0;   // 1 = 位置探针，2 = UV 探针
static int  g_cfg_verbose = 0; // 1 = 输出每个缓冲的 hash 日志
static int  g_cfg_dump = 0;    // 1 = 导出顶点/权重缓冲供离线分析
static int  g_cfg_enable = 1;  // 配置文件里的总开关（设置界面的开关失效时用）

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
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
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
typedef void (*PFN_vkDestroyBuffer)(VkDevice, VkBuffer, const void *);
static PFN_vkDestroyBuffer         real_destroy_buf = nullptr;
static PFN_vkCmdBindVertexBuffers  real_bind_vb = nullptr;
static PFN_vkCmdBindVertexBuffers2 real_bind_vb2 = nullptr;
typedef VkResult (*PFN_vkAllocateMemory)(VkDevice, const void *, const void *, VkDeviceMemory *);
typedef void (*PFN_vkGetBufferMemoryRequirements)(VkDevice, VkBuffer, void *);
typedef void (*PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, void *);
typedef void (*PFN_vkCmdCopyBuffer)(VkCommandBuffer, VkBuffer, VkBuffer, uint32_t, const void *);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const void *, const void *, VkDevice *);

// ── 计算管线追踪（用于定位景深等后处理 pass）──────────────────────
typedef VkResult (*PFN_vkCreateShaderModule)(VkDevice, const void *, const void *, void **);
typedef VkResult (*PFN_vkCreateComputePipelines)(VkDevice, void *, uint32_t, const void *, const void *, void **);
typedef void (*PFN_vkCmdBindPipeline)(VkCommandBuffer, int, void *);
typedef void (*PFN_vkCmdDispatch)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);

static PFN_vkCreateShaderModule      real_create_shader = nullptr;
static PFN_vkCreateComputePipelines  real_create_cpipe = nullptr;
static PFN_vkCmdBindPipeline         real_bind_pipe = nullptr;
static PFN_vkCmdDispatch             real_dispatch = nullptr;
typedef VkResult (*PFN_vkCreateGraphicsPipelines)(VkDevice, void *, uint32_t, const void *, const void *, void **);
typedef void (*PFN_vkCmdDraw)(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
// ── 描述符追踪（找骨骼矩阵 UBO）─────────────────────────────────
typedef void (*PFN_vkUpdateDescriptorSets)(VkDevice, uint32_t, const void *, uint32_t, const void *);
typedef void (*PFN_vkCmdBindDescriptorSets)(VkCommandBuffer, int, void *, uint32_t, uint32_t, const void **, uint32_t, const uint32_t *);
static PFN_vkUpdateDescriptorSets    real_update_desc = nullptr;
static PFN_vkCmdBindDescriptorSets   real_bind_desc = nullptr;

struct DescBufInfo { void *buffer; VkDeviceSize offset, range; };
struct WriteDescSet {
    int sType; const void *pNext; void *dstSet;
    uint32_t dstBinding, dstArrayElement, descriptorCount, descriptorType;
    const void *pImageInfo; const DescBufInfo *pBufferInfo; const void *pTexelBufferView;
};
struct DescEnt { void *set; uint32_t binding; VkBuffer buf; VkDeviceSize off, range; };
static DescEnt g_desc[8192];
static int     g_desc_n = 0;
struct DSetRec { VkCommandBuffer cb; void *sets[8]; int n; };
static DSetRec g_dset[128];
static int     g_dset_n = 0;

static PFN_vkCreateGraphicsPipelines real_create_gpipe = nullptr;
static PFN_vkCmdDraw                 real_cmd_draw = nullptr;
static uint32_t g_cur_gpipe[256];
static VkCommandBuffer g_gcb_slot[256];
static uint64_t g_gdraw_seen[128];
static int      g_gdraw_n = 0;

static uint32_t g_shader_hash[2048];      // shaderModule -> hash
static int      g_shader_hash_n = 0;
static uint32_t g_pipe_hash[4096];        // computePipeline -> hash
static int      g_pipe_hash_n = 0;
static uint32_t g_cur_cpipe[256];         // commandBuffer -> pipeline hash
static VkCommandBuffer g_cb_slot[256];
static uint32_t g_disp_seen[64];          // 已记录过的 hash
static int      g_disp_seen_n = 0;
static uint64_t g_disp_count = 0;

static PFN_vkAllocateMemory                real_alloc_mem = nullptr;
static PFN_vkGetBufferMemoryRequirements   real_get_buf_req = nullptr;
static PFN_vkGetPhysicalDeviceMemoryProperties real_get_mem_props = nullptr;
static PFN_vkCmdCopyBuffer                 real_copy_buf = nullptr;
static PFN_vkCreateDevice                  real_create_dev = nullptr;
static VkPhysicalDevice                    g_phys = nullptr;

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

static void    *g_shader_mod[2048];
static void    *g_pipe_ptr[4096];
static int      g_shader_mod_n = 0;

static uint32_t shader_hash_of(void *mod) {
    for (int i = 0; i < g_shader_mod_n; i++) if (g_shader_mod[i] == mod) return g_shader_hash[i];
    return 0;
}
static uint32_t pipe_hash_of(void *pipe) {
    for (int i = 0; i < g_pipe_hash_n; i++) if (g_pipe_ptr[i] == pipe) return g_pipe_hash[i];
    return 0;
}

extern "C" int ys_vkCreateShaderModule(VkDevice dev, const void *ci, const void *alloc, void **out) {
    int r = real_create_shader ? real_create_shader(dev, ci, alloc, out) : -1;
    if (r == 0 && ci && out && *out && g_shader_mod_n < 2048) {
        uint64_t sz = *(const uint64_t *)((const uint8_t *)ci + 24);
        const void *code = *(const void *const *)((const uint8_t *)ci + 32);
        if (code && sz) {
            g_shader_mod[g_shader_mod_n] = *out;
            g_shader_hash[g_shader_mod_n] = fnv1a((const uint8_t *)code, (size_t)sz, 0x811C9DC5u);
            g_shader_mod_n++;
        }
    }
    return r;
}

extern "C" int ys_vkCreateComputePipelines(VkDevice dev, void *cache, uint32_t count,
                                           const void *cis, const void *alloc, void **out) {
    int r = real_create_cpipe ? real_create_cpipe(dev, cache, count, cis, alloc, out) : -1;
    if (r == 0 && cis && out) {
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *base = (const uint8_t *)cis + (size_t)i * 96;
            void *mod = *(void *const *)(base + 48);      // stage.module
            if (out[i] && g_pipe_hash_n < 4096) {
                g_pipe_ptr[g_pipe_hash_n] = out[i];
                g_pipe_hash[g_pipe_hash_n] = shader_hash_of(mod);
                g_pipe_hash_n++;
            }
        }
    }
    return r;
}

extern "C" void ys_vkCmdBindPipeline(VkCommandBuffer cb, int bindPoint, void *pipe) {
    if (bindPoint == 1) {   // GRAPHICS
        int slot = -1, empty = -1;
        for (int i = 0; i < 256; i++) {
            if (g_gcb_slot[i] == cb) { slot = i; break; }
            if (!g_gcb_slot[i] && empty < 0) empty = i;
        }
        if (slot < 0) slot = empty;
        if (slot >= 0) { g_gcb_slot[slot] = cb; g_cur_gpipe[slot] = pipe_hash_of(pipe); }
    }
    if (bindPoint == 0) {   // VK_PIPELINE_BIND_POINT_COMPUTE
        int slot = -1, empty = -1;
        for (int i = 0; i < 256; i++) {
            if (g_cb_slot[i] == cb) { slot = i; break; }
            if (!g_cb_slot[i] && empty < 0) empty = i;
        }
        if (slot < 0) slot = empty;
        if (slot >= 0) { g_cb_slot[slot] = cb; g_cur_cpipe[slot] = pipe_hash_of(pipe); }
    }
    if (real_bind_pipe) real_bind_pipe(cb, bindPoint, pipe);
}

extern "C" int ys_vkCreateGraphicsPipelines(VkDevice dev, void *cache, uint32_t count,
                                            const void *cis, const void *alloc, void **out) {
    int r = real_create_gpipe ? real_create_gpipe(dev, cache, count, cis, alloc, out) : -1;
    if (r == 0 && cis && out) {
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t *base = (const uint8_t *)cis + (size_t)i * 104;  // sizeof(VkGraphicsPipelineCreateInfo)
            uint32_t sc = *(const uint32_t *)(base + 20);
            const uint8_t *stages = *(const uint8_t *const *)(base + 24);
            void *frag = nullptr;
            for (uint32_t s = 0; s < sc && stages; s++) {
                const uint8_t *st = stages + (size_t)s * 48;
                int bit = *(const int *)(st + 20);
                if (bit == 0x10) { frag = *(void *const *)(st + 24); break; }   // FRAGMENT
                if (s == 0) frag = *(void *const *)(st + 24);
            }
            if (out[i] && g_pipe_hash_n < 4096) {
                g_pipe_ptr[g_pipe_hash_n] = out[i];
                g_pipe_hash[g_pipe_hash_n] = frag ? shader_hash_of(frag) : 0;
                g_pipe_hash_n++;
            }
        }
    }
    return r;
}

extern "C" void ys_vkCmdDraw(VkCommandBuffer cb, uint32_t vcount, uint32_t icount,
                             uint32_t fvertex, uint32_t finstance) {
    uint32_t h = 0;
    for (int i = 0; i < 256; i++) if (g_gcb_slot[i] == cb) { h = g_cur_gpipe[i]; break; }
    if (h && vcount <= 8) {
        uint64_t key = ((uint64_t)h << 16) | (vcount & 0xFFFF);
        bool seen = false;
        for (int i = 0; i < g_gdraw_n; i++) if (g_gdraw_seen[i] == key) { seen = true; break; }
        if (!seen && g_gdraw_n < 128) {
            g_gdraw_seen[g_gdraw_n++] = key;
            LOGI("fulldraw %08x verts=%u", h, vcount);
        }
    }
    if (real_cmd_draw) real_cmd_draw(cb, vcount, icount, fvertex, finstance);
}

extern "C" void ys_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    g_disp_count++;
    uint32_t h = 0;
    for (int i = 0; i < 256; i++) if (g_cb_slot[i] == cb) { h = g_cur_cpipe[i]; break; }
    if (h) {
        bool seen = false;
        for (int i = 0; i < g_disp_seen_n; i++) if (g_disp_seen[i] == h) { seen = true; break; }
        if (!seen && g_disp_seen_n < 64) {
            g_disp_seen[g_disp_seen_n++] = h;
            LOGI("compute %08x dims=%ux%ux%u", h, x, y, z);
        }
    }
    if (real_dispatch) real_dispatch(cb, x, y, z);
}

extern "C" int ys_vkCreateDevice(VkPhysicalDevice phys, const void *ci, const void *alloc, VkDevice *out) {
    g_phys = phys;
    LOGI("vkCreateDevice phys=%p", (void *)phys);
    if (!real_create_dev) return -1;
    return real_create_dev(phys, ci, alloc, out);
}

extern "C" void ys_vkDestroyBuffer(VkDevice dev, VkBuffer buf, const void *alloc) {
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_bufent_n; i++) {
        if (g_bufent[i].buf == buf) { g_bufent[i] = g_bufent[--g_bufent_n]; break; }
    }
    pthread_mutex_unlock(&g_mtx);
    if (real_destroy_buf) real_destroy_buf(dev, buf, alloc);
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

// VkMappedMemoryRange 必须以 sType/pNext 开头（sType = 6），否则驱动读到垃圾指针直接崩
struct MappedRange {
    int           sType;
    const void   *pNext;
    VkDeviceMemory memory;
    VkDeviceSize   offset;
    VkDeviceSize   size;
};
static int g_cfg_flush = 1;

typedef int (*PFN_vkInvalidateMappedMemoryRanges)(VkDevice, uint32_t, const void *);
static PFN_vkInvalidateMappedMemoryRanges p_inval = nullptr;

static void invalidate_mem(VkDevice dev, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size) {
    if (!p_inval || !mem) return;
    MappedRange r{6, nullptr, mem, off, size};
    p_inval(dev, 1, &r);
}

static void flush_mem(VkDevice dev, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size) {
    if (!g_cfg_flush || !p_flush || !mem) return;
    MappedRange r{6, nullptr, mem, off, size};
    p_flush(dev, 1, &r);
}

// 列出所有跟踪到的大缓冲，便于找真正的顶点数据
static void list_bufs(void) {
    int shown = 0;
    for (int i = 0; i < g_bufent_n && shown < 40; i++) {
        BufEnt *b = &g_bufent[i];
        if (!b->mem || b->size < 200000 || b->size > 2000000) continue;
        LOGI("buf[%d] size=%llu", i, (unsigned long long)b->size);
        shown++;
    }
    LOGI("bufs total=%d", g_bufent_n);
}

// 探针：把 binding0 的所有顶点 x 坐标 +0.5，看模型会不会整体平移
static uint8_t *g_base[8] = {nullptr};
static size_t   g_base_sz[8] = {0};

// 把某个 binding 的顶点属性整体平移，用来确认这条数据通道是否真的被 GPU 使用
static void probe_shift(VkCommandBuffer cb, int binding, int stride, int ncomp, float delta) {
    VbRec *r = vbrec_get(cb, 0);
    if (!r || binding >= 8 || !r->buf[binding] || !p_map) return;
    BufEnt *be = lookup_buf(r->buf[binding]);
    if (!be || !be->mem) return;
    void *p = nullptr;
    if (p_map(be->dev, be->mem, be->off, be->size, 0, &p) != 0 || !p) return;
    invalidate_mem(be->dev, be->mem, be->off, be->size);
    if (!g_base[binding] || g_base_sz[binding] != (size_t)be->size) {
        free(g_base[binding]);
        g_base[binding] = (uint8_t *)malloc((size_t)be->size);
        if (!g_base[binding]) { if (p_unmap) p_unmap(be->dev, be->mem); return; }
        memcpy(g_base[binding], p, (size_t)be->size);
        g_base_sz[binding] = (size_t)be->size;
        LOGI("probe base vb%d %zu bytes", binding, g_base_sz[binding]);
    }
    memcpy(p, g_base[binding], g_base_sz[binding]);
    float *f = (float *)((uint8_t *)p + r->off[binding]);
    size_t n = (size_t)((be->size - r->off[binding]) / stride);
    for (size_t i = 0; i < n; i++)
        for (int k = 0; k < ncomp; k++) f[i * (stride / 4) + k] += delta;
    flush_mem(be->dev, be->mem, be->off, be->size);
    if (p_unmap) p_unmap(be->dev, be->mem);
    static int lg = 0;
    if (!lg) { lg = 1; LOGI("probe vb%d shifted %+0.2f (n=%zu stride=%d)", binding, delta, n, stride); }
}

// ── GPU 侧替换：自建缓冲 + vkCmdCopyBuffer ────────────────────────
struct MemReqs { VkDeviceSize size, alignment; uint32_t memoryTypeBits; };
struct MemType { int propertyFlags; uint32_t heapIndex; };
struct MemHeap { VkDeviceSize size; int flags; };
struct MemProps { uint32_t memoryTypeCount; MemType memoryTypes[32]; uint32_t memoryHeapCount; MemHeap memoryHeaps[16]; };
struct BufferCI { int sType; const void *pNext; int flags; VkDeviceSize size; int usage; int sharingMode; uint32_t qfCount; const uint32_t *qf; };
struct AllocInfo { int sType; const void *pNext; VkDeviceSize size; uint32_t memoryTypeIndex; };
struct CopyRegion { VkDeviceSize srcOffset, dstOffset, size; };

static int g_cfg_mode = 2;   // 1 = CPU 直写（会闪），2 = GPU 拷贝
static int g_copy_pos = 1, g_copy_uv = 1, g_copy_ib = 1, g_copy_blend = 1;
static int g_cfg_wprobe = 0;   // 1 = 权重全写成「100% 骨骼0」，验证缓冲是否被使用
static int g_cfg_posprobe = 0; // 1 = 位置全写成同一点，验证位置缓冲是否被使用
static VkBuffer g_buf_wconst = nullptr;
static VkBuffer g_buf_pconst = nullptr;

static int find_mem_type(uint32_t bits) {
    if (!g_phys || !real_get_mem_props) return -1;
    MemProps mp{};
    real_get_mem_props(g_phys, &mp);
    int fallback = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount && i < 32; i++) {
        if (!(bits & (1u << i))) continue;
        int f = mp.memoryTypes[i].propertyFlags;
        if ((f & 0x2) && (f & 0x4)) {
            if (f & 0x1) return (int)i;      // 优先 DEVICE_LOCAL + HOST_VISIBLE
            if (fallback < 0) fallback = (int)i;
        }
    }
    return fallback;
}

static VkBuffer make_buf(VkDevice dev, const void *data, size_t size) {
    if (!real_create_buf || !real_alloc_mem || !real_bind_mem || !real_get_buf_req || !size) return nullptr;
    BufferCI ci{12, nullptr, 0, (VkDeviceSize)size, 0x1 | 0x80 | 0x40, 0, 0, nullptr};
    VkBuffer b = nullptr;
    if (real_create_buf(dev, &ci, nullptr, &b) != 0 || !b) { LOGE("make_buf create fail"); return nullptr; }
    MemReqs rq{};
    real_get_buf_req(dev, b, &rq);
    int mt = find_mem_type(rq.memoryTypeBits);
    if (mt < 0) { LOGE("make_buf no memtype"); return nullptr; }
    AllocInfo ai{5, nullptr, rq.size, (uint32_t)mt};
    VkDeviceMemory m = nullptr;
    if (real_alloc_mem(dev, &ai, nullptr, &m) != 0 || !m) { LOGE("make_buf alloc fail"); return nullptr; }
    real_bind_mem(dev, b, m, 0);
    void *p = nullptr;
    if (p_map && p_map(dev, m, 0, rq.size, 0, &p) == 0 && p) {
        memcpy(p, data, size);
        flush_mem(dev, m, 0, rq.size);
        if (p_unmap) p_unmap(dev, m);
    } else LOGE("make_buf map fail");
    return b;
}

static int       g_cfg_blend = 1;
static int       g_blend_size = 639648;   // 原版骨骼权重缓冲大小 = 19989 * 32
static VkBuffer  g_buf_pos = nullptr, g_buf_uv = nullptr, g_buf_blend = nullptr;
static VkBuffer  g_buf_ib[3] = {nullptr, nullptr, nullptr};
static uint16_t *g_ib16[3] = {nullptr, nullptr, nullptr};

static void ensure_mod_buffers(VkDevice dev) {
    if (!dev) return;
    if (!g_buf_pos && g_vb_pos && g_vb_pos_sz) g_buf_pos = make_buf(dev, g_vb_pos, g_vb_pos_sz);
    if (!g_buf_uv && g_vb_uv && g_vb_uv_sz)    g_buf_uv  = make_buf(dev, g_vb_uv, g_vb_uv_sz);
    if (!g_buf_blend && g_vb_blend && g_vb_blend_sz && g_cfg_blend)
        g_buf_blend = make_buf(dev, g_vb_blend, g_vb_blend_sz);
    for (int i = 0; i < 3; i++) {
        if (g_buf_ib[i] || !g_ib_mod[i] || !g_ib_mod_n[i]) continue;
        g_ib16[i] = (uint16_t *)malloc(g_ib_mod_n[i] * 2);
        if (!g_ib16[i]) continue;
        for (size_t k = 0; k < g_ib_mod_n[i]; k++) g_ib16[i][k] = (uint16_t)(g_ib_mod[i][k] & 0xFFFF);
        g_buf_ib[i] = make_buf(dev, g_ib16[i], g_ib_mod_n[i] * 2);
    }
    static int lg = 0;
    if (!lg) { lg = 1; LOGI("mod buffers pos=%p uv=%p ib=%p/%p/%p", (void *)g_buf_pos, (void *)g_buf_uv,
                            (void *)g_buf_ib[0], (void *)g_buf_ib[1], (void *)g_buf_ib[2]); }
}

// 在 draw 之前记录拷贝命令：提交时按顺序执行，必定覆盖游戏自己的上传
static bool gpu_replace(VkCommandBuffer cb, int part, const IbBind *ib, VkDevice dev) {
    if (!real_copy_buf) return false;
    ensure_mod_buffers(dev);
    VbRec *r = vbrec_get(cb, 0);
    CopyRegion rg{};
    if (g_copy_pos && r && r->buf[0]) {
        VkBuffer srcp = g_buf_pos;
        if (g_cfg_posprobe) {
            if (!g_buf_pconst && g_vb_pos_sz) {
                uint8_t *tmp = (uint8_t *)calloc(1, g_vb_pos_sz);
                if (tmp) {
                    for (size_t o = 0; o + 40 <= g_vb_pos_sz; o += 40) {
                        float *f = (float *)(tmp + o);
                        f[0] = 0.f; f[1] = 1.0f; f[2] = 0.f;   // 全部塌到 (0,1,0)
                        f[3] = 0.f; f[4] = 1.f; f[5] = 0.f;   // 法线朝上
                        f[9] = 1.f;
                    }
                    g_buf_pconst = make_buf(dev, tmp, g_vb_pos_sz);
                    free(tmp);
                }
            }
            if (g_buf_pconst) srcp = g_buf_pconst;
        }
        if (srcp) {
            rg = CopyRegion{0, r->off[0], (VkDeviceSize)g_vb_pos_sz};
            real_copy_buf(cb, srcp, r->buf[0], 1, &rg);
        }
    }
    if (g_copy_uv && g_buf_uv && r && r->buf[1]) {
        rg = CopyRegion{0, r->off[1], (VkDeviceSize)g_vb_uv_sz};
        real_copy_buf(cb, g_buf_uv, r->buf[1], 1, &rg);
    }
    if (g_copy_ib && g_buf_ib[part] && g_ib_mod_n[part]) {
        rg = CopyRegion{0, ib->off + (VkDeviceSize)PART_FIRST[part] * 2, (VkDeviceSize)(g_ib_mod_n[part] * 2)};
        real_copy_buf(cb, g_buf_ib[part], ib->buf, 1, &rg);
    }
    // 骨骼权重（原版按顶点 32 字节，走 SSBO，不在顶点绑定里）——按尺寸定位
    if (g_copy_blend && g_buf_blend && g_blend_size > 0) {
        VkBuffer tgt = nullptr;
        VkDeviceSize toff = 0;
        BufEnt *tb = nullptr;
        for (int i = g_bufent_n - 1; i >= 0; i--) {
            if (g_bufent[i].mem && (VkDeviceSize)g_blend_size == g_bufent[i].size) {
                tgt = g_bufent[i].buf; toff = g_bufent[i].off; tb = &g_bufent[i]; break;
            }
        }
        if (tgt && tb && g_cfg_dump) {
            static int db = 0;
            if (!db) {
                db = 1;
                void *p = nullptr;
                if (p_map && p_map(tb->dev, tb->mem, tb->off, tb->size, 0, &p) == 0 && p) {
                    invalidate_mem(tb->dev, tb->mem, tb->off, tb->size);
                    char path[400];
                    snprintf(path, sizeof(path), "%s/blend_target.bin", g_outdir);
                    FILE *f = fopen(path, "wb");
                    if (f) { fwrite(p, 1, (size_t)tb->size, f); fclose(f); LOGI("dumped %s", path); }
                    if (p_unmap) p_unmap(tb->dev, tb->mem);
                }
            }
        }
        if (tgt) {
            VkBuffer srcb = g_buf_blend;
            if (g_cfg_wprobe) {
                if (!g_buf_wconst && g_blend_size > 0) {
                    uint8_t *tmp = (uint8_t *)calloc(1, (size_t)g_blend_size);
                    if (tmp) {
                        for (size_t o = 0; o + 32 <= (size_t)g_blend_size; o += 32) {
                            *(float *)(tmp + o) = 1.0f;   // w0 = 1
                        }
                        g_buf_wconst = make_buf(dev, tmp, (size_t)g_blend_size);
                        free(tmp);
                    }
                }
                if (g_buf_wconst) srcb = g_buf_wconst;
            }
            // 同尺寸的缓冲可能有多份，全部写一遍
            int cnt = 0;
            for (int i = 0; i < g_bufent_n && cnt < 8; i++) {
                if (!g_bufent[i].mem || g_bufent[i].size != (VkDeviceSize)g_blend_size) continue;
                CopyRegion rb{0, g_bufent[i].off, (VkDeviceSize)g_vb_blend_sz};
                real_copy_buf(cb, srcb, g_bufent[i].buf, 1, &rb);
                cnt++;
            }
            static int lg = 0;
            if (!lg) { lg = 1; LOGI("blend replaced -> %d buffer(s) of size %d", cnt, g_blend_size); }
        }
    }
    static int lg = 0;
    if (!lg) { lg = 1; LOGI("gpu replace part=%d", part); }
    return true;
}

extern "C" void ys_vkUpdateDescriptorSets(VkDevice dev, uint32_t wcount, const void *writes,
                                          uint32_t ccount, const void *copies) {
    const WriteDescSet *w = (const WriteDescSet *)writes;
    for (uint32_t i = 0; w && i < wcount; i++) {
        if (w[i].descriptorType < 6 || w[i].descriptorType > 9) continue;
        const DescBufInfo *bi = w[i].pBufferInfo;
        if (!bi) continue;
        uint32_t cnt = w[i].descriptorCount ? w[i].descriptorCount : 1;
        for (uint32_t k = 0; k < cnt && k < 8 && g_desc_n < 8192; k++) {
            DescEnt &e = g_desc[g_desc_n++];
            e.set = w[i].dstSet;
            e.binding = w[i].dstBinding + k;
            e.buf = (VkBuffer)bi[k].buffer;
            e.off = bi[k].offset;
            e.range = bi[k].range;
        }
    }
    if (real_update_desc) real_update_desc(dev, wcount, writes, ccount, copies);
}

extern "C" void ys_vkCmdBindDescriptorSets(VkCommandBuffer cb, int bindPoint, void *layout,
                                           uint32_t firstSet, uint32_t count, const void **sets,
                                           uint32_t dynCount, const uint32_t *dyn) {
    DSetRec *r = nullptr;
    for (int i = 0; i < g_dset_n; i++) if (g_dset[i].cb == cb) { r = &g_dset[i]; break; }
    if (!r && g_dset_n < 128) { r = &g_dset[g_dset_n++]; memset(r, 0, sizeof(*r)); r->cb = cb; }
    if (r && sets) for (uint32_t i = 0; i < count && i < 8; i++) {
        r->sets[i] = (void *)sets[i];
        if ((int)i >= r->n) r->n = (int)i + 1;
    }
    if (real_bind_desc) real_bind_desc(cb, bindPoint, layout, firstSet, count, sets, dynCount, dyn);
}

// 导出当前 draw 通过描述符绑定的小 buffer（骨骼矩阵通常在这里）
static void dump_ubo(VkCommandBuffer cb) {
    DSetRec *r = nullptr;
    for (int i = 0; i < g_dset_n; i++) if (g_dset[i].cb == cb) { r = &g_dset[i]; break; }
    if (!r) { LOGE("ubo: no descriptor sets for cb"); return; }
    VkBuffer seen[16]; int seen_n = 0, dumped = 0;
    for (int i = 0; i < g_desc_n && dumped < 16; i++) {
        bool bound = false;
        for (int s = 0; s < r->n; s++) if (r->sets[s] && r->sets[s] == g_desc[i].set) bound = true;
        if (!bound) continue;
        BufEnt *be = lookup_buf(g_desc[i].buf);
        if (!be || !be->mem || !p_map) continue;
        VkDeviceSize range = g_desc[i].range;
        if (range == 0 || range == (VkDeviceSize)-1) range = be->size - g_desc[i].off;
        LOGI("desc b%u range=%llu bufsize=%llu", g_desc[i].binding,
             (unsigned long long)range, (unsigned long long)be->size);
        if (range < 256 || range > (1u << 20)) continue;
        bool dup = false;
        for (int k = 0; k < seen_n; k++) if (seen[k] == g_desc[i].buf) dup = true;
        if (dup) continue;
        if (seen_n < 16) seen[seen_n++] = g_desc[i].buf;
        void *p = nullptr;
        if (p_map(be->dev, be->mem, be->off, be->size, 0, &p) != 0 || !p) continue;
        invalidate_mem(be->dev, be->mem, be->off, be->size);
        VkDeviceSize sz = range;
        if (g_desc[i].off + sz > be->size) sz = be->size - g_desc[i].off;
        char path[400];
        snprintf(path, sizeof(path), "%s/ubo_b%u_%llu.bin", g_outdir, g_desc[i].binding, (unsigned long long)sz);
        FILE *f = fopen(path, "wb");
        if (f) {
            fwrite((const uint8_t *)p + g_desc[i].off, 1, (size_t)sz, f);
            fclose(f);
            LOGI("dumped %s (binding %u)", path, g_desc[i].binding);
            dumped++;
        }
        if (p_unmap) p_unmap(be->dev, be->mem);
    }
    LOGI("ubo dump done: %d, desc=%d, sets=%d", dumped, g_desc_n, r->n);
}

// 导出当前 draw 绑定的所有顶点缓冲，用于离线分析真实布局
static void dump_vbs(VkCommandBuffer cb) {
    VbRec *r = vbrec_get(cb, 0);
    if (!r) { LOGE("dump_vbs: no rec"); return; }
    LOGI("dump_vbs used=0x%x", r->used);
    for (int i = 0; i < 8; i++) {
        if (!(r->used & (1 << i)) || !r->buf[i]) continue;
        BufEnt *be = lookup_buf(r->buf[i]);
        if (!be || !be->mem) { LOGI("vb%d no ent", i); continue; }
        LOGI("vb%d size=%llu off=%llu", i, (unsigned long long)be->size, (unsigned long long)r->off[i]);
        void *p = nullptr;
        if (!p_map || p_map(be->dev, be->mem, be->off, be->size, 0, &p) != 0 || !p) { LOGE("vb%d map fail", i); continue; }
        invalidate_mem(be->dev, be->mem, be->off, be->size);
        size_t n = (size_t)be->size;
        if (n > (2u << 20)) n = 2u << 20;
        char path[400];
        snprintf(path, sizeof(path), "%s/vb_%d_%llu_%llu.bin", g_outdir, i,
                 (unsigned long long)be->size, (unsigned long long)r->off[i]);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(p, 1, n, f); fclose(f); LOGI("dumped %s (%zu)", path, n); }
        else LOGE("dump fail %s", path);
        if (p_unmap) p_unmap(be->dev, be->mem);
    }
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
                    if (g_cfg_verbose) LOGI("[A] %08x [B] %08x size=%llu esz=%u", a, b, (unsigned long long)n, esz);
                }
                if (!e->hashed2) {
                    e->hashed2 = 1;
                    uint32_t d = fnv_expand(base, n / esz, esz, 0x811C9DC5u);
                    if (g_cfg_verbose) LOGI("[D] %08x idxs=%zu size=%llu esz=%u", d, n / esz, (unsigned long long)n, esz);
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
                        if (g_cfg_verbose) LOGI("[E] %08x idx=%u first=%u", d2, indexCount, firstIndex);
                    // 验证：head = first 0 / count 28278，命中就让这次 draw 空转
                    if (g_test_mode == 1 && firstIndex == 0 && indexCount == 28278) {
                        static int logged = 0;
                        if (!logged) { logged = 1; LOGI("TEST: skip head draw"); }
                        indexCount = 0;
                    }
                    // —— 模型替换：按 (firstIndex, indexCount) 定位部件 ——
                    if (g_enabled && g_cfg_enable && (g_ib_size == 0 || (int)n == g_ib_size)) {
                        static int dumped = 0;
                        if (!dumped && firstIndex == 71181 && g_cfg_dump) { dumped = 1; dump_vbs(cb); list_bufs(); dump_ubo(cb); }
                        if (g_cfg_probe == 1) probe_shift(cb, 0, 40, 3, 10.0f);   // 位置 +10
                        else if (g_cfg_probe == 2) probe_shift(cb, 1, 20, 2, 0.5f); // UV +0.5
                        int part = -1;
                        for (int pi = 0; pi < 3; pi++)
                            if (firstIndex == PART_FIRST[pi] && indexCount == PART_COUNT[pi]) part = pi;
                        if (part < 0) {
                            static uint32_t seen[16]; static int seen_n = 0;
                            bool dup = false;
                            for (int k = 0; k < seen_n; k++) if (seen[k] == indexCount) dup = true;
                            if (!dup && seen_n < 16) { seen[seen_n++] = indexCount; LOGI("nomatch size=%zu first=%u count=%u", n, firstIndex, indexCount); }
                        }
                        if (part >= 0) {
                            if (g_cfg_mode == 2) {
                                if (gpu_replace(cb, part, ib, e->dev) && g_ib_mod_n[part])
                                    indexCount = (uint32_t)g_ib_mod_n[part];
                            } else {
                                replace_part(cb, part, base, ib, e);
                                if (g_cfg_ib && g_ib_mod_n[part]) indexCount = (uint32_t)g_ib_mod_n[part];
                            }
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
    if (!strcmp(name, "vkUpdateDescriptorSets")) {
        if (!real_update_desc) { real_update_desc = (PFN_vkUpdateDescriptorSets)p; LOGI("intercept vkUpdateDescriptorSets"); }
        return (PFN_vkVoidFunction)ys_vkUpdateDescriptorSets;
    }
    if (!strcmp(name, "vkCmdBindDescriptorSets")) {
        if (!real_bind_desc) { real_bind_desc = (PFN_vkCmdBindDescriptorSets)p; LOGI("intercept vkCmdBindDescriptorSets"); }
        return (PFN_vkVoidFunction)ys_vkCmdBindDescriptorSets;
    }
    if (!strcmp(name, "vkCreateGraphicsPipelines")) {
        if (!real_create_gpipe) { real_create_gpipe = (PFN_vkCreateGraphicsPipelines)p; LOGI("intercept vkCreateGraphicsPipelines"); }
        return (PFN_vkVoidFunction)ys_vkCreateGraphicsPipelines;
    }
    if (!strcmp(name, "vkCmdDraw")) {
        if (!real_cmd_draw) { real_cmd_draw = (PFN_vkCmdDraw)p; LOGI("intercept vkCmdDraw"); }
        return (PFN_vkVoidFunction)ys_vkCmdDraw;
    }
    if (!strcmp(name, "vkCreateShaderModule")) {
        if (!real_create_shader) { real_create_shader = (PFN_vkCreateShaderModule)p; LOGI("intercept vkCreateShaderModule"); }
        return (PFN_vkVoidFunction)ys_vkCreateShaderModule;
    }
    if (!strcmp(name, "vkCreateComputePipelines")) {
        if (!real_create_cpipe) { real_create_cpipe = (PFN_vkCreateComputePipelines)p; LOGI("intercept vkCreateComputePipelines"); }
        return (PFN_vkVoidFunction)ys_vkCreateComputePipelines;
    }
    if (!strcmp(name, "vkCmdBindPipeline")) {
        if (!real_bind_pipe) { real_bind_pipe = (PFN_vkCmdBindPipeline)p; LOGI("intercept vkCmdBindPipeline"); }
        return (PFN_vkVoidFunction)ys_vkCmdBindPipeline;
    }
    if (!strcmp(name, "vkCmdDispatch")) {
        if (!real_dispatch) { real_dispatch = (PFN_vkCmdDispatch)p; LOGI("intercept vkCmdDispatch"); }
        return (PFN_vkVoidFunction)ys_vkCmdDispatch;
    }
    if (!strcmp(name, "vkCreateDevice")) {
        if (!real_create_dev) { real_create_dev = (PFN_vkCreateDevice)p; LOGI("intercept vkCreateDevice"); }
        return (PFN_vkVoidFunction)ys_vkCreateDevice;
    }
    if (!strcmp(name, "vkAllocateMemory")) { if (!real_alloc_mem) real_alloc_mem = (PFN_vkAllocateMemory)p; return p; }
    if (!strcmp(name, "vkGetBufferMemoryRequirements")) { if (!real_get_buf_req) real_get_buf_req = (PFN_vkGetBufferMemoryRequirements)p; return p; }
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties")) { if (!real_get_mem_props) real_get_mem_props = (PFN_vkGetPhysicalDeviceMemoryProperties)p; return p; }
    if (!strcmp(name, "vkCmdCopyBuffer")) {
        if (!real_copy_buf) { real_copy_buf = (PFN_vkCmdCopyBuffer)p; LOGI("intercept vkCmdCopyBuffer"); }
        return p;
    }
    if (!strcmp(name, "vkDestroyBuffer")) {
        if (!real_destroy_buf) { real_destroy_buf = (PFN_vkDestroyBuffer)p; LOGI("intercept vkDestroyBuffer"); }
        return (PFN_vkVoidFunction)ys_vkDestroyBuffer;
    }
    if (!strcmp(name, "vkBindBufferMemory")) {
        if (!real_bind_mem) { real_bind_mem = (PFN_vkBindBufferMemory)p; LOGI("intercept vkBindBufferMemory"); }
        return (PFN_vkVoidFunction)ys_vkBindBufferMemory;
    }
    if (!strcmp(name, "vkCmdBindVertexBuffers")) {
        if (!real_bind_vb) { real_bind_vb = (PFN_vkCmdBindVertexBuffers)p; LOGI("intercept vkCmdBindVertexBuffers"); }
        return (PFN_vkVoidFunction)ys_vkCmdBindVertexBuffers;
    }
    if (!strcmp(name, "vkCmdBindVertexBuffers2") || !strcmp(name, "vkCmdBindVertexBuffers2EXT") ||
        !strcmp(name, "vkCmdBindVertexBuffers2KHR")) {
        if (!real_bind_vb2) { real_bind_vb2 = (PFN_vkCmdBindVertexBuffers2)p; LOGI("intercept %s", name); }
        return (PFN_vkVoidFunction)ys_vkCmdBindVertexBuffers2;
    }
    if (!strcmp(name, "vkFlushMappedMemoryRanges")) { if (!p_flush) p_flush = (PFN_vkFlushMappedMemoryRanges)p; return p; }
    if (!strcmp(name, "vkInvalidateMappedMemoryRanges")) { if (!p_inval) p_inval = (PFN_vkInvalidateMappedMemoryRanges)p; return p; }
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
    if (!strcmp(name, "vkUpdateDescriptorSets")) {
        if (!real_update_desc) { real_update_desc = (PFN_vkUpdateDescriptorSets)p; LOGI("intercept vkUpdateDescriptorSets"); }
        return (PFN_vkVoidFunction)ys_vkUpdateDescriptorSets;
    }
    if (!strcmp(name, "vkCmdBindDescriptorSets")) {
        if (!real_bind_desc) { real_bind_desc = (PFN_vkCmdBindDescriptorSets)p; LOGI("intercept vkCmdBindDescriptorSets"); }
        return (PFN_vkVoidFunction)ys_vkCmdBindDescriptorSets;
    }
    if (!strcmp(name, "vkCreateGraphicsPipelines")) {
        if (!real_create_gpipe) { real_create_gpipe = (PFN_vkCreateGraphicsPipelines)p; LOGI("intercept vkCreateGraphicsPipelines"); }
        return (PFN_vkVoidFunction)ys_vkCreateGraphicsPipelines;
    }
    if (!strcmp(name, "vkCmdDraw")) {
        if (!real_cmd_draw) { real_cmd_draw = (PFN_vkCmdDraw)p; LOGI("intercept vkCmdDraw"); }
        return (PFN_vkVoidFunction)ys_vkCmdDraw;
    }
    if (!strcmp(name, "vkCreateShaderModule")) {
        if (!real_create_shader) { real_create_shader = (PFN_vkCreateShaderModule)p; LOGI("intercept vkCreateShaderModule"); }
        return (PFN_vkVoidFunction)ys_vkCreateShaderModule;
    }
    if (!strcmp(name, "vkCreateComputePipelines")) {
        if (!real_create_cpipe) { real_create_cpipe = (PFN_vkCreateComputePipelines)p; LOGI("intercept vkCreateComputePipelines"); }
        return (PFN_vkVoidFunction)ys_vkCreateComputePipelines;
    }
    if (!strcmp(name, "vkCmdBindPipeline")) {
        if (!real_bind_pipe) { real_bind_pipe = (PFN_vkCmdBindPipeline)p; LOGI("intercept vkCmdBindPipeline"); }
        return (PFN_vkVoidFunction)ys_vkCmdBindPipeline;
    }
    if (!strcmp(name, "vkCmdDispatch")) {
        if (!real_dispatch) { real_dispatch = (PFN_vkCmdDispatch)p; LOGI("intercept vkCmdDispatch"); }
        return (PFN_vkVoidFunction)ys_vkCmdDispatch;
    }
    if (!strcmp(name, "vkCreateDevice")) {
        if (!real_create_dev) { real_create_dev = (PFN_vkCreateDevice)p; LOGI("intercept vkCreateDevice"); }
        return (PFN_vkVoidFunction)ys_vkCreateDevice;
    }
    if (!strcmp(name, "vkAllocateMemory")) { if (!real_alloc_mem) real_alloc_mem = (PFN_vkAllocateMemory)p; return p; }
    if (!strcmp(name, "vkGetBufferMemoryRequirements")) { if (!real_get_buf_req) real_get_buf_req = (PFN_vkGetBufferMemoryRequirements)p; return p; }
    if (!strcmp(name, "vkGetPhysicalDeviceMemoryProperties")) { if (!real_get_mem_props) real_get_mem_props = (PFN_vkGetPhysicalDeviceMemoryProperties)p; return p; }
    if (!strcmp(name, "vkCmdCopyBuffer")) {
        if (!real_copy_buf) { real_copy_buf = (PFN_vkCmdCopyBuffer)p; LOGI("intercept vkCmdCopyBuffer"); }
        return p;
    }
    if (!strcmp(name, "vkDestroyBuffer")) {
        if (!real_destroy_buf) { real_destroy_buf = (PFN_vkDestroyBuffer)p; LOGI("intercept vkDestroyBuffer"); }
        return (PFN_vkVoidFunction)ys_vkDestroyBuffer;
    }
    if (!strcmp(name, "vkBindBufferMemory")) {
        if (!real_bind_mem) { real_bind_mem = (PFN_vkBindBufferMemory)p; LOGI("intercept(i) vkBindBufferMemory"); }
        return (PFN_vkVoidFunction)ys_vkBindBufferMemory;
    }
    if (!strcmp(name, "vkCmdBindVertexBuffers")) {
        if (!real_bind_vb) { real_bind_vb = (PFN_vkCmdBindVertexBuffers)p; LOGI("intercept vkCmdBindVertexBuffers"); }
        return (PFN_vkVoidFunction)ys_vkCmdBindVertexBuffers;
    }
    if (!strcmp(name, "vkCmdBindVertexBuffers2") || !strcmp(name, "vkCmdBindVertexBuffers2EXT") ||
        !strcmp(name, "vkCmdBindVertexBuffers2KHR")) {
        if (!real_bind_vb2) { real_bind_vb2 = (PFN_vkCmdBindVertexBuffers2)p; LOGI("intercept %s", name); }
        return (PFN_vkVoidFunction)ys_vkCmdBindVertexBuffers2;
    }
    if (!strcmp(name, "vkFlushMappedMemoryRanges")) { if (!p_flush) p_flush = (PFN_vkFlushMappedMemoryRanges)p; return p; }
    if (!strcmp(name, "vkInvalidateMappedMemoryRanges")) { if (!p_inval) p_inval = (PFN_vkInvalidateMappedMemoryRanges)p; return p; }
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
Java_com_ysmod_hook_Entry_start(JNIEnv *env, jclass clazz, jstring outDir, jboolean enabled) {
    g_enabled = enabled ? 1 : 0;
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
                else if (!strncmp(line, "flush=", 6)) g_cfg_flush = atoi(line + 6);
                else if (!strncmp(line, "ibsize=", 7)) g_ib_size = atoi(line + 7);
                else if (!strncmp(line, "probe=", 6)) g_cfg_probe = atoi(line + 6);
                else if (!strncmp(line, "mode=", 5)) g_cfg_mode = atoi(line + 5);
                else if (!strncmp(line, "blend=", 6)) g_cfg_blend = atoi(line + 6);
                else if (!strncmp(line, "blendsize=", 10)) g_blend_size = atoi(line + 10);
                else if (!strncmp(line, "verbose=", 8)) g_cfg_verbose = atoi(line + 8);
                else if (!strncmp(line, "dump=", 5)) g_cfg_dump = atoi(line + 5);
                else if (!strncmp(line, "enable=", 7)) g_cfg_enable = atoi(line + 7);
                else if (!strncmp(line, "copypos=", 8)) g_copy_pos = atoi(line + 8);
                else if (!strncmp(line, "copyuv=", 7)) g_copy_uv = atoi(line + 7);
                else if (!strncmp(line, "copyib=", 7)) g_copy_ib = atoi(line + 7);
                else if (!strncmp(line, "copyblend=", 10)) g_copy_blend = atoi(line + 10);
                else if (!strncmp(line, "wprobe=", 7)) g_cfg_wprobe = atoi(line + 7);
                else if (!strncmp(line, "posprobe=", 9)) g_cfg_posprobe = atoi(line + 9);
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
