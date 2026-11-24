// (SMRPG) Lossless Scaling Frame Generation for the port: the separate library (tools/lsfg/lsfg_smrpg.h). What is here
// is the port's part; the rest is from lsfg-vk-android (GameNative, release branch, MIT): its "framegen" and, from its
// layer, extract.cpp's resource table, trans.cpp's translation and the mipmaps shader it ships (mipmaps_replacement) -
// with the DXBC -> SPIR-V translator of DXVK (zlib) and the pe-parse (MIT) it uses. Its license:
// third_party/lsfg-vk-android.
// Nothing leaves here as an exception: the C functions return a code and the reason in err. DXVK's translator reports
// with dxvk::DxvkError, which does not derive from std::exception (a damaged Lossless.dll or one from another version):
// that is why the shaders of the requested mode are all translated at init, and a failure comes out there, with its
// name, and not when creating the context in the middle of the game.
#include "lsfg_smrpg.h"

#include "common/exception.hpp"
#include "lsfg_3_1.hpp"
#include "lsfg_3_1p.hpp"

#include <dxbc_modinfo.h>
#include <dxbc_module.h>
#include <dxbc_reader.h>
#include <pe-parse/parse.h>
#include <thirdparty/spirv.hpp>
#include <util_error.h>

#include <volk.h>  // (its pointers: the one for vkCreateComputePipelines is replaced with pc_create)

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include "mipmaps_replacement_spv.h"

#ifdef _WIN32
#define LSFG_SMRPG_API __declspec(dllexport)
#else
#define LSFG_SMRPG_API __attribute__((visibility("default")))
#endif

namespace {

// (from lsfg-vk src/extract/extract.cpp) the RT_RCDATA resource of Lossless.dll with each shader's DXBC; the "p_" ones,
// performance mode's (LSFG_3_1P asks for them by that name)
const std::unordered_map<std::string, uint32_t> kNameIdx = {{
    {"mipmaps", 255}, {"alpha[0]", 267}, {"alpha[1]", 268}, {"alpha[2]", 269}, {"alpha[3]", 270},
    {"beta[0]", 275}, {"beta[1]", 276}, {"beta[2]", 277}, {"beta[3]", 278}, {"beta[4]", 279},
    {"gamma[0]", 257}, {"gamma[1]", 259}, {"gamma[2]", 260}, {"gamma[3]", 261}, {"gamma[4]", 262},
    {"delta[0]", 257}, {"delta[1]", 263}, {"delta[2]", 264}, {"delta[3]", 265}, {"delta[4]", 266},
    {"delta[5]", 258}, {"delta[6]", 271}, {"delta[7]", 272}, {"delta[8]", 273}, {"delta[9]", 274},
    {"generate", 256},
    {"p_mipmaps", 255}, {"p_alpha[0]", 290}, {"p_alpha[1]", 291}, {"p_alpha[2]", 292}, {"p_alpha[3]", 293},
    {"p_beta[0]", 298}, {"p_beta[1]", 299}, {"p_beta[2]", 300}, {"p_beta[3]", 301}, {"p_beta[4]", 302},
    {"p_gamma[0]", 280}, {"p_gamma[1]", 282}, {"p_gamma[2]", 283}, {"p_gamma[3]", 284}, {"p_gamma[4]", 285},
    {"p_delta[0]", 280}, {"p_delta[1]", 286}, {"p_delta[2]", 287}, {"p_delta[3]", 288}, {"p_delta[4]", 289},
    {"p_delta[5]", 281}, {"p_delta[6]", 294}, {"p_delta[7]", 295}, {"p_delta[8]", 296}, {"p_delta[9]", 297},
    {"p_generate", 256},
}};

std::unordered_map<uint32_t, std::vector<uint8_t>> g_res;     // (the resources read from Lossless.dll)
std::unordered_map<std::string, std::vector<uint8_t>> g_spv;  // (the mode's shaders, already translated)
bool g_perf, g_orig_mipmaps;
// initializeExternal threw halfway in that mode (0 quality, 1 performance): it leaves its instance set with no device,
// its finalize does not remove it and later inits would return without doing anything; only closing the game fixes it
bool g_lib_broken[2];

int on_resource(void *, const peparse::resource &res) {
    if (res.type != peparse::RT_RCDATA || res.buf == nullptr || res.buf->bufLen <= 0) return 0;
    std::vector<uint8_t> d(res.buf->bufLen);
    std::copy_n(res.buf->buf, res.buf->bufLen, d.data());
    g_res[res.name] = std::move(d);
    return 0;
}

// (from lsfg-vk src/extract/trans.cpp) DXBC -> SPIR-V with DXVK's compiler; the bindings, renumbered 0..n-1
struct BindingOffsets {
    uint32_t bindingIndex{}, bindingOffset{}, setIndex{}, setOffset{};
};
std::vector<uint8_t> translate(const std::vector<uint8_t> &bytecode) {
    dxvk::DxbcReader reader(reinterpret_cast<const char *>(bytecode.data()), bytecode.size());
    dxvk::DxbcModule module(reader);
    const dxvk::DxbcModuleInfo info{};
    auto code = module.compile(info, "CS");
    std::vector<BindingOffsets> offs;
    std::vector<uint32_t> ids;
    for (auto ins : code) {
        if (ins.opCode() == spv::OpDecorate) {
            if (ins.arg(2) == spv::DecorationBinding) {
                const uint32_t id = ins.arg(1);
                offs.resize(std::max(offs.size(), size_t(id + 1)));
                offs[id].bindingIndex = ins.arg(3);
                offs[id].bindingOffset = ins.offset() + 3;
                ids.push_back(id);
            }
            if (ins.arg(2) == spv::DecorationDescriptorSet) {
                const uint32_t id = ins.arg(1);
                offs.resize(std::max(offs.size(), size_t(id + 1)));
                offs[id].setIndex = ins.arg(3);
                offs[id].setOffset = ins.offset() + 3;
            }
        }
        if (ins.opCode() == spv::OpFunction) break;
    }
    std::vector<BindingOffsets> valid;
    for (const auto id : ids)
        if (offs[id].bindingOffset) valid.push_back(offs[id]);
    for (size_t i = 0; i < valid.size(); i++) code.data()[valid.at(i).bindingOffset] = static_cast<uint8_t>(i);
    std::vector<uint8_t> spirv(code.size());
    std::copy_n(reinterpret_cast<uint8_t *>(code.data()), code.size(), spirv.data());
    return spirv;
}
// (the translator's error, into one that does derive from std::exception and names the shader)
std::vector<uint8_t> translate_named(const std::string &name, const std::vector<uint8_t> &bytecode) {
    try {
        return translate(bytecode);
    } catch (const dxvk::DxvkError &e) {
        throw std::runtime_error("the shader " + name + " of Lossless.dll could not be translated (" + e.message() +
                                 "): is Lossless.dll damaged or from another version?");
    }
}

bool is_mipmaps(const std::string &name) { return name == "mipmaps" || name == "p_mipmaps"; }

std::vector<uint8_t> load_shader(const std::string &name) {
    if (!g_orig_mipmaps && is_mipmaps(name))
        return std::vector<uint8_t>(mipmaps_replacement_spv, mipmaps_replacement_spv + mipmaps_replacement_spv_len);
    auto spv = g_spv.find(name);
    if (spv != g_spv.end()) return spv->second;
    auto hit = kNameIdx.find(name);
    if (hit == kNameIdx.end()) throw std::runtime_error("unknown shader: " + name);
    auto res = g_res.find(hit->second);
    if (res == g_res.end()) throw std::runtime_error("missing shader " + name + " in Lossless.dll");
    return translate_named(name, res->second);
}

void set_err(char *err, int len, const std::string &msg) {
    if (err && len > 0) snprintf(err, (size_t)len, "%s", msg.c_str());
}

// ---- pipeline cache. LSFG creates its own pipelines (one per shader: about 20 compute ones) without a cache, when
// creating the first context after init, and on the AYN Thor's Adreno that took 0.75-1.2 s: the stall on turning it on
// in every session. After its initializeExternal (which loads volk's device pointers, also the ones of every later
// init), its vkCreateComputePipelines becomes pc_create, which gives them a VkPipelineCache read from cache_path if its
// header is from this GPU and this driver; it is saved if it changed after creating each context, and closed in
// finalize.
PFN_vkCreateComputePipelines g_ccp_orig;
VkPipelineCache g_pc;
VkDevice g_pc_dev;
std::string g_pc_path;
size_t g_pc_saved;
constexpr size_t kPcMax = 64u << 20;  // (more than this in the file: damaged or something else's; start from scratch)

VKAPI_ATTR VkResult VKAPI_CALL pc_create(VkDevice dev, VkPipelineCache cache, uint32_t n, const VkComputePipelineCreateInfo *ci,
                                         const VkAllocationCallbacks *alloc, VkPipeline *out) {
    if (!cache && dev == g_pc_dev) cache = g_pc;
    return g_ccp_orig(dev, cache, n, ci, alloc, out);
}

// the header (VkPipelineCacheHeaderVersionOne) is from this GPU and this driver
bool pc_header_ok(const std::vector<uint8_t> &d, VkPhysicalDevice phys) {
    if (d.size() < 16 + VK_UUID_SIZE || !vkGetPhysicalDeviceProperties) return false;
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(phys, &p);
    uint32_t h[4];
    memcpy(h, d.data(), sizeof h);  // header size, version, vendorID, deviceID
    return h[0] >= 16 + VK_UUID_SIZE && h[0] <= d.size() && h[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
           h[2] == p.vendorID && h[3] == p.deviceID && !memcmp(d.data() + 16, p.pipelineCacheUUID, VK_UUID_SIZE);
}

void pc_save() {
    if (!g_pc || !vkGetPipelineCacheData) return;
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_pc_dev, g_pc, &sz, nullptr) != VK_SUCCESS || !sz || sz > kPcMax || sz == g_pc_saved) return;
    std::vector<uint8_t> d(sz);
    if (vkGetPipelineCacheData(g_pc_dev, g_pc, &sz, d.data()) != VK_SUCCESS) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(g_pc_path).parent_path(), ec);
    const std::string tmp = g_pc_path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) return;
    bool ok = fwrite(d.data(), 1, sz, f) == sz;
    ok = fclose(f) == 0 && ok;
    if (ok) {
        remove(g_pc_path.c_str());  // (Windows rename does not overwrite; a cut here loses the cache, never damages it)
        ok = rename(tmp.c_str(), g_pc_path.c_str()) == 0;
    }
    if (ok) g_pc_saved = sz;
    else remove(tmp.c_str());
}

void pc_close() {
    if (vkCreateComputePipelines == pc_create) vkCreateComputePipelines = g_ccp_orig;
    if (!g_pc) return;
    pc_save();
    if (vkDestroyPipelineCache) vkDestroyPipelineCache(g_pc_dev, g_pc, nullptr);
    g_pc = VK_NULL_HANDLE;
    g_pc_dev = VK_NULL_HANDLE;
}

void pc_open(const LsfgSmrpgInit *in) {
    if (!in->cache_path || !*in->cache_path || !vkCreateComputePipelines || !vkCreatePipelineCache) return;
    if (g_pc && (g_pc_dev != in->device || g_pc_path != in->cache_path)) pc_close();
    if (!g_pc) {
        std::vector<uint8_t> d;
        if (FILE *f = fopen(in->cache_path, "rb")) {
            if (fseek(f, 0, SEEK_END) == 0) {
                long n = ftell(f);
                if (n > 0 && (size_t)n <= kPcMax && fseek(f, 0, SEEK_SET) == 0) {
                    d.resize((size_t)n);
                    if (fread(d.data(), 1, d.size(), f) != d.size()) d.clear();
                }
            }
            fclose(f);
        }
        if (!pc_header_ok(d, in->phys)) d.clear();
        VkPipelineCacheCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
        ci.initialDataSize = d.size();
        ci.pInitialData = d.empty() ? nullptr : d.data();
        if (vkCreatePipelineCache(in->device, &ci, nullptr, &g_pc) != VK_SUCCESS) {
            ci.initialDataSize = 0;  // (data the driver does not accept: an empty one)
            ci.pInitialData = nullptr;
            if (vkCreatePipelineCache(in->device, &ci, nullptr, &g_pc) != VK_SUCCESS) g_pc = VK_NULL_HANDLE;
        }
        if (!g_pc) return;
        g_pc_dev = in->device;
        g_pc_path = in->cache_path;
        g_pc_saved = d.size();
    }
    if (vkCreateComputePipelines != pc_create) {
        g_ccp_orig = vkCreateComputePipelines;
        vkCreateComputePipelines = pc_create;
    }
}

}  // namespace

extern "C" {

LSFG_SMRPG_API int lsfg_smrpg_abi(void) { return LSFG_SMRPG_ABI; }

LSFG_SMRPG_API int lsfg_smrpg_init(const LsfgSmrpgInit *in, char *err, int errlen) {
    try {
        if (!in || !in->dll_path || !in->gipa || in->generations < 1 || in->flow_scale <= 0) {
            set_err(err, errlen, "invalid parameters");
            return -1;
        }
        const int mode = in->performance != 0;
        if (g_lib_broken[mode]) {
            set_err(err, errlen, "it already failed to start in this mode and cannot be started again: close the game "
                                 "and open it again");
            return -5;
        }
        g_res.clear();
        g_spv.clear();
        peparse::parsed_pe *dll = peparse::ParsePEFromFile(in->dll_path);
        if (!dll) {
            set_err(err, errlen, std::string("could not read ") + in->dll_path);
            return -2;
        }
        peparse::IterRsrc(dll, on_resource, nullptr);
        peparse::DestructParsedPE(dll);
        for (const auto &[name, idx] : kNameIdx)
            if (g_res.find(idx) == g_res.end()) {
                set_err(err, errlen, "Lossless.dll lacks the shader " + name + ": is Lossless Scaling up to "
                                                                                       "date?");
                return -3;
            }
        g_perf = mode != 0;
        g_orig_mipmaps = in->orig_mipmaps != 0;
        for (const auto &[name, idx] : kNameIdx) {
            if ((name.rfind("p_", 0) == 0) != g_perf || (!g_orig_mipmaps && is_mipmaps(name))) continue;
            g_spv[name] = translate_named(name, g_res.at(idx));
        }
        try {
            if (g_perf)
                LSFG_3_1P::initializeExternal(in->gipa, in->instance, in->phys, in->device, in->queue_family, in->queue,
                                              in->hdr != 0, 1.0f / in->flow_scale, (uint64_t)in->generations, load_shader);
            else
                LSFG_3_1::initializeExternal(in->gipa, in->instance, in->phys, in->device, in->queue_family, in->queue,
                                             in->hdr != 0, 1.0f / in->flow_scale, (uint64_t)in->generations, load_shader);
        } catch (...) {
            g_lib_broken[mode] = true;
            throw;
        }
        pc_open(in);  // (afterward: its volkLoadDevice has just set the device's pointers)
        return 0;
    } catch (const std::exception &e) {
        set_err(err, errlen, e.what());
        return -4;
    } catch (const dxvk::DxvkError &e) {
        set_err(err, errlen, e.message());
        return -4;
    } catch (...) {
        set_err(err, errlen, "unknown error at startup");
        return -4;
    }
}

LSFG_SMRPG_API int lsfg_smrpg_create(const VkImage in[2], const VkImage *out, int n, uint32_t w, uint32_t h, VkFormat fmt,
                                     char *err, int errlen) {
    try {
        std::vector<VkImage> outs(out, out + n);
        VkExtent2D ext{w, h};
        int ctx = g_perf ? LSFG_3_1P::createContextFromImages(in[0], in[1], outs, ext, fmt)
                         : LSFG_3_1::createContextFromImages(in[0], in[1], outs, ext, fmt);
        if (ctx >= 0) pc_save();  // (its pipelines exist by now: if the cache changed, to disk)
        return ctx;
    } catch (const std::exception &e) {
        set_err(err, errlen, e.what());
        return -1;
    } catch (const dxvk::DxvkError &e) {
        set_err(err, errlen, e.message());
        return -1;
    } catch (...) {
        set_err(err, errlen, "unknown error creating the context");
        return -1;
    }
}

LSFG_SMRPG_API int lsfg_smrpg_present(int ctx, char *err, int errlen) {
    try {
        std::vector<int> none;
        if (g_perf) LSFG_3_1P::presentContext(ctx, -1, none);
        else LSFG_3_1::presentContext(ctx, -1, none);
        return 0;
    } catch (const LSFG::vulkan_error &e) {
        set_err(err, errlen, e.what());
        return e.error() == VK_TIMEOUT ? -2 : -1;
    } catch (const std::exception &e) {
        set_err(err, errlen, e.what());
        return -1;
    } catch (const dxvk::DxvkError &e) {
        set_err(err, errlen, e.message());
        return -1;
    } catch (...) {
        set_err(err, errlen, "unknown error while generating");
        return -1;
    }
}

LSFG_SMRPG_API void lsfg_smrpg_destroy(int ctx) {
    try {
        if (g_perf) LSFG_3_1P::deleteContext(ctx);
        else LSFG_3_1::deleteContext(ctx);
    } catch (...) {
    }
}

LSFG_SMRPG_API void lsfg_smrpg_finalize(void) {
    try {
        pc_close();  // (before LSFG: saves the cache and leaves its vkCreateComputePipelines as it was)
        LSFG_3_1P::finalize();
        LSFG_3_1::finalize();
    } catch (...) {
    }
    g_res.clear();
    g_spv.clear();
}

}  // extern "C"
