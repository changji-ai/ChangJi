// 工作进程的协议。
//
// 这一层没有 Python 对应物，是纯粹的新逻辑，不走对拍。
//
// **单独抽出来就是为了能在这儿测。** 接上真模型之后一个用例要跑几分钟，
// 协议层的 bug 没法反复撞——和 `scheduler` 把加载卸载做成注入回调
// 是同一个理由。

#include <doctest/doctest.h>

#include <set>

#include <string>

#include "infer/worker_farm.hpp"
#include "infer/worker_proto.hpp"
#include "util/text.hpp"

using namespace changji;

namespace {

infer::Task sample_frame() {
    infer::Task t;
    t.kind = infer::TaskKind::Frame;
    t.shot_id = "ep01_sh001";
    t.prompts.positive = "雨夜天台，霓虹";
    t.prompts.negative = "低质量";
    t.prompts.reference_images = {"assets/lin.png", "assets/rooftop.png"};
    t.spec.tier = models::Tier::DRAFT;
    t.spec.width = 448;
    t.spec.height = 256;
    t.spec.steps = 8;
    t.spec.steps_pinned = true;
    t.prompts.base_model = true;
    t.prompts.seed_override = 4242;
    t.dest = R"(C:\项目\frames\ep01_sh001.png)";
    t.seed = 1234567890123LL;
    return t;
}

}  // namespace

TEST_CASE("任务往返一圈不丢东西") {
    const auto t = sample_frame();
    const auto back = infer::task_from_json(infer::to_json(t));

    CHECK(back.kind == t.kind);
    CHECK(back.shot_id == t.shot_id);
    CHECK(back.prompts.positive == t.prompts.positive);
    CHECK(back.prompts.negative == t.prompts.negative);
    CHECK(back.prompts.reference_images == t.prompts.reference_images);
    CHECK(back.spec.width == t.spec.width);
    CHECK(back.spec.height == t.spec.height);
    CHECK(back.spec.steps == t.spec.steps);
    // 跨机时干活那台按这个决定动不动步数，丢了就会把人钉死的 28 压成 6
    CHECK(back.spec.steps_pinned);
    // 定妆图派出去：要基础文生图权重、种子是发起方定死的
    CHECK(back.prompts.base_model);
    REQUIRE(back.prompts.seed_override.has_value());
    CHECK(*back.prompts.seed_override == 4242);
    // **中文路径和反斜杠**。这个项目在这上面栽过好几次，
    // 而任务是要跨进程传的，路径错了产物就写到别处去了。
    CHECK(back.dest == t.dest);
    CHECK(back.seed == t.seed);
}

TEST_CASE("出片任务的那几项也要活着回来") {
    infer::Task t;
    t.kind = infer::TaskKind::Video;
    t.shot_id = "ep01_sh002";
    t.dest = "/tmp/x.mp4";
    t.frames = 25;
    t.motion = "推近";
    t.style_line = models::StyleLine::ANIME;
    t.tier = models::Tier::FINAL;
    t.start_image = "/tmp/first.png";
    t.seed = 42;

    const auto back = infer::task_from_json(infer::to_json(t));
    CHECK(back.kind == infer::TaskKind::Video);
    CHECK(back.frames == 25);
    CHECK(back.motion == "推近");
    // **style_line 必须传对。** 它决定提示词用哪个分隔符，
    // 而提示词是要和 Python 逐字节对上的——方案里记过这个坑：
    // 两个视频后端都写死过 REALISTIC，动画线的项目就差一个分隔符。
    CHECK(back.style_line == models::StyleLine::ANIME);
    CHECK(back.tier == models::Tier::FINAL);
    REQUIRE(back.start_image.has_value());
    CHECK(*back.start_image == "/tmp/first.png");
}

TEST_CASE("尾帧、环境声开关、视频那份负向词和画面层也要活着回来") {
    // 2026-09-16 之前这几项不在协议里：填了 last_frame_prompt、尾帧也出来了，
    // 走工作进程池就退回单帧图生视频；派出去的出片任务负向词是空的；环境
    // 声按工作进程那台的设置来，同一章一半有一半没有。
    infer::Task t;
    t.kind = infer::TaskKind::Video;
    t.shot_id = "ep01_sh001";
    t.dest = "/tmp/out.mp4";
    t.seed = 1;
    t.start_image = "/tmp/first.png";
    t.end_image = "/tmp/last.png";
    t.keep_ambient = false;
    t.prompts.negative_video = "溶解转场";
    t.prompts.video_scene = "中景，雨夜天台";
    t.prompts.style_layer = "胶片质感";
    const auto j = infer::to_json(t);
    const infer::Task back = infer::task_from_json(j);
    REQUIRE(back.end_image.has_value());
    CHECK(*back.end_image == "/tmp/last.png");
    REQUIRE(back.keep_ambient.has_value());
    CHECK_FALSE(*back.keep_ambient);
    CHECK(back.prompts.negative_video == "溶解转场");
    CHECK(back.prompts.video_scene == "中景，雨夜天台");
    CHECK(back.prompts.style_layer == "胶片质感");

    SUBCASE("没填的不凭空多出来") {
        infer::Task bare;
        bare.kind = infer::TaskKind::Video;
        bare.shot_id = "x";
        bare.dest = "/tmp/x.mp4";
        const auto jb = infer::to_json(bare);
        CHECK_FALSE(jb.contains("end_image"));
        CHECK_FALSE(jb.contains("keep_ambient"));
        const infer::Task bb = infer::task_from_json(jb);
        CHECK_FALSE(bb.end_image.has_value());
        CHECK_FALSE(bb.keep_ambient.has_value());
    }
}

TEST_CASE("没有首帧图的任务不该凭空多出一个") {
    auto t = sample_frame();
    t.start_image.reset();
    const auto j = infer::to_json(t);
    CHECK_FALSE(j.contains("start_image"));
    CHECK_FALSE(infer::task_from_json(j).start_image.has_value());
}

TEST_CASE("缺必填字段要当场报，不能用默认值糊过去") {
    // 一个没带 dest 的任务默默写到当前目录去，比当场报错难查得多。
    auto j = infer::to_json(sample_frame());

    for (const char* key : {"kind", "shot_id", "dest", "seed", "spec",
                            "prompts"}) {
        CAPTURE(key);
        auto broken = j;
        broken.erase(key);
        CHECK_THROWS_AS(infer::task_from_json(broken), std::runtime_error);
    }
}

TEST_CASE("kind 只认 frame 和 video") {
    auto j = infer::to_json(sample_frame());
    j["kind"] = "随便写的";
    CHECK_THROWS_AS(infer::task_from_json(j), std::runtime_error);

    CHECK(infer::task_kind_from("frame").has_value());
    CHECK(infer::task_kind_from("video").has_value());
    CHECK_FALSE(infer::task_kind_from("").has_value());
}

TEST_CASE("结果和进度也往返得回来") {
    infer::TaskResult r;
    r.ok = false;
    r.error = "出首帧失败：显存不够";
    const auto rb = infer::task_result_from_json(infer::to_json(r));
    CHECK_FALSE(rb.ok);
    // 错误原因要能直接给用户看——它会变成事件流里那条 warn
    CHECK(rb.error == r.error);

    infer::TaskProgress p;
    p.state = "running";
    p.step = 3;
    p.steps = 8;
    p.phase = "decode";
    p.preview_step = 3;
    p.preview = "data:image/png;base64,AAAA";
    const auto pb = infer::task_progress_from_json(infer::to_json(p));
    CHECK(pb.state == "running");
    CHECK(pb.step == 3);
    CHECK(pb.steps == 8);
    // 准备、采样、解码要分得开，不然进度会从 1927/1927 跳回 1/8 再跳到 78/78
    CHECK(pb.phase == "decode");
    // 预览随进度带回来，镜头墙上才有活在别的机器上跑时的小图
    CHECK(pb.preview_step == 3);
    CHECK(pb.preview == "data:image/png;base64,AAAA");
    // 没带就是"没有"，别把空串当一张图
    infer::TaskProgress bare;
    const auto bb = infer::task_progress_from_json(infer::to_json(bare));
    CHECK(bb.preview_step == -1);
    CHECK(bb.preview.empty());
    CHECK_FALSE(pb.result.has_value());
}

TEST_CASE("种子必须由协调者给，不能让工作进程自己算") {
    // 工作进程不知道 attempts，自己算出来的种子和串行跑的不一样。
    // 那样并行就不是"更快"，是"结果变了"——而这一点没有任何报错会提醒你。
    auto j = infer::to_json(sample_frame());
    j.erase("seed");
    CHECK_THROWS_AS(infer::task_from_json(j), std::runtime_error);
}

TEST_CASE("多卡自动拉起：端口按卡号排，不会撞") {
    // 端口撞了的表现是"某张卡的工作进程起不来"，而日志里只有一句连不上，
    // 看不出是端口规则算错了。所以这条规则单独拿出来测。
    CHECK(infer::worker_port_for(9001, 0) == 9001);
    CHECK(infer::worker_port_for(9001, 7) == 9008);
    // 八张卡两两不同
    std::set<int> seen;
    for (int g = 0; g < 8; ++g) seen.insert(infer::worker_port_for(9001, g));
    CHECK(seen.size() == 8);
}

TEST_CASE("多卡自动拉起：这四种情况都不该动手") {
    // **不动手时要退回单卡进程内跑**，不是报错——多卡拉不起来该继续干活。
    models::HardwareProfile two;
    models::GPUInfo g;
    g.count = 2;
    two.gpu = g;

    SUBCASE("用户自己填了 endpoints（包括跨机）") {
        config::Settings s;
        s.workers.endpoints = {"http://别的机器:9001"};
        CHECK(infer::WorkerFarm::start(s, two) == nullptr);
    }
    SUBCASE("显式关掉了") {
        config::Settings s;
        s.workers.auto_spawn = false;
        CHECK(infer::WorkerFarm::start(s, two) == nullptr);
    }
    SUBCASE("只有一张卡：进程内跑更省事") {
        config::Settings s;
        models::HardwareProfile one;
        models::GPUInfo g1;
        g1.count = 1;
        one.gpu = g1;
        CHECK(infer::WorkerFarm::start(s, one) == nullptr);
    }
    SUBCASE("压根没探到显卡") {
        config::Settings s;
        models::HardwareProfile none;
        CHECK(infer::WorkerFarm::start(s, none) == nullptr);
    }
}

TEST_CASE("工作进程回的话拼进错误消息时按字符截，不按字节") {
    // 2026-09-11 实跑：json_extract 那条错误消息 substr(0, 400) 截在半个汉字上，
    // 进了任务快照之后 GET /api/script/series 序列化 JSON 直接 500，
    // 批量写正文的进度就再也看不见了。这两句话走的是同一条路
    // （事件流的 warn → 快照 → JSON），原来也是同样的写法。
    std::string body;
    for (int i = 0; i < 60; ++i) body += "显存不够，这一镜出不了图。";
    REQUIRE(text::utf8_len(body) > 200);
    // 按字节截 200 恰好落在半个汉字上（200 不是 3 的倍数）——
    // 这就是要防的那一下：不合法的 UTF-8 装进 json 一 dump 就抛。
    CHECK_THROWS(nlohmann::json(body.substr(0, 200)).dump());

    SUBCASE("拒了任务") {
        const std::string msg = infer::worker_rejected_message(503, body);
        CHECK(msg.find("拒了这个任务（503）") != std::string::npos);
        CHECK(msg.find("显存不够") != std::string::npos);
        CHECK_NOTHROW(nlohmann::json(msg).dump());
        CHECK(text::utf8_len(msg) < 250);
    }
    SUBCASE("回的不是 {\"id\":...}") {
        const std::string msg = infer::worker_bad_accept_message(body);
        CHECK(msg.find("{\"id\":...}") != std::string::npos);
        CHECK_NOTHROW(nlohmann::json(msg).dump());
        CHECK(text::utf8_len(msg) < 250);
    }
    SUBCASE("短的一个字不少") {
        CHECK(infer::worker_rejected_message(500, "坏了") ==
              "工作进程拒了这个任务（500）：坏了");
        CHECK(infer::worker_bad_accept_message("<html>") ==
              "工作进程回的不是 {\"id\":...}：<html>");
    }
}

TEST_CASE("配音任务：那几个字段要能往返") {
    infer::Task t;
    t.kind = infer::TaskKind::Tts;
    t.shot_id = "sh007";
    t.text = "雨夜的天台上，他没有回头。";
    t.voice_id = "female_01";
    t.emotion = "低沉";
    t.intensity = 0.7;
    t.dest = "C:/out/sh007.wav";
    t.return_artifact = true;

    const infer::Task back = infer::task_from_json(infer::to_json(t));
    CHECK(back.kind == infer::TaskKind::Tts);
    CHECK(back.text == t.text);
    CHECK(back.voice_id == t.voice_id);
    CHECK(back.emotion == t.emotion);
    CHECK(back.intensity == doctest::Approx(t.intensity));
    CHECK(back.return_artifact);
}

TEST_CASE("配音出来多长要能带回来") {
    // 配音先行那条线靠这个数反推镜头时长。只有跑活那台知道它——
    // 它顺手就量了，没道理让派活方再算一遍。
    infer::TaskResult r;
    r.ok = true;
    r.duration_s = 3.75;
    r.artifact_id = std::string(40, 'a');
    const infer::TaskResult back = infer::task_result_from_json(infer::to_json(r));
    CHECK(back.duration_s == doctest::Approx(3.75));
    CHECK(back.artifact_id == r.artifact_id);
}

TEST_CASE("非法 UTF-8 在入口就被判掉") {
    // nlohmann 解析时照单全收，dump 时才抛 type_error.316——那时候已经
    // 出了 try 块，派活方拿到的是一个空白的 500。2026-09-12 实撞过：
    // 请求体是 GBK 的中文台词，工作进程回 500，日志里只有一行
    // "invalid UTF-8 byte at index 174"。
    nlohmann::json j = infer::to_json([] {
        infer::Task t;
        t.kind = infer::TaskKind::Tts;
        t.shot_id = "sh001";
        t.dest = "C:/out/a.wav";
        return t;
    }());
    // GBK 的"雨夜"：两个字节都不是合法的 UTF-8 起始
    j["text"] = std::string("\xD3\xEA\xD2\xB9");
    CHECK_THROWS_AS(infer::task_from_json(j), std::runtime_error);

    // 合法的就该过
    j["text"] = "雨夜";
    CHECK_NOTHROW(infer::task_from_json(j));
}

TEST_CASE("这部电影挑的档位要跟着任务过去") {
    // **跨机时这是唯一一条让对面用对模型的路**：那台的模型目录在别处，
    // 路径带过去没有意义，只能带 id 让它自己解析。
    auto t = sample_frame();
    t.pick = {{"image", "qwen-image-edit-2509-q4_k_m"}, {"video", "h3-full-q8_0"}};
    const auto back = infer::task_from_json(infer::to_json(t));
    CHECK(back.pick == t.pick);
}

TEST_CASE("没挑过就别在请求体里留一个空对象") {
    // 日志里一个 "pick": {} 读着像"挑了、挑空了"，而这两件事后面要分。
    const auto j = infer::to_json(sample_frame());
    CHECK_FALSE(j.contains("pick"));
}

TEST_CASE("老派活方不带 pick，那时候要的就是老行为") {
    // 对面版本比这台老时根本没有这个键。当"没挑"——每台按自己 [models]
    // 里写的文件名跑，和以前一模一样。形状不对（不是对象）时同理。
    auto j = infer::to_json(sample_frame());
    CHECK(infer::task_from_json(j).pick.empty());

    j["pick"] = "不是对象";
    CHECK(infer::task_from_json(j).pick.empty());

    j["pick"] = {{"image", 42}, {"video", "这个是好的"}};
    const auto back = infer::task_from_json(j);
    CHECK(back.pick.count("image") == 0);
    CHECK(back.pick.at("video") == "这个是好的");
}

// ---------------------------------------------------------------------------
// 抢位置
// ---------------------------------------------------------------------------
//
// 用户 2026-09-20：「这个节点服务器应该先看这个机器有几个空闲卡然后派几个
// 任务」「派成功失败都要反馈」「任务结束了也要连接的全部机器广播，这样别的
// 机器也可以派任务，就看谁抢的快」。
//
// 那台放一个位置出来时把所有挂着的一起叫醒，谁先 POST 上来谁拿到。这里钉的
// 是派活这头**纯判断**的那一半：拿到几号版本、什么时候该去抢。

TEST_CASE("那台拒了：把它报的空位版本取出来") {
    CHECK(infer::room_seq_of(R"({"detail":"正忙","busy":true,"slots":2,"free":0,"seq":7})") == 7);
}

TEST_CASE("老版本的工作进程不报版本：当成 0，等于退化成一次短轮询") {
    // **不能因此卡住**。0 的意思是"我不知道你现在是第几版"，那头见到对不上
    // 的版本会立刻回——退回到改之前的节奏，而不是永远等下去。
    CHECK(infer::room_seq_of(R"({"detail":"正忙","busy":true})") == 0);
    CHECK(infer::room_seq_of("") == 0);
    CHECK(infer::room_seq_of("not json") == 0);
    CHECK(infer::room_seq_of("[1,2,3]") == 0);
}

TEST_CASE("有位置了就去抢") {
    std::uint64_t now = 0;
    CHECK(infer::room_freed(R"({"free":1,"slots":2,"seq":7})", 7, &now));
    CHECK(now == 7);
}

TEST_CASE("版本变了也去抢——哪怕这一刻又被别人占上了") {
    // **这一条最要紧。** 两台机器互相抢的时候，位置一空两边都被叫醒，
    // 慢的那台赶到时 `free` 已经是 0 了。只认 `free > 0` 的话，它会一直
    // 等在一个永远为 0 的数上，而那台其实一直在放位置、一直被别人抢走
    // ——这台就永远轮不上。版本变了就说明"刚才有过位置"，该再去试一次。
    std::uint64_t now = 0;
    CHECK(infer::room_freed(R"({"free":0,"slots":1,"seq":9})", 7, &now));
    CHECK(now == 9);   // 下一轮拿新版本去等
}

TEST_CASE("没位置、版本也没变：接着等") {
    std::uint64_t now = 0;
    CHECK_FALSE(infer::room_freed(R"({"free":0,"slots":1,"seq":7})", 7, &now));
    CHECK(now == 7);
}

TEST_CASE("那台答得乱七八糟：当成没变，让调用方按超时退回老节奏") {
    std::uint64_t now = 42;
    CHECK_FALSE(infer::room_freed("", 7, &now));
    CHECK_FALSE(infer::room_freed("<html>404</html>", 7, &now));
    CHECK_FALSE(infer::room_freed("[]", 7, &now));
    // 缺字段：版本按问的那个留着，不去抢
    CHECK_FALSE(infer::room_freed(R"({"slots":1})", 7, &now));
}

// ---------------------------------------------------------------------------
// 派活的那台没影了
// ---------------------------------------------------------------------------
//
// 用户 2026-09-20：「派活的这个机器派完后下线了，等任务完成都没上线，
// 怎么处理呢」。
//
// 槽是靠派活方回来收才放的。它一下线，这条记录就永远占着位置——而
// `idle` 量的是**派活方来问的间隔**，不是这件活跑了多久：人还在盯着的话
// 它每 5 秒就把这个数清零一次。

TEST_CASE("派活的那头没影了，不是取消的理由") {
    using namespace std::chrono;
    // **2026-09-20 实测撞到的那一次**：派活那台被杀掉之后，那台 L20 上
    // 那一镜已经采样了 125 秒、眼看就出来了，却因为「两分钟没人来问」被
    // 取消——125 秒的卡时间白烧，而结果本来是留得住的。
    //
    // 对的做法：让它跑完，放到一边，对方上线后自己来取。
    CHECK(infer::orphan_check(false, /*没人问*/ minutes(10),
                              /*跑了*/ minutes(3)) == infer::Orphan::Keep);
    CHECK(infer::orphan_check(false, hours(1), minutes(3)) ==
          infer::Orphan::Keep);
}

TEST_CASE("跑了太久才取消：那是卡住了，和谁在看没关系") {
    using namespace std::chrono;
    // 成片档一镜几分钟，2K 长镜头最坏十几分钟；半小时还没完就不是慢，
    // 是卡住了——那张卡再等下去也等不出东西。
    CHECK(infer::orphan_check(false, seconds(1), minutes(29)) ==
          infer::Orphan::Keep);
    CHECK(infer::orphan_check(false, seconds(1), minutes(31)) ==
          infer::Orphan::CancelStuck);
    // 派活方一直在盯着也一样取消：卡住了就是卡住了。
    CHECK(infer::orphan_check(false, seconds(0), hours(2)) ==
          infer::Orphan::CancelStuck);
}

TEST_CASE("放在一边的留一个钟头：它不占位置，留着是为了对方回来取") {
    using namespace std::chrono;
    // 算完那一刻位置就放出来了，所以留久一点没有代价——留的是一小块元数据，
    // 产物在 blob 库里按内容存着。用处是派活方重启之后照着同样的输入再派
    // 一次，指纹对得上就直接给结果，省掉整整一次渲染。
    CHECK(infer::orphan_check(true, minutes(59), seconds(0)) ==
          infer::Orphan::Keep);
    CHECK(infer::orphan_check(true, hours(2), seconds(0)) ==
          infer::Orphan::DropSettled);
}

TEST_CASE("同样的活算出同一把钥匙，落盘路径不算数") {
    // "对方上线后自己来取"全靠这个：它重启之后 dest 可能换了个临时目录，
    // 而那和产出的内容没关系——算进去的话就成了另一件活，白跑一遍。
    infer::Task a;
    a.kind = infer::TaskKind::Frame;
    a.shot_id = "ep01_sh003";
    a.prompts.positive = "雨夜天台";
    a.seed = 42;
    a.dest = "/tmp/a/ep01_sh003.png";
    infer::Task b = a;
    b.dest = "/var/folders/xx/别的地方/ep01_sh003.png";
    CHECK(infer::task_key(a) == infer::task_key(b));

    // 换了种子就是另一张图，不能拿上一次的顶替
    infer::Task c = a;
    c.seed = 43;
    CHECK(infer::task_key(a) != infer::task_key(c));
    // 换了提示词同理
    infer::Task d = a;
    d.prompts.positive = "晴天天台";
    CHECK(infer::task_key(a) != infer::task_key(d));
}
