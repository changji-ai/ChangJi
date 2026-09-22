#include "util/say.hpp"
#include "infer/ggml_abi.hpp"

#if defined(CHANGJI_HAVE_SD) || defined(CHANGJI_HAVE_LLAMA)
#define CHANGJI_HAVE_GGML 1
#include <cstring>

#include "ggml.h"
#endif

namespace changji::infer {

#ifdef CHANGJI_HAVE_GGML

bool ggml_available() { return true; }

GgmlAbi check_ggml_abi() {
    GgmlAbi out;
    out.compiled_max_name = GGML_MAX_NAME;

    ggml_init_params p{};
    p.mem_size = 1024 * 1024;
    p.mem_buffer = nullptr;
    p.no_alloc = true;  // 只要结构体，不要真分配

    ggml_context* ctx = ggml_init(p);
    if (ctx == nullptr) {
        out.detail = SAY("拿不到 ggml context");
        return out;
    }

    ggml_tensor* t = ggml_new_tensor_1d(ctx, GGML_TYPE_F32, 4);
    if (t == nullptr) {
        ggml_free(ctx);
        out.detail = SAY("建不出张量");
        return out;
    }

    // 100 个 'x'：比上游默认的 64 长，比 sd.cpp 要的 160 短。
    // 名字在多少字节处被截断，库就是按多少编的。
    const std::string probe(100, 'x');
    ggml_set_name(t, probe.c_str());
    out.probe_read_back = static_cast<int>(std::strlen(ggml_get_name(t)));
    ggml_free(ctx);

    // **读回来的长度反映的是"库"按多少编的，不是我们按多少编的。**
    // 第一版写成"读回 100 就算过"，结果默认构建报了个
    // "库和本体都是 64"——自相矛盾却是绿的：库其实是 160
    // （sd.cpp 的 add_definitions 只作用于它自己的目录作用域，
    // 加到了 ggml 上、没加到我们的源文件上），我们是 64。
    // 那正是这个探针要抓的情形，却被它自己放过了。
    //
    // 所以要**先从探针反推库的值，再和本体比**。
    const int lib_max_name = out.probe_read_back == 63 ? 64
                             : out.probe_read_back == static_cast<int>(probe.size())
                                 ? 160
                                 : -1;

    if (lib_max_name < 0) {
        out.detail = SAYF("写 100 字符读回 %1，"
                          "这个截断位置对不上任何已知的 GGML_MAX_NAME",
                          std::to_string(out.probe_read_back));
        return out;
    }
    if (lib_max_name == out.compiled_max_name) {
        out.ok = true;
        out.detail = SAYF("库和本体都是 GGML_MAX_NAME=%1（写 100 读回 %2）",
                          std::to_string(out.compiled_max_name),
                          std::to_string(out.probe_read_back));
        return out;
    }
    out.detail = SAYF("库是按 GGML_MAX_NAME=%1 编的，本体是 %2"
                      "——ggml_tensor 的大小两边不一致。现在还没人在本体里直接"
                      "读写张量字段，所以暂时不出事；哪天有人写了，症状会是"
                      "踩内存，而且离原因很远。",
                      std::to_string(lib_max_name),
                      std::to_string(out.compiled_max_name));
    return out;
}

#else

bool ggml_available() { return false; }

GgmlAbi check_ggml_abi() {
    GgmlAbi out;
    out.ok = true;  // 没链就没有这个风险
    out.detail = SAY("没链 ggml");
    return out;
}

#endif

}  // namespace changji::infer
