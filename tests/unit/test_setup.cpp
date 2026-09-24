// 首次运行那一页：模型清单、推荐、写回配置、进度解析。
//
// Python 侧没有对应物（那边模型是 ComfyUI 管的），所以不是对拍，是新逻辑。
//
// 盯五件事，每一件都是"错了不会立刻报错"的那一类：
//
//   **清单本身自洽**——角色名必须在 models_field 那张表里认得出来。
//   漏一个的表现是"下完了但配置里没写上"，而那要到出片时才报"模型没配"。
//
//   **推荐永远给得出答案**——界面上"没有默认值"等于让用户自己去猜。
//
//   **换家族要清空上一家的键**——`video_lora` 的默认值指着 H3 的 Turbo
//   LoRA，选了 Wan 之后还留着的话，sd.cpp 会把一个给 H3 做的 LoRA 挂到
//   Wan 上。加载不上而已，画面照出、耗时照旧，没有任何报错。
//
//   **aria2 的进度行**——解析错了的表现是进度条一直停在 0，而下载本身
//   是好的，很难往解析上想。
//
//   **配齐了没有**——判错的两个方向都很难看：判松了用户进首页点什么都跑
//   不了，判紧了用云端大模型的人被永远挡在这一页上。

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "config/model_index.hpp"
#include "config/settings.hpp"
#include "http/setup_api.hpp"
#include "setup/autostart.hpp"
#include "setup/update_check.hpp"
#include "setup/catalog.hpp"
#include "setup/downloader.hpp"
#include "setup/source.hpp"
#include "util/paths.hpp"

#include "scoped_env.hpp"

using namespace changji;
using json = nlohmann::json;

TEST_CASE("清单里每个角色名都要能落到 [models] 的某个字段上") {
    config::ModelsConfig m;
    for (const auto& g : setup::catalog()) {
        for (const auto& role : g.owned_roles) {
            CAPTURE(g.key);
            CAPTURE(role);
            CHECK(config::models_field(m, role) != nullptr);
        }
        for (const auto& o : g.options) {
            for (const auto& f : o.files) {
                if (f.role.empty()) continue;
                CAPTURE(o.id);
                CAPTURE(f.role);
                // 文件声明的角色必须是这一组管的那几个之一。不是的话
                // 写回时不会被清空，换选项之后会留在配置里。
                const bool owned = std::find(g.owned_roles.begin(),
                                             g.owned_roles.end(),
                                             f.role) != g.owned_roles.end();
                CHECK(owned);
                CHECK(config::models_field(m, f.role) != nullptr);
            }
        }
    }
}

TEST_CASE("每个文件都有仓库、路径和核对过的字节数") {
    // **字节数是下载完成的唯一判据。** 填 0 的话那个文件永远"下完了"，
    // 而截断的权重加载时报的是"读不对"，指向完全错误的方向。
    for (const auto& g : setup::catalog()) {
        for (const auto& o : g.options) {
            if (o.id == setup::kNoneOption) {
                CHECK(o.files.empty());
                continue;
            }
            // **判据是"下不下文件"，不是"是不是 kNoneOption"。**
            // 走云端 API 那一项也一个文件都不下，它不是"什么都不装"，
            // 是一个完整可用的选择——见 catalog.cpp 里 glm-4.7-flash-api。
            if (o.files.empty()) continue;
            CAPTURE(o.id);
            for (const auto& f : o.files) {
                CAPTURE(f.name);
                CHECK_FALSE(f.name.empty());
                CHECK_FALSE(f.repo.empty());
                CHECK_FALSE(f.path.empty());
                CHECK(f.bytes > 0);
                // 仓库名形如 `组织/名字`，路径不带前导斜杠——
                // 两个源的地址都是这么拼的，错了就是 404，
                // 而 404 在下载器眼里和"网络不通"长得一样。
                CHECK(f.repo.find('/') != std::string::npos);
                CHECK(f.path.front() != '/');
            }
            CHECK(o.total_bytes() > 0);
        }
    }
}

TEST_CASE("下载地址：国内魔搭、国外 HuggingFace") {
    // 三个源的仓库名和路径完全一致，只有域名和分支名不同
    // （魔搭是 master 而且多一段 /models）。拼错了就是 404。
    const std::string repo = "QuantStack/Qwen-Image-Edit-2509-GGUF";
    const std::string path = "Qwen-Image-Edit-2509-Q6_K.gguf";
    CHECK(setup::resolve_url(setup::Source::ModelScope, repo, path) ==
          "https://modelscope.cn/models/QuantStack/Qwen-Image-Edit-2509-GGUF/"
          "resolve/master/Qwen-Image-Edit-2509-Q6_K.gguf");
    CHECK(setup::resolve_url(setup::Source::HuggingFace, repo, path) ==
          "https://huggingface.co/QuantStack/Qwen-Image-Edit-2509-GGUF/"
          "resolve/main/Qwen-Image-Edit-2509-Q6_K.gguf");
    CHECK(setup::resolve_url(setup::Source::HfMirror, repo, path) ==
          "https://hf-mirror.com/QuantStack/Qwen-Image-Edit-2509-GGUF/"
          "resolve/main/Qwen-Image-Edit-2509-Q6_K.gguf");
    // 带子目录的路径原样接上，不做任何转义
    CHECK(setup::resolve_url(setup::Source::ModelScope, "Comfy-Org/MiniMax-H3",
                             "vae/minimax_h3_audio_vae_fp32.safetensors") ==
          "https://modelscope.cn/models/Comfy-Org/MiniMax-H3/resolve/master/"
          "vae/minimax_h3_audio_vae_fp32.safetensors");
    // Auto 还没探的时候当魔搭用——调用方本该先 detect_source()，
    // 真漏了的话给一条能用的路比拼出个空串好。
    CHECK(setup::resolve_url(setup::Source::Auto, repo, path) ==
          setup::resolve_url(setup::Source::ModelScope, repo, path));
}

TEST_CASE("源的名字认得出来，认不出的返回空") {
    CHECK(setup::source_from_string("modelscope") == setup::Source::ModelScope);
    CHECK(setup::source_from_string("huggingface") == setup::Source::HuggingFace);
    CHECK(setup::source_from_string("hf-mirror") == setup::Source::HfMirror);
    CHECK(setup::source_from_string("auto") == setup::Source::Auto);
    // **认不出必须是空，不能悄悄退回默认值**：前端传了个拼错的源，
    // 静默换成别的会让人以为自己选的生效了。
    CHECK_FALSE(setup::source_from_string("魔塔").has_value());
    CHECK_FALSE(setup::source_from_string("").has_value());
    // 转回去要能对上，前端拿这个字符串当选择器的值
    for (const auto s : {setup::Source::ModelScope, setup::Source::HuggingFace,
                         setup::Source::HfMirror, setup::Source::Auto}) {
        CHECK(setup::source_from_string(setup::to_string(s)) == s);
    }
}

TEST_CASE("选项 id 在整张表里唯一") {
    // 重了的话 `Group::find` 会挑到先出现的那个，而界面上两项长得不一样。
    std::set<std::string> seen;
    for (const auto& g : setup::catalog()) {
        for (const auto& o : g.options) {
            if (o.id == setup::kNoneOption) continue;  // 每组一个，本来就重
            CAPTURE(o.id);
            CHECK(seen.insert(o.id).second);
        }
    }
}

TEST_CASE("推荐：显存多大都得给得出一套") {
    for (const double vram : {0.0, 6.0, 8.0, 12.0, 16.0, 24.0, 32.0, 48.0, 80.0}) {
        CAPTURE(vram);
        const auto rec = setup::recommend(vram);
        for (const auto& g : setup::catalog()) {
            CAPTURE(g.key);
            const auto it = rec.find(g.key);
            REQUIRE(it != rec.end());
            // **不能推荐"不下载"。** 那一项是给人手动挑的，
            // 推荐成默认值等于默认什么都不装。
            CHECK(it->second != setup::kNoneOption);
            CHECK(g.find(it->second) != nullptr);
        }
    }
}

TEST_CASE("推荐：卡越大挑得越好，而且不会推荐装不下的") {
    // **别把这两个叫 small / big。** Windows SDK 的 rpcndr.h 里有
    // `#define small char`，`const auto small` 在 MSVC 上直接编不过
    // （C3530「auto 不能与任何其他类型说明符组合」，指向的行看着完全正常）。
    const auto small_card = setup::recommend(8.0);
    const auto big_card = setup::recommend(80.0);
    CHECK(small_card.at("image") != big_card.at("image"));

    for (const auto& g : setup::catalog()) {
        const auto* picked = g.find(setup::recommend(24.0).at(g.key));
        REQUIRE(picked != nullptr);
        CAPTURE(g.key);
        CAPTURE(picked->id);
        // 编剧那一组的预算只有六成，别的按整张卡算
        CHECK(picked->min_vram_gb <= (g.key == "llm" ? 24.0 * 0.6 : 24.0));
    }
    const auto rec5090 = setup::recommend(31.8);
    // **推荐 = 这张卡上权重能常驻的最好一档。** 5090 上 Qwen-Image-Edit
    // Q6_K 正好是那一档：实测权重常驻 35 秒一张，再往上 Q8_0（21.8 GB）
    // 在这张卡上只能放内存，慢五倍。
    CHECK(rec5090.at("image") == "qwen-image-edit-2509-q6_k");
    // 出片这一组同理：完整版 H3 的权重要 33.5 GB 才常驻得下，这张卡差
    // 一点点，所以推荐落在精简版上。**完整版没被禁掉**，只是不当默认值
    // ——用户这台机器上现在跑的就是完整版（权重放内存，实测 124 秒一镜），
    // 那个选择由配置里的「正在用」保住，不会被推荐值顶掉。
    //
    // **2026-09-17 从 Q4_K_M 升到 Q6_K**：用户要「增加全部档位」，精简那
    // 一支补进了 Q5_0 / Q6_K / Q8_0（仓库里真有的那几档）。在这之前
    // Q4_K_M(26.5 GB) 和 bf16(55 GB) 之间是空的，这张卡只能将就 Q4_K_M。
    // 现在 Q6_K 门槛 31.5 GB，**在 31.8 GB 上刚好够**，按上面出图那一组
    // 同一个原则（挑权重能常驻的最好一档）它就该是这一档。
    //
    // ⚠️ 余量只有 0.3 GB。门槛是引擎自己那套 `weights_for` 算的，不是这儿
    // 估的——真在 5090 上跑爆了，要改的是那套策略的留量，不是把这一档从
    // 清单里摘掉。
    //
    // **这一条同时钉着 rank 的排法。** 给完整那一支补上量化档之后它先挂过
    // 一次：完整的 rank 整段高于精简，于是推荐落到了 `h3-full-q3_k_m`
    //（门槛 30.5 GB，塞得下），一个严重退化的完整版压过了高精度的精简版。
    // 而完整多出来的 13B 推理时根本不加载——同一量化级别上它不更好，只是
    // 更大。rank 因此改成按量化级别排、同级别里精简在前。
    CHECK(rec5090.at("video") == "h3-pruned-q6_k");
    // **编剧那一组默认推本地**（2026-09-19 改回来的）。这是产品要给别人用
    // 的：图片、视频、配音全在本地，编剧不该是唯一要人自己去领密钥的一环。
    // 2026-09-13 到 09-19 之间这儿推的是云端那一项（本地最高 59 分对云上
    // 81.8，而且要占走整张卡）——那两笔账没变，变的是"默认要能跑通"压过了
    // 它们。云端那一项还在，只在一档本地的都装不下时才轮到。
    const auto* llm_group = &setup::catalog().front();
    REQUIRE(llm_group->key == "llm");
    const auto family_of = [&](double vram) {
        const std::string id = setup::recommend(vram).at("llm");
        const setup::Option* o = llm_group->find(id);
        REQUIRE(o != nullptr);
        return o->family;
    };
    // 8 GB：六成预算 4.8 GB，连 4B 那档（门槛 7 GB）都装不下，退回云端
    CHECK(setup::recommend(8.0).at("llm") == "zhipu-free");
    // 5090（31.8 GB）：14B 那档是实测过的锚点，见 llm_family_note
    CHECK(family_of(31.8) == "Qwen3-14B");
    // 80 GB：本机能跑的里最大的
    CHECK(family_of(80.0) == "Qwen3-32B");
    // 24 GB：具体落在哪档由门槛算，但一定是本地的一档
    {
        const std::string id = setup::recommend(24.0).at("llm");
        const setup::Option* o = llm_group->find(id);
        REQUIRE(o != nullptr);
        CHECK_FALSE(o->files.empty());
        CHECK(o->min_vram_gb <= 24.0 * 0.6);
    }
    // 本地那几档回来了，每一档都真有文件、字节数非零、选了就切到进程内
    CHECK(llm_group->find("qwen3-14b-q4_k_m") != nullptr);
    CHECK(llm_group->find("qwen3-32b-q8_0") != nullptr);
    CHECK(llm_group->required);
    for (const auto& o : llm_group->options) {
        CAPTURE(o.id);
        if (o.id == "zhipu-free" || o.id == setup::kNoneOption) {
            CHECK(o.files.empty());
            continue;
        }
        REQUIRE(o.files.size() == 1);
        CHECK(o.files.front().bytes > 0);
        CHECK(o.files.front().role == "llm");
        bool local = false;
        for (const auto& [k, v] : o.settings) {
            if (k == "llm.backend" && v == "local") local = true;
        }
        CHECK(local);
    }
}

TEST_CASE("显存门槛是从引擎那两个函数反推的，不是手填的") {
    // **这条钉的是"表和引擎不许分家"。** 界面上写着"≥ 24 GB 权重可常驻"，
    // 而真正决定权重放哪的是 `ModelsConfig::weights_for` /
    // `image_weights_for`。手填一份的话，那两个函数里的常数一调
    // （5090 上量出来的 14.6 / 6.6 + 4，已经改过好几轮），
    // 这张表就开始骗人——**而"说装得下、实际 OOM"没有任何报错**，
    // 只会在跑到第 34 段时炸。
    config::ModelsConfig m;
    for (const auto& g : setup::catalog()) {
        if (g.key != "video" && g.key != "image") continue;  // 大模型那条另算
        const bool image = g.key == "image";
        for (const auto& o : g.options) {
            if (o.id == setup::kNoneOption) continue;
            // 扩散模型是这一组文件里挂在主角色上的那一个
            double model_gb = 0;
            for (const auto& f : o.files) {
                if (f.role == g.owned_roles.front()) {
                    model_gb = static_cast<double>(f.bytes) / 1e9;
                }
            }
            REQUIRE(model_gb > 0);
            CAPTURE(o.id);
            CAPTURE(model_gb);
            CAPTURE(o.min_vram_gb);
            const auto at = [&](double v) {
                return image ? m.image_weights_for(v, model_gb)
                             : m.weights_for(v, model_gb);
            };
            // 门槛上：权重常驻。门槛下半档：退回全放内存。
            CHECK(at(o.min_vram_gb) != "cpu");
            CHECK(at(o.min_vram_gb - 0.5) == "cpu");
        }
    }
}

TEST_CASE("每个家族的每一档精度都在表里") {
    // 用户 2026-09-10 的原话："将所有精度的模型都显示出来"。
    // 少一档不会有任何报错，只是那一档在界面上不存在。
    std::map<std::string, std::set<std::string>> by_family;
    for (const auto& g : setup::catalog()) {
        for (const auto& o : g.options) {
            if (o.id == setup::kNoneOption) continue;
            CAPTURE(o.id);
            CHECK_FALSE(o.family.empty());
            CHECK_FALSE(o.family_note.empty()); // 界面按家族显示这一句
            // **量化档和显存门槛只有本地权重才有。** 云端那一项没有权重，
            // 也就没有"哪个精度""要多少显存"可言——它的门槛正好是 0，
            // 这也是 recommend() 在任何一张卡上都挑得动它的原因。
            if (o.files.empty()) continue;
            CHECK_FALSE(o.quant.empty());       // 每一档都要说清是哪个精度
            CHECK(o.min_vram_gb > 0);
            by_family[o.family].insert(o.quant);
        }
    }
    // QuantStack 那个仓库的 13 档。**这一族没有 BF16 也没有 fp8**——
    // 上游只放了 GGUF 梯队，2026-09-15 从基础版 Qwen-Image 换过来时
    // 那两档就没了（基础版在 city96 有 14 档 + Comfy 的 fp8）。
    CHECK(by_family.at("Qwen-Image-Edit 2509").size() == 13);
    // **Wan 2.2 TI2V-5B 2026-09-17 从清单里去掉了**（用户要求）：它出的
    // 成片用户看过，原话是「图生视频还是太差了」，留着只会让人下三个 G 再
    // 得出同一个结论。引擎那头认 Wan 权重的代码没动，已经配了的照跑。
    CHECK(by_family.count("Wan 2.2 TI2V-5B") == 0);
    // 出片只剩 H3 两支。完整 9 档、精简 9 档，两支都是
    // bf16 / Q8_0 / int8_convrot(/fp8_scaled) / Q6_K / Q5* / Q4* / Q3* (/Q2_K)。
    CHECK(by_family.at("MiniMax-H3 完整").size() == 9);
    CHECK(by_family.at("MiniMax-H3 精简").size() == 9);
    // 编剧模型那一组现在一个权重都不下（进程内后端删了），所以这儿
    // 不再有 Qwen3-* 那几家。
}

TEST_CASE("写回配置：选中的键填上，同一组没用到的键清空") {
    // H3 用 video_llm 当编码器、还要 video_audio_vae；
    // Wan 用 video_text_encoder，两个都不要。
    const json h3 = setup::config_patch({{"video", "h3-full-q4_k_m"}});
    CHECK(h3["models"]["video"] == "minimax_h3_fl2va-Q4_K_M.gguf");
    CHECK(h3["models"]["video_llm"] == "qwen3vl_32b_minimax_h3-Q4_K_M.gguf");
    CHECK(h3["models"]["video_audio_vae"] ==
          "minimax_h3_audio_vae_fp32.safetensors");
    CHECK(h3["models"]["video_text_encoder"] == "");
    CHECK(h3["models"]["video_high_noise"] == "");
    // H3 的三个旋钮和 Wan 完全不同，填错了都不报错
    CHECK(h3["models"]["video_rng"] == "cpu");
    CHECK(h3["models"]["video_cfg"] == 1.0);

    // **「不下载」那一档接着管这组用例的重点。** Wan 去掉之后出片只剩 H3
    // 一家，而 `owned_roles` 那八个键要清空这件事一点没变：从 H3 换到
    // 「不下载」时，video_lora 里留着的 Turbo LoRA 会被挂到人家自己手配的
    // 模型上——加载不上而已，画面照出、耗时照旧，没有任何报错。
    const json none = setup::config_patch({{"video", setup::kNoneOption}});
    CHECK_FALSE(none.contains("models"));

    // int8_convrot 那两档走的是 safetensors，不是 GGUF，但同一组的键该填
    // 的填、该清的清，和上面那档一模一样。
    const json cr = setup::config_patch({{"video", "h3-pruned-int8_convrot"}});
    CHECK(cr["models"]["video"] ==
          "minimax_h3_fl2va_pruned_int8_convrot.safetensors");
    CHECK(cr["models"]["video_llm"] != "");
    CHECK(cr["models"]["video_text_encoder"] == "");
    CHECK(cr["models"]["video_high_noise"] == "");
}

TEST_CASE("写回配置：步数一律写 0，交给引擎按 Turbo 和档位表自己算") {
    // **写死步数会把首帧一起钉死。** `[tiers].final_steps` 会落进档位表
    // （Runtime::profile），而首帧的步数取的就是档位表那个数
    // （effective_spec 里 frame_steps = table_final_steps）。Turbo 那个 LoRA
    // 只挂在视频模型上，出图那一步没有它——6 步就是裸跑 6 步，首帧糊，
    // 而首帧是跨镜头一致性的锚点，糊了后面每一镜都糊，全程不报错。
    //
    // 2026-09-10 在这一版上真栽过：设置页从「出片 6 Turbo · 首帧 30」
    // 变成了「出片 6 Turbo · 首帧 6」。
    for (const char* id : {"h3-full-q4_k_m", "h3-pruned-q4_k_m",
                           "h3-full-int8_convrot", "h3-pruned-q2_k"}) {
        CAPTURE(id);
        const json patch = setup::config_patch({{"video", id}});
        REQUIRE(patch["tiers"].contains("final_steps"));
        CHECK(patch["tiers"]["final_steps"] == 0);
    }
}

TEST_CASE("写回配置：不下载那一档只写旋钮，不动模型路径") {
    const json patch = setup::config_patch({{"llm", setup::kNoneOption}});
    CHECK(patch["llm"]["backend"] == "remote");
    // 用户可能本来就手配了一套模型，只是这次不想重下。
    CHECK_FALSE(patch.contains("models"));
}

TEST_CASE("写回配置：云端那一项连地址和模型名一起写，也不动模型路径") {
    const json patch = setup::config_patch({{"llm", "zhipu-free"}});
    CHECK(patch["llm"]["backend"] == "remote");
    CHECK(patch["llm"]["base_url"] == "https://open.bigmodel.cn/api/paas/v4");
    CHECK(patch["llm"]["model"] == "glm-4.7-flash");
    // **它不是 kNoneOption，但一个文件都不下**，所以同样不该清 models.llm
    // ——盘上那个权重还在，清了的话用户想切回本地得重新去找它叫什么。
    CHECK_FALSE(patch.contains("models"));
}

TEST_CASE("写回配置：认不出的选项跳过那一组，不炸") {
    const json patch = setup::config_patch(
        {{"video", "这个选项早就删了"}, {"image", "qwen-image-edit-2509-q6_k"}});
    CHECK_FALSE(patch["models"].contains("video"));
    CHECK(patch["models"]["image"] == "Qwen-Image-Edit-2509-Q6_K.gguf");
}

TEST_CASE("apply_setup_patch 把 patch 落到内存里那份配置上") {
    config::Settings s;
    config::apply_setup_patch(s, setup::config_patch({{"video", "h3-full-q4_k_m"}}));
    CHECK(s.models.video == "minimax_h3_fl2va-Q4_K_M.gguf");
    CHECK(s.models.video_llm == "qwen3vl_32b_minimax_h3-Q4_K_M.gguf");
    CHECK(s.models.video_rng == "cpu");
    CHECK(s.models.video_cfg == doctest::Approx(1.0));
    // 0 = 没填，按显存推的档位表走。见上面那条用例。
    CHECK(s.tiers.final_steps == 0);

    // 再换一档，上一家的键必须真的被清掉——只清 patch 不清内存的话，
    // 界面显示的是新的、跑的是旧的。
    //
    // **2026-09-17 从 Wan 换成 int8_convrot 这一档**（Wan 从清单里去掉了）。
    // 这一档走 safetensors 不走 GGUF，正好也钉住换格式时路径真的跟着换。
    config::apply_setup_patch(
        s, setup::config_patch({{"video", "h3-pruned-int8_convrot"}}));
    CHECK(s.models.video == "minimax_h3_fl2va_pruned_int8_convrot.safetensors");
    CHECK(s.models.video_text_encoder.empty());
    CHECK(s.models.video_high_noise.empty());

    config::apply_setup_patch(s, json{{"llm", {{"backend", "remote"}}}});
    CHECK(s.llm.backend == "remote");

    // **地址和模型名也要落进内存，不能只写文件。** 只落 backend 的话，
    // 选了云端那一项之后文件里是智谱、内存里还是上一家，写剧本仍然发往
    // 上一家，要等重启才"自己好了"。
    config::apply_setup_patch(s, setup::config_patch({{"llm", "zhipu-free"}}));
    CHECK(s.llm.base_url == "https://open.bigmodel.cn/api/paas/v4");
    CHECK(s.llm.model == "glm-4.7-flash");
}

TEST_CASE("现在用的是哪一项：外接大模型按地址认，不按模型名") {
    // 2026-09-14 的 bug 就在这条判据上。按模型名认的话，用户在设置页把
    // glm-4.7-flash 换成 glm-5.3 那一刻，这一组就"认不出是哪一家"：
    // 初始化页显示成「不下载 · 用别的外接服务」，而下一次保存会把
    // zhipu-free 那一项写死的 glm-4.7-flash 冲回配置文件——
    // **他自己挑的模型名存不住**。
    const auto* llm = &setup::catalog().front();
    REQUIRE(llm->key == "llm");

    config::Settings s;
    s.llm.backend = "remote";
    s.llm.base_url = "https://open.bigmodel.cn/api/paas/v4";

    SUBCASE("就是那一项默认的模型名") {
        s.llm.model = "glm-4.7-flash";
        CHECK(http::current_option(*llm, s) == "zhipu-free");
    }
    SUBCASE("换成这家别的模型，还是这一家") {
        s.llm.model = "glm-5.3";
        CHECK(http::current_option(*llm, s) == "zhipu-free");
    }
    SUBCASE("地址结尾多个斜杠不算两家") {
        s.llm.base_url = "https://open.bigmodel.cn/api/paas/v4/";
        s.llm.model = "glm-5.3";
        CHECK(http::current_option(*llm, s) == "zhipu-free");
    }
    SUBCASE("别处的服务：认不出是哪一家，那就是「用别的外接服务」") {
        s.llm.base_url = "http://127.0.0.1:11434/v1";
        // 模型名碰巧和清单里那个一样也不算——地址才是身份证
        s.llm.model = "glm-4.7-flash";
        CHECK(http::current_option(*llm, s) == setup::kNoneOption);
    }
    // 「进程内那条路按文件认」那一支去掉了：那条后端已经没有了。
}

TEST_CASE("配齐了没有：接外面的服务也算配齐") {
    const auto* llm = &setup::catalog().front();
    REQUIRE(llm->key == "llm");

    config::Settings s;
    // 进程内那条按文件算：没填 [models].llm 就不算配齐
    s.llm.backend = "local";
    CHECK_FALSE(http::group_satisfied(*llm, s));

    // **这一条挡住的是"用云端大模型的人被永远关在初始化页上"。**
    // 本机/局域网的服务不校验密钥，填不填都算配齐。
    s.llm.backend = "remote";
    s.llm.base_url = "http://127.0.0.1:11434/v1";
    s.llm.api_key = "";
    CHECK(http::group_satisfied(*llm, s));

    // **默认那一档（OpenRouter）没填密钥就不算配齐。** 只看 backend
    // 的话这一页会放人过去，然后第一次写剧本 401。
    s.llm = config::LLMConfig{};
    REQUIRE(s.llm.api_key.empty());
    CHECK_FALSE(http::group_satisfied(*llm, s));
    s.llm.api_key = "sk-or-v1-填了";
    CHECK(http::group_satisfied(*llm, s));

    // 改回 local：密钥填没填都不算数，看的是权重在不在
    s.llm.backend = "local";
    CHECK_FALSE(http::group_satisfied(*llm, s));
    s.models.llm = "llm/没有这个文件.gguf";
    CHECK_FALSE(http::group_satisfied(*llm, s));
}

TEST_CASE("aria2 的进度行认得出来") {
    const auto p = setup::parse_aria2_progress(
        "[#396de5 3.7MiB/2.3GiB(0%) CN:8 DL:844KiB ETA:48m2s]");
    REQUIRE(p.has_value());
    CHECK(p->downloaded == static_cast<std::uint64_t>(3.7 * 1024 * 1024));
    CHECK(p->total == static_cast<std::uint64_t>(2.3 * 1024 * 1024 * 1024));
    CHECK(p->speed_bps == doctest::Approx(844.0 * 1024));
}

TEST_CASE("aria2 的进度：取最后一条，不是第一条") {
    // 日志是追加的，前面那些是几秒钟以前的。取第一条的表现是
    // 进度条永远停在开跑那一瞬间，而下载本身在跑。
    const std::string log =
        "[#396de5 3.7MiB/2.3GiB(0%) CN:8 DL:844KiB ETA:48m2s]\n"
        "FILE: /tmp/probe.gguf\n"
        "[#396de5 1.2GiB/2.3GiB(52%) CN:8 DL:12MiB ETA:1m30s]\n";
    const auto p = setup::parse_aria2_progress(log);
    REQUIRE(p.has_value());
    CHECK(p->downloaded == static_cast<std::uint64_t>(1.2 * 1024 * 1024 * 1024));
    CHECK(p->speed_bps == doctest::Approx(12.0 * 1024 * 1024));
}

TEST_CASE("aria2 的进度：不是进度行的一概返回空") {
    // 返回一个"0 字节"比返回空更糟：那会把进度条按回 0，
    // 看着像下载重来了。
    CHECK_FALSE(setup::parse_aria2_progress("").has_value());
    CHECK_FALSE(setup::parse_aria2_progress("Download Results:").has_value());
    CHECK_FALSE(setup::parse_aria2_progress("[#396de5 没有斜杠]").has_value());
}

TEST_CASE("人读的字节数换算") {
    CHECK(setup::parse_human_bytes("512") == 512);
    CHECK(setup::parse_human_bytes("1KiB") == 1024);
    CHECK(setup::parse_human_bytes("1.5MiB") == 1024 * 1024 * 3 / 2);
    CHECK(setup::parse_human_bytes(" 2GiB") ==
          static_cast<std::uint64_t>(2) * 1024 * 1024 * 1024);
    CHECK_FALSE(setup::parse_human_bytes("abc").has_value());
    CHECK_FALSE(setup::parse_human_bytes("").has_value());
}

TEST_CASE("进度快照能转成 JSON，字段一个都不少") {
    // 前端整页都靠这几个字段。少一个的表现是界面上某一处永远是
    // "undefined"，而接口本身 200。
    setup::Snapshot s;
    setup::ItemProgress p;
    p.group = "video";
    p.option = "h3-full-q4_k_m";
    p.name = "a.gguf";
    p.total = 100;
    p.downloaded = 40;
    p.speed_bps = 1024;
    p.state = setup::ItemState::Running;
    s.items.push_back(p);
    s.total = 100;
    s.downloaded = 40;
    s.state = setup::RunState::Running;

    const json j = s.to_json();
    CHECK(j["state"] == "running");
    CHECK(j["total"] == 100);
    CHECK(j["downloaded"] == 40);
    CHECK(j["items"][0]["state"] == "running");
    CHECK(j["items"][0]["speedBps"] == 1024.0);
    for (const char* key : {"state", "items", "error", "tool", "dir", "total",
                            "downloaded", "speedBps", "etaSeconds"}) {
        CAPTURE(key);
        CHECK(j.contains(key));
    }
}

TEST_CASE("下到一半的模型不能算齐——aria2c 会把文件先按最终大小占好") {
    // 2026-09-10 真栽在这上面：61.7 GB 的 bf16 才下了 6 GB，而 aria2c
    // 一开始就把整个文件预分配了，于是 file_size 返回的**正好是**清单里
    // 那个字节数。"大小对上 = 下完了"这条被骗过去，初始化页显示已完成，
    // 用户选了它去出片——花屏。而且一句报错都没有：safetensors 的头在
    // 文件开头，早下下来了，解析得好好的，错的是后面还是零的那些张量。
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path tmp =
        fs::temp_directory_path() / "changji_dl_test";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    const fs::path model = tmp / "big.safetensors";
    { std::ofstream f(model, std::ios::binary); f << "xxxx"; }

    SUBCASE("没有控制文件：不在下载中") {
        CHECK_FALSE(http::download_in_progress(model));
    }
    SUBCASE("有 .aria2 控制文件：还在下，哪怕大小已经对上") {
        fs::path ctrl = model;
        ctrl += ".aria2";
        { std::ofstream f(ctrl, std::ios::binary); f << "ctrl"; }
        CHECK(http::download_in_progress(model));
    }
    SUBCASE("文件压根不存在也不算在下载") {
        CHECK_FALSE(http::download_in_progress(tmp / paths::from_utf8("根本没有这个.gguf")));
    }
    fs::remove_all(tmp, ec);
}

TEST_CASE("同时下几个：按下载器分档，能用环境变量压回去") {
    using changji::setup::parallel_lanes;

    // curl 是单连接，并发几个就快几倍——这一档给得多。
    CHECK(parallel_lanes(10, "curl") == 4);
    // aria2c 自己已经开了 8 条连接（`-x 8`）。再乘 4 就是 32 条，
    // 源站那边要么限速要么掐连接，所以这一档只给 2。
    CHECK(parallel_lanes(10, "aria2c") == 2);

    // **不能超过要下的文件数**：三个文件开四路，多出来的那一路
    // 一开起来就发现没活干，白建一个线程。
    CHECK(parallel_lanes(3, "curl") == 3);
    CHECK(parallel_lanes(1, "curl") == 1);
    // 一个文件都没有时也得是 1：返回 0 的话下面那个线程池一路都不起，
    // 整轮下载会"成功"地什么都没下。
    CHECK(parallel_lanes(0, "curl") == 1);

    // 压回一个一个下，以及封顶。环境变量写坏了按默认来，不让整轮停下。
    {
        const changji::test::ScopedEnv one("CHANGJI_DOWNLOAD_PARALLEL", "1");
        CHECK(parallel_lanes(10, "curl") == 1);
    }
    {
        const changji::test::ScopedEnv many("CHANGJI_DOWNLOAD_PARALLEL", "99");
        CHECK(parallel_lanes(50, "curl") == 8);   // 封在 8
    }
    {
        const changji::test::ScopedEnv junk("CHANGJI_DOWNLOAD_PARALLEL", "很多");
        CHECK(parallel_lanes(10, "curl") == 4);   // 认不出就按默认
    }
}

TEST_CASE("下全了没改名的 .part 启动时收编") {
    // 父进程被杀、孤儿 curl 把文件写完：盘上是一个字节不差的 .part，
    // 而"这台有没有这个模型"问的是正式名字。见 adopt_finished_parts。
    namespace fs = std::filesystem;
    const auto& groups = setup::catalog();
    REQUIRE(!groups.empty());
    // 挑整张表里最小的那个文件，稀疏文件撑到它的字节数
    const setup::FileSpec* pick = nullptr;
    for (const auto& g : groups)
        for (const auto& o : g.options)
            for (const auto& f : o.files)
                if (f.bytes > 0 && (!pick || f.bytes < pick->bytes)) pick = &f;
    REQUIRE(pick != nullptr);

    const fs::path dir = fs::temp_directory_path() / paths::from_utf8("changji_收编part");
    fs::remove_all(dir);
    fs::create_directories(dir);
    const fs::path dest = dir / pick->name;
    fs::create_directories(dest.parent_path());
    const fs::path part = dest.parent_path() / (dest.filename().string() + ".part");

    SUBCASE("字节数恰好对上：改名") {
        { std::ofstream(part, std::ios::binary).put('x'); }
        fs::resize_file(part, pick->bytes);
        CHECK(setup::adopt_finished_parts(dir) == 1);
        CHECK(fs::exists(dest));
        CHECK_FALSE(fs::exists(part));
        CHECK(fs::file_size(dest) == pick->bytes);
    }
    SUBCASE("少了一个字节：不碰，留给续传") {
        { std::ofstream(part, std::ios::binary).put('x'); }
        fs::resize_file(part, pick->bytes - 1);
        CHECK(setup::adopt_finished_parts(dir) == 0);
        CHECK(fs::exists(part));
        CHECK_FALSE(fs::exists(dest));
    }
    fs::remove_all(dir);
}

TEST_CASE("H3 要有 16 GB 卡上挑得动的那几档") {
    // 用户 2026-09-17：「minimax-h3 增加支持 16g 显存可用的模型」。
    //
    // ⚠️ **先说清 16 GB 上会发生什么**：`resident_vram` 算出来的门槛是
    // 模型大小 + 15 GB 上下（那 15 GB 是视频解码和采样缓冲），所以 **H3 没有
    // 任何一档能在 16 GB 上常驻显存**——连 1 GB 的模型都要 16 GB。16 GB 上
    // 走的一定是"权重放内存"那条（`weights = smart` 自己会选 cpu）。
    //
    // 这几档的意义是**每一步搬的东西少一半**：同样走流式，6.3 GiB 比
    // 10.6 GiB 少搬四成多，而 PCIe 正是那条路的瓶颈。在这之前 16 GB 的卡上
    // H3 最小只有 10.6 GiB 那一档可挑。
    const auto cat = setup::catalog();
    const setup::Group* video = nullptr;
    for (const auto& g : cat) {
        if (g.key == "video") video = &g;
    }
    REQUIRE(video != nullptr);

    double smallest = 1e18;
    bool has_q3 = false, has_q2 = false;
    for (const auto& o : video->options) {
        if (o.family.rfind("MiniMax-H3", 0) != 0) continue;
        if (o.quant == "Q3_K") has_q3 = true;
        if (o.quant == "Q2_K") has_q2 = true;
        // 扩散模型那一份的大小（第一个文件就是它，见 catalog.cpp）
        if (!o.files.empty()) {
            smallest = std::min(smallest, static_cast<double>(o.files[0].bytes));
        }
    }
    CHECK(has_q3);
    CHECK(has_q2);
    // 最小那一档不该再是 10.6 GiB 那个
    CHECK(smallest < 7.0 * 1024 * 1024 * 1024);
}

TEST_CASE("开机自启：写出去的那个文件按平台是对的形状") {
    // ⚠️ **三套模板各自的语法错了不会报错**，只是开机不起来——而那要重启
    // 一次才发现。所以逐平台钉住形状。
    const auto body = setup::autostart_file_body(paths::from_utf8("/tmp/有空格 的/changji"), 8123);
    const auto path = setup::autostart_file_path().generic_string();
    CAPTURE(path);

    // 端口和可执行文件路径三个平台都要在。
    CHECK(body.find("8123") != std::string::npos);
    CHECK(body.find("changji") != std::string::npos);

#if defined(_WIN32)
    CHECK(path.find("Startup") != std::string::npos);
    // **`start ""` 那个空标题不能省**：`start "C:\path\x.exe"` 会把第一个
    // 带引号的参数当成窗口标题，于是什么都没起来，而且不报错。
    CHECK(body.find("start \"\"") != std::string::npos);
    // 路径有空格，必须带引号
    CHECK(body.find("\"/tmp/有空格 的/changji\"") != std::string::npos);
    // **文件是 UTF-8，cmd 按 GBK 读**：不先换代码页，路径里的中文就是乱码，
    // 开机起不来。换代码页那一行要在路径**之前**（cmd 读一行跑一行）。
    const auto chcp = body.find("chcp 65001");
    REQUIRE(chcp != std::string::npos);
    CHECK(chcp < body.find("有空格"));
#elif defined(__APPLE__)
    CHECK(path.find("LaunchAgents") != std::string::npos);
    CHECK(path.find("com.changji.server.plist") != std::string::npos);
    CHECK(body.find("<key>RunAtLoad</key><true/>") != std::string::npos);
    // **不能有 KeepAlive**：设了的话人从界面上把服务停掉，launchd 会立刻
    // 再拉起来——「我明明关了它还在」，而没有任何地方说得清为什么。
    CHECK(body.find("KeepAlive") == std::string::npos);
    // plist 里参数是一个个 <string>，不靠空格分词，所以路径不用引号
    CHECK(body.find("<string>--port</string>") != std::string::npos);
#else
    CHECK(path.find(".config/autostart") != std::string::npos);
    CHECK(body.find("[Desktop Entry]") != std::string::npos);
    CHECK(body.find("Type=Application") != std::string::npos);
#endif
}

TEST_CASE("开机自启：开了关了都读得回来") {
    const int port = 8125;
    const auto before = setup::autostart_status(port);
    if (!before.supported) return;   // 取不到 self_exe 的环境，跳过

    // **测完要还原**：这台机器上的真实状态不能被用例改掉。
    struct Restore {
        bool was;
        int port;
        ~Restore() { setup::set_autostart(was, port); }
    } restore{before.enabled, port};

    const auto on = setup::set_autostart(true, port);
    CHECK(on.error.empty());
    CHECK(on.state.enabled);
    CHECK(setup::autostart_status(port).enabled);

    const auto off = setup::set_autostart(false, port);
    CHECK(off.error.empty());
    CHECK_FALSE(off.state.enabled);
    CHECK_FALSE(setup::autostart_status(port).enabled);
}

TEST_CASE("更新检查：地址固定、判据是「一不一样」") {
    config::UpdateConfig cfg;
    CHECK(cfg.auto_check);              // 不默认开等于这件事没有
    CHECK(cfg.channel == "release");

    // 地址里那两个字**就是发布那头固定的 tag 名**，不做映射——多一层映射就
    // 多一个会对不上的地方。
    CHECK(setup::version_json_url(cfg) ==
          "https://github.com/integemjack/changji/releases/download/release/version.json");
    cfg.channel = "beta";
    CHECK(setup::version_json_url(cfg).find("/download/beta/version.json") !=
          std::string::npos);
    cfg.channel = "乱写的";
    CHECK(setup::version_json_url(cfg).find("/download/release/") !=
          std::string::npos);   // 认不出就回正式线

    // ⚠️ **不做语义化版本比较，只比一不一样。** 这套东西的版本号有两种形状
    // （`v1.2.0` 和 `beta-<分支>-<短 sha>`），后一种根本没有先后可言；而发布
    // 那头永远只有一个 Release，那上面挂的就是最新的，不一样就是该换了。
    CHECK(setup::is_different_version("v1.2.0", "v1.3.0"));
    CHECK(setup::is_different_version("v1.3.0", "v1.2.0"));  // 回退也算"不一样"
    CHECK_FALSE(setup::is_different_version("v1.2.0", "v1.2.0"));
    // 任一边不知道就当没有新版：**报一个假的"有新版"比不报更糟**，
    // 人点过去发现下不到。
    CHECK_FALSE(setup::is_different_version("", "v1.2.0"));
    CHECK_FALSE(setup::is_different_version("v1.2.0", ""));
}

TEST_CASE("更新检查：取不到、回的不是 JSON、缺字段，三种都不报假的新版") {
    config::UpdateConfig cfg;

    const auto none = setup::check_update(cfg, "v1.0.0",
                                          [](const std::string&) { return std::string(); });
    CHECK_FALSE(none.newer);
    CHECK_FALSE(none.error.empty());

    // 取到的多半是一张 404 页面。**不把原文贴进错误里**——几十 KB 的 HTML
    // 摆进界面没人读得下去。
    const std::string html(4000, 'x');
    const auto junk = setup::check_update(cfg, "v1.0.0",
                                          [&](const std::string&) { return "<html>" + html; });
    CHECK_FALSE(junk.newer);
    CHECK(junk.error.size() < 200);

    const auto nover = setup::check_update(
        cfg, "v1.0.0", [](const std::string&) { return R"({"channel":"release"})"; });
    CHECK_FALSE(nover.newer);
    CHECK_FALSE(nover.error.empty());

    const auto ok = setup::check_update(cfg, "v1.0.0", [](const std::string&) {
        return R"({"version":"v1.1.0","built_at":"2026-09-17T10:00:00Z"})";
    });
    CHECK(ok.newer);
    CHECK(ok.latest == "v1.1.0");
    CHECK(ok.built_at == "2026-09-17T10:00:00Z");
    CHECK(ok.error.empty());
    CHECK(ok.url.find("releases") != std::string::npos);
}

TEST_CASE("更新检查带缓存：关着不问、没到点不问、问砸了不盖掉上一次") {
    // ⚠️ **不带缓存的话，每打开一次设置页就是一次跨网请求**，超时 8 秒。
    // GitHub 连不上的时候那一页每次都要多转 8 秒——而那一页恰恰是"出事了才
    // 打开"的那一页。
    config::UpdateConfig cfg;
    cfg.every_hours = 24.0;
    // **每条用例自己一份缓存。** 共用一个的话上一条填进去的东西会让下一条
    // 根本不去问，而"到底问没问"正是这段唯一要测的东西。
    setup::UpdateCache cache;
    int calls = 0;
    const auto good = [&](const std::string&) {
        ++calls;
        return std::string(R"({"version":"v9.9.9"})");
    };

    SUBCASE("关着就一次都不问") {
        cfg.auto_check = false;
        const auto a = setup::cached_update(cache, cfg, "v1.0.0", good, /*force=*/false);
        CHECK(calls == 0);
        CHECK_FALSE(a.newer);
        // **手上没有答案时别编一个**：说清是"没查"，不是"已经是最新的"。
        CHECK_FALSE(a.error.empty());

        // 人点了「现在查一次」，那一下必须真去问，不然按钮等于没有。
        const auto b = setup::cached_update(cache, cfg, "v1.0.0", good, /*force=*/true);
        CHECK(calls == 1);
        CHECK(b.newer);
    }

    SUBCASE("没到点就给上一次那份") {
        const auto a = setup::cached_update(cache, cfg, "v1.0.0", good, false);
        CHECK(calls == 1);
        CHECK(a.newer);
        for (int i = 0; i < 5; ++i) setup::cached_update(cache, cfg, "v1.0.0", good, false);
        CHECK(calls == 1);   // 还是那一次
    }

    SUBCASE("问砸了不盖掉上一次问到的那份") {
        // 网断一下就把"有新版"抹成"取不到"，而那条消息本来是对的。
        setup::cached_update(cache, cfg, "v1.0.0", good, true);
        const auto after = setup::cached_update(
            cache, cfg, "v1.0.0", [](const std::string&) { return std::string(); }, true);
        CHECK(after.newer);
        CHECK(after.latest == "v9.9.9");
    }
}

TEST_CASE("state 里的 selected 要带上替换档，不然弹窗一开就弹回默认") {
    // 用户 2026-09-18：出片模型里把文本编码器换成 Q2_K_M、视频 VAE 换成
    // int8_convrot，存下，**再打开两个都弹回默认**。
    //
    // `selected` 是界面上那几个下拉的初值（models.js 里 `picks` 就是拿它
    // 拼的）。原来这儿只放组一级那个 id，替换档那两个键读不到，Vue 那边
    // 回落到 `alt.choices[0]` —— 默认那档。
    //
    // 当时那一屏是自相矛盾的：精度那一列的体积按存下的小编码器算（走的是
    // option_json → alts_from_config），下拉却显示大的那份。真正伤人的是
    // 接着那一下——再点一次「保存」，发出去的是下拉里那份默认值，
    // **用户特意挑的小编码器被自己的保存覆盖掉**。
    const setup::Group* video = nullptr;
    for (const auto& g : setup::catalog()) {
        if (g.key == "video") video = &g;
    }
    REQUIRE(video != nullptr);

    // 找一档带替换档的（H3 完整那一档）
    const setup::Option* opt = nullptr;
    for (const auto& o : video->options) {
        if (!o.alts.empty()) { opt = &o; break; }
    }
    REQUIRE(opt != nullptr);
    REQUIRE(opt->alts.size() >= 2);

    // 每一组替换档都挑**不是第一个**的那一份——第一个是默认值，
    // 拿它测的话，"回填了"和"回落到默认"两种情况长得一模一样。
    config::Settings s;
    std::map<std::string, std::string> want;
    for (const auto& alt : opt->alts) {
        REQUIRE(alt.choices.size() >= 2);
        const auto& pick = alt.choices.back();
        std::string* field = config::models_field(s.models, alt.role);
        REQUIRE(field != nullptr);
        *field = pick.name;
        want[setup::alt_key(video->key, alt.role)] = pick.name;
        // 选的确实不是默认那份，不然这条测试什么都证明不了
        REQUIRE(pick.name != alt.choices.front().name);
    }
    // 组一级也得指着这一档，否则 selected[video] 会落到推荐档上，
    // 而那一档的 alts 未必是这几个
    {
        std::string* f = config::models_field(s.models, "video");
        REQUIRE(f != nullptr);
        *f = opt->files.empty() ? std::string() : opt->files.front().name;
    }

    models::HardwareProfile profile;
    profile.vram_gb = 48.0;
    const auto res = http::get_setup_state(s, profile);
    REQUIRE(res.status == 200);
    const auto& sel = res.body.at("selected");

    for (const auto& [k, v] : want) {
        CHECK_MESSAGE(sel.contains(k), "selected 里没有替换档的键：" << k);
        if (sel.contains(k)) CHECK(sel.at(k).get<std::string>() == v);
    }
}

// ---------------------------------------------------------------------------
// 模型放哪、去哪儿找（2026-09-24）
// ---------------------------------------------------------------------------
//
// 用户那天的三句话：「下载模型应该放到模型目录下相应类型的目录里，目录用英语」
// 「增加索引目录下所有模型文件的功能」「增加下载模型选择模型存放目录」。
//
// 盯的是"错了不报错"的那两样：**新下的换了位置之后，老位置上那几份还认不认**
// （不认 = 几十 GB 重下一遍），和**人自己放进来的认不认**。

namespace {

namespace fs = std::filesystem;

void put_file(const fs::path& p, std::uint64_t bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    { std::ofstream(p, std::ios::binary).put('x'); }
    fs::resize_file(p, bytes);
}

std::string base_name(const std::string& rel) {
    const auto slash = rel.find_last_of('/');
    return slash == std::string::npos ? rel : rel.substr(slash + 1);
}

bool same_file(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}

}  // namespace

TEST_CASE("新下的按类型放进模型目录下的英文子目录") {
    CHECK(setup::type_dir("llm") == "llm");
    CHECK(setup::type_dir("video") == "video");
    CHECK(setup::type_dir("image") == "image");
    // 定妆和空景那一组和首帧那一组是同一族图像模型，放一处。
    CHECK(setup::type_dir("image_base") == "image");
    CHECK(setup::type_dir("tts") == "tts");

    for (const auto& g : setup::catalog()) {
        for (const auto& o : g.options) {
            std::vector<setup::FileSpec> files = o.files;
            for (const auto& alt : o.alts) {
                files.insert(files.end(), alt.choices.begin(), alt.choices.end());
            }
            for (const auto& f : files) {
                const std::string rel = setup::download_rel(g.key, f);
                CAPTURE(g.key);
                CAPTURE(rel);
                const auto slash = rel.find('/');
                REQUIRE(slash != std::string::npos);
                // **只一层**：`<类型>/<文件名>`。清单里带的子目录（`loras/`）不跟着走。
                CHECK(rel.find('/', slash + 1) == std::string::npos);
                CHECK(rel.substr(slash + 1) == base_name(f.name));
                // 类型那一段是英文：小写字母和下划线，一个汉字都没有。
                const std::string dir = rel.substr(0, slash);
                CHECK(!dir.empty());
                CHECK(std::all_of(dir.begin(), dir.end(), [](char c) {
                    return (c >= 'a' && c <= 'z') || c == '_';
                }));
            }
        }
    }

    // 编剧那一组本来就在 `llm/` 底下：落点和原来一模一样，老用户那份不用挪。
    for (const auto& g : setup::catalog()) {
        if (g.key != "llm") continue;
        for (const auto& o : g.options) {
            for (const auto& f : o.files) {
                CAPTURE(f.name);
                CHECK(setup::download_rel(g.key, f) == f.name);
            }
        }
    }
}

TEST_CASE("模型目录按名字找：新位置、老位置、人自己放的都认") {
    const fs::path tmp = fs::temp_directory_path() / paths::from_utf8("changji_模型索引");
    std::error_code ec;
    fs::remove_all(tmp, ec);
    put_file(tmp / "video" / "a.gguf", 10);                     // 新位置
    put_file(tmp / "b.safetensors", 5);                         // 老位置，平铺在根上
    put_file(tmp / "mine" / "deep" / "c.gguf", 7);              // 人自己放的
    put_file(tmp / ".cache" / "d.gguf", 3);                     // 点开头的目录不扫
    put_file(tmp / "video" / "e.gguf.part", 4);                 // 下到一半的不算
    put_file(tmp / "readme.txt", 2);                            // 不是模型
    put_file(tmp / "x" / "same.gguf", 1);                       // 同名两份：浅的那份优先
    put_file(tmp / "y" / "z" / "same.gguf", 1);
    put_file(tmp / paths::from_utf8("图像") / paths::from_utf8("中文名.gguf"), 6);
    config::forget_model_index();

    SUBCASE("索引里只有模型文件，路径用 / 分隔、UTF-8") {
        const auto idx = config::index_models(tmp, /*fresh=*/true);
        std::set<std::string> rels;
        for (const auto& f : idx.files) rels.insert(f.rel);
        const std::set<std::string> want{"b.safetensors", "mine/deep/c.gguf", "video/a.gguf",
                                         "x/same.gguf", "y/z/same.gguf", "图像/中文名.gguf"};
        CHECK(rels == want);
        CHECK_FALSE(idx.truncated);
    }

    SUBCASE("按文件名找，名字里带的子目录不算数") {
        auto hit = config::find_model(tmp, "a.gguf");
        REQUIRE(hit.has_value());
        CHECK(same_file(*hit, tmp / "video" / "a.gguf"));
        // 清单里写的是 `loras/a.gguf`、文件在 `video/` 底下：照样认。
        hit = config::find_model(tmp, "loras/a.gguf");
        REQUIRE(hit.has_value());
        CHECK(same_file(*hit, tmp / "video" / "a.gguf"));
        hit = config::find_model(tmp, "c.gguf");
        REQUIRE(hit.has_value());
        CHECK(same_file(*hit, tmp / "mine" / "deep" / "c.gguf"));
        hit = config::find_model(tmp, "中文名.gguf");
        REQUIRE(hit.has_value());
        CHECK(same_file(*hit, tmp / paths::from_utf8("图像") / paths::from_utf8("中文名.gguf")));
        hit = config::find_model(tmp, "same.gguf");
        REQUIRE(hit.has_value());
        CHECK(same_file(*hit, tmp / "x" / "same.gguf"));
    }

    SUBCASE("给了大小就只认大小恰好对上的") {
        CHECK(config::find_model(tmp, "a.gguf", 10).has_value());
        // 截断的同名文件不算：拿它去出片，加载时报的是"权重读不对"。
        CHECK_FALSE(config::find_model(tmp, "a.gguf", 11).has_value());
    }

    SUBCASE("点开头的目录、下到一半的、不存在的：都找不到") {
        CHECK_FALSE(config::find_model(tmp, "d.gguf").has_value());
        CHECK_FALSE(config::find_model(tmp, "e.gguf").has_value());
        CHECK_FALSE(config::find_model(tmp, "没有这个.gguf").has_value());
    }

    SUBCASE("resolve：拼出来的位置上没有，就按名字在目录里找") {
        config::ModelsConfig m;
        m.dir = paths::to_utf8(tmp);
        const fs::path ws = tmp / "ws";
        CHECK(same_file(m.resolve("a.gguf", ws), tmp / "video" / "a.gguf"));
        // 原位置上有的照旧回原位置。
        CHECK(same_file(m.resolve("b.safetensors", ws), tmp / "b.safetensors"));
        // 哪儿都没有：照旧回拼出来的那个（体检拿它说"该放哪儿"）。
        CHECK(m.resolve("nothere.gguf", ws).lexically_normal() ==
              fs::absolute(tmp / "nothere.gguf").lexically_normal());
    }

    SUBCASE("挪走之后不认旧的：缓存里那份过时了就重扫") {
        REQUIRE(config::find_model(tmp, "c.gguf").has_value());
        fs::rename(tmp / "mine" / "deep" / "c.gguf", tmp / "video" / "c.gguf");
        const auto hit = config::find_model(tmp, "c.gguf");
        REQUIRE(hit.has_value());
        CHECK(same_file(*hit, tmp / "video" / "c.gguf"));
    }

    config::forget_model_index();
    fs::remove_all(tmp, ec);
}

TEST_CASE("索引接口：认得的、大小不对的、不认得的，各说各的") {
    // 清单里最小的两个文件，一个按真大小摆（认得），一个只摆 1 字节（大小不对）。
    const setup::Group* g_small = nullptr;
    const setup::FileSpec* small = nullptr;
    for (const auto& g : setup::catalog())
        for (const auto& o : g.options)
            for (const auto& f : o.files)
                if (f.bytes > 0 && (!small || f.bytes < small->bytes)) {
                    small = &f;
                    g_small = &g;
                }
    REQUIRE(small != nullptr);
    const setup::FileSpec* other = nullptr;
    for (const auto& g : setup::catalog())
        for (const auto& o : g.options)
            for (const auto& f : o.files)
                if (f.bytes > 0 && base_name(f.name) != base_name(small->name)) other = &f;
    REQUIRE(other != nullptr);

    const fs::path tmp = fs::temp_directory_path() / "changji_index_api";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    put_file(tmp / paths::from_utf8(setup::download_rel(g_small->key, *small)), small->bytes);
    put_file(tmp / "old" / base_name(other->name), 1);
    put_file(tmp / "mine.gguf", 3);

    config::Settings s;
    s.models.dir = paths::to_utf8(tmp);
    const auto r = http::get_setup_index(s);
    REQUIRE(r.status == 200);
    const json& j = r.body;
    CHECK(j["exists"] == true);
    CHECK(j["count"] == 3);
    CHECK(j["known"] == 1);
    std::map<std::string, json> by;
    for (const auto& f : j["files"]) by[f["path"].get<std::string>()] = f;
    const std::string known_at = setup::download_rel(g_small->key, *small);
    REQUIRE(by.count(known_at) == 1);
    CHECK(by[known_at]["status"] == "known");
    CHECK(by[known_at]["group"] == g_small->key);
    REQUIRE(by.count("old/" + base_name(other->name)) == 1);
    CHECK(by["old/" + base_name(other->name)]["status"] == "size");
    CHECK(by["old/" + base_name(other->name)]["expectBytes"] == other->bytes);
    REQUIRE(by.count("mine.gguf") == 1);
    CHECK(by["mine.gguf"]["status"] == "unknown");

    SUBCASE("目录还不存在：不报错，回一份空的") {
        config::Settings none;
        none.models.dir = paths::to_utf8(tmp / "没有这个目录");
        const auto e = http::get_setup_index(none);
        CHECK(e.status == 200);
        CHECK(e.body["exists"] == false);
        CHECK(e.body["count"] == 0);
    }

    config::forget_model_index();
    fs::remove_all(tmp, ec);
}

TEST_CASE("下载器：老位置上已经有的不重下，下了一半的挪到新位置接着下") {
    if (setup::pick_tool().empty()) {
        WARN("这台机器上没有 aria2c / curl，下载器起不来，跳过");
        return;
    }
    const fs::path tmp = fs::temp_directory_path() / "changji_dl_legacy";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    // 一份平铺在根上、大小对得上的（这一版之前下的）；一份平铺在根上、`.part`
    // 已经下全只差改名的（上一轮父进程被杀）。**两份都不该发起任何下载**——
    // 地址指着一个连不上的端口，真去下的话这一项会报失败。
    put_file(tmp / "legacy.gguf", 16);
    put_file(tmp / "half.gguf.part", 12);
    config::forget_model_index();

    const auto spec = [](const std::string& name, std::uint64_t bytes) {
        setup::FileSpec f;
        f.name = name;
        f.bytes = bytes;
        return f;
    };
    std::vector<setup::Item> items;
    items.push_back({"video", "x", spec("legacy.gguf", 16), "video/legacy.gguf",
                     "http://127.0.0.1:9/never"});
    items.push_back({"video", "x", spec("half.gguf", 12), "video/half.gguf",
                     "http://127.0.0.1:9/never"});
    auto& d = setup::Downloader::instance();
    REQUIRE(d.start(items, tmp, "modelscope", {}));
    for (int i = 0; i < 100 && d.running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE_FALSE(d.running());
    const auto snap = d.snapshot();
    CHECK(snap.state == setup::RunState::Done);
    REQUIRE(snap.items.size() == 2);

    // 老位置上那份：认出来就算有，**不挪、不复制**，报它实际在哪。
    CHECK(snap.items[0].state == setup::ItemState::Present);
    CHECK(snap.items[0].path == "legacy.gguf");
    CHECK(fs::exists(tmp / "legacy.gguf"));
    CHECK_FALSE(fs::exists(tmp / "video" / "legacy.gguf"));

    // 下全了的 `.part`：挪到新位置、改成正式名字。
    CHECK(snap.items[1].state == setup::ItemState::Present);
    CHECK(fs::file_size(tmp / "video" / "half.gguf") == 12);
    CHECK_FALSE(fs::exists(tmp / "half.gguf.part"));

    config::forget_model_index();
    fs::remove_all(tmp, ec);
}

TEST_CASE("下全了没改名的 .part：新位置（类型目录）底下的也收编") {
    const setup::Group* grp = nullptr;
    const setup::FileSpec* pick = nullptr;
    for (const auto& g : setup::catalog())
        for (const auto& o : g.options)
            for (const auto& f : o.files)
                if (f.bytes > 0 && (!pick || f.bytes < pick->bytes)) {
                    pick = &f;
                    grp = &g;
                }
    REQUIRE(pick != nullptr);
    const fs::path dir = fs::temp_directory_path() / "changji_adopt_typed";
    std::error_code ec;
    fs::remove_all(dir, ec);
    const fs::path dest = dir / paths::from_utf8(setup::download_rel(grp->key, *pick));
    const fs::path part = dest.parent_path() / (dest.filename().string() + ".part");
    put_file(part, pick->bytes);
    CHECK(setup::adopt_finished_parts(dir) == 1);
    CHECK(fs::file_size(dest) == pick->bytes);
    CHECK_FALSE(fs::exists(part));
    fs::remove_all(dir, ec);
}
