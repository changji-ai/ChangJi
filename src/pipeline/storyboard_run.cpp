#include "pipeline/storyboard_run.hpp"

#include <algorithm>

#include "config/runtime.hpp"
#include "llm/call_log.hpp"
#include "stages/storyboard.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

namespace changji::pipeline {

using namespace changji::models;

namespace {

/// 拆一段镜头，顺手把闸门的裁决记进 `gates.jsonl`（见 `llm::GateVerdict`）。
///
/// **记的是「这一次调用吐的东西解析得动吗」**，一次调用一行，对得回
/// `index.jsonl` 里那一行和它的 .prompt.txt。粘回来的那条路没有调用，
/// `call_id` 是空的，于是整个不记。
///
/// ⚠️ **拆完之后那道「分镜表不完整」（check_coverage）不在这儿记。** 它是把
/// 各场合起来之后才判的章级判定，而按场拆的时候手上有 N 次调用——挂在其中
/// 任何一次头上都是假的。要记它得另起一种"章级裁决"，那是另一件事。
std::vector<Shot> parse_and_note(const std::string& raw,
                                 const AssetLibrary& assets,
                                 const std::string& subject) {
    llm::GateVerdict v;
    // **在 `complete()` 回来之后才取**，见 `llm::take_last_call_id()`。
    v.call_id = llm::take_last_call_id();
    v.stage = "storyboard";
    v.subject = subject;
    const auto opt = llm::call_log_options(config::runtime().snapshot().llm);
    try {
        std::vector<Shot> shots = stages::parse_storyboard(raw, assets);
        v.ok = true;
        llm::note_gate(v, opt);
        return shots;
    } catch (const stages::StoryboardError& e) {
        v.gate = e.code();
        v.reason = e.what();
        llm::note_gate(v, opt);
        throw;
    }
}

}  // namespace

StoryboardRunResult run_storyboard(const StoryboardRunOptions& opts,
                                   llm::Client& client, CancelToken& tok) {
    StoryboardRunResult result;
    const std::string& script = opts.script;
    const AssetLibrary& assets = opts.assets;

    std::vector<stages::SceneBlock> scenes = stages::split_scenes(script, assets);
    std::vector<Shot> shots;
    // **目标量按剧本估，不按名义时长。** 估不出来（剧本是空的、全是场次头）
    // 才退回 duration_s。
    const double estimated = stages::estimate_script_seconds(script);
    const double target_s = estimated > 0.0 ? estimated : opts.duration_s;

    if (scenes.size() <= 1) {
        // ---- 整章一次拆：和 2026-09-15 之前逐字节一样 ----
        const stages::DurationQuota quota =
            stages::DurationQuota::for_duration(target_s);
        llm::Request req;
        req.prompt = stages::build_storyboard_prompt(script, assets, quota,
                                                     opts.episode_id);
        // 镜头数写进 schema。配额那句话模型不一定听——实测 60 秒的章出过
        // 两镜六秒，提示词里"合计 16 个镜头"一个字没少。
        req.schema = stages::llm_shot_schema(
            assets, stages::shot_count_bounds(quota, target_s,
                                              stages::count_beats(script)));
        req.schema_name = "storyboard";
        req.on_thinking = opts.on_thinking;
        // **想多久不在这儿写死了**（2026-09-19，原来是 `= "high"`）。这一步
        // 正是最早逮到这个病的地方：2026-09-17 把思考捞出来读，它在思考里把
        // 整张分镜表逐镜写完了（Shot 11 的 first_frame / dialogue / motion /
        // characters 全是成品），然后才用 JSON 再写一遍——一场 410 秒还一个
        // 字正文都没落。
        //
        // 病和药方都没变，只是搬进了配置（`[llm] reasoning_effort`，默认
        // 仍是 high，模型窗里能调；想单独给这一步换档用
        // `[llm.effort] storyboard`）。留空 = 按配置来。
        // 真觉得镜头变差了，先把那一项调回 max 再谈别的。
        if (opts.peek) {
            result.peeked = llm::schema_as_prompt(req.prompt, req.schema);
            return result;
        }
        shots = parse_and_note(
            opts.pasted.empty() ? client.complete(req, tok) : opts.pasted.front(),
            assets, opts.episode_id.empty() ? SAY("整章") : opts.episode_id);
    } else {
        // ---- 按场拆 ----
        stages::assign_scene_seconds(scenes, target_s);
        result.scenes = static_cast<int>(scenes.size());
        std::string prev_tail;
        int running_order = 0;
        for (std::size_t i = 0; i < scenes.size(); ++i) {
            const stages::SceneBlock& scene = scenes[i];
            if (opts.on_progress) {
                const std::string nth = std::to_string(scene.index);
                const std::string all = std::to_string(scenes.size());
                opts.on_progress(
                    scene.body.empty()
                        ? SAYF("正在拆第 %1/%2 场", nth, all)
                        : SAYF("正在拆第 %1/%2 场：%3", nth, all, scene.body));
            }
            const stages::DurationQuota quota =
                stages::DurationQuota::for_duration(scene.seconds);
            llm::Request req;
            req.prompt = stages::build_scene_storyboard_prompt(
                scene, static_cast<int>(scenes.size()), assets, quota,
                opts.episode_id, prev_tail);
            req.schema = stages::llm_scene_shot_schema(
                assets,
                stages::shot_count_bounds(quota, scene.seconds,
                                          stages::count_beats(scene.text)),
                scene.location_id,
                // 第几场。拆完 `stamp_scene` 盖的就是这个数，提前告诉模型，
                // 省得它为一个会被覆盖的值猜半天。
                std::max(1, scene.index));
            req.schema_name = "storyboard";
            req.on_thinking = opts.on_thinking;
            // 同上面整章那条：想多久按配置来，理由写在那儿。
            if (opts.peek) {
                // 场次头写清楚是哪一场：三份提示词贴到别处去跑，得认得出
                // 哪份对哪份。
                const std::string nth = std::to_string(scene.index);
                const std::string all = std::to_string(scenes.size());
                result.peeked +=
                    (scene.body.empty()
                         ? SAYF("\n\n===== 第 %1/%2 场 =====\n\n", nth, all)
                         : SAYF("\n\n===== 第 %1/%2 场：%3 =====\n\n", nth, all,
                                scene.body)) +
                    llm::schema_as_prompt(req.prompt, req.schema);
                continue;
            }

            // 粘回来的那一段，或者去问模型。**数量在进来之前就核过**
            //（见 post_plan）：少一段的话后面几场整体错位一场，而错位
            // 出来的分镜表看着是合法的，没有任何报错。
            const std::string raw = opts.pasted.empty()
                                        ? client.complete(req, tok)
                                        : opts.pasted.at(i);
            std::vector<Shot> part = parse_and_note(
                raw, assets,
                SAYF("%1 · 第 %2 场",
                     opts.episode_id.empty() ? SAY("整章") : opts.episode_id,
                     std::to_string(scene.index)));
            stages::stamp_scene(part, scene);
            // 各场的 order 都从 0 起，合起来之前先排成全章的次序，
            // 不然最后重编号那一步按 order 稳定排序会把几场交错在一起。
            std::stable_sort(part.begin(), part.end(),
                             [](const Shot& a, const Shot& b) {
                                 return a.order < b.order;
                             });
            for (Shot& s : part) s.order = running_order++;
            const Shot& last = part.back();
            prev_tail = !text::strip_ws(last.visual_desc).empty()
                            ? last.visual_desc
                            : last.first_frame_prompt;
            shots.insert(shots.end(), part.begin(), part.end());
        }
        // **只看不发到此为止。** 底下那几道（补台词、查覆盖、重编号）都是
        // 对着真镜头做的，零镜头走过去会报"分镜表不完整"——而这一下本来
        // 就没打算生成任何镜头。
        if (opts.peek) return result;
    }

    // **先把剧本里漏掉的台词补进去，再查。** 分镜模型不搬台词——实跑
    // 九句只写两句，dump 出来看是压根没生成。台词本来就在剧本里，
    // 有顺序有说话人，引擎自己放比指望模型重打一遍靠谱。
    //
    // 放在 check_coverage 之前：一句台词都没写的那种「哑剧」，本来就
    // 是这一步能救回来的，不该先报错退出。
    result.placed_lines = stages::place_missing_dialogue(shots, script, assets);
    // 口型要在补完台词之后推，否则补进去的那几镜不会做口型。
    const auto gaps = stages::check_coverage(script, shots);
    if (!gaps.empty()) {
        // **整段一个键。** 原来是「抬头 + 逐条 + 收尾」三截拼的，拼出来
        // 的三块拿给翻译的人，中间填什么、语序怎么排都看不见。
        std::string joined;
        for (const auto& g : gaps) joined += "\n" + g;
        const std::string msg = SAYF(
            "分镜表不完整：%1\n\n换一个更强的模型，或者手工补齐这些字段后再跑。",
            joined);
        // 代号给着，虽然这一条现在还不进 gates.jsonl（章级判定，见
        // `parse_and_note` 上那段）。不给的话将来接上时又要回来找一遍。
        throw stages::StoryboardError(msg, "coverage_gap");
    }
    apply_lipsync_rules(shots);
    // 编号和顺序按引擎的来。模型编出来的 id 有错章号、没补零、打错字的。
    stages::renumber_shots(shots, opts.episode_id);
    // **不压回目标时长。** 这一章多长由它自己的内容定，整部电影是最后把
    // 各章接起来，不在这儿也不在装配时切。2026-09-16 之前这儿有一道
    // rebalance_durations，只在按时长切的那条路上跑——那条路当天删了，
    // 这一道跟着没了。
    result.shots = std::move(shots);
    return result;
}

}  // namespace changji::pipeline
