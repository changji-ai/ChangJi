#include "infer/scheduler.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>

#include "util/paths.hpp"
#include "util/say.hpp"

namespace changji::infer {

namespace {

/// 借不到、但**是被另一件正在干的活挡住的**。
///
/// 和"这张卡真的装不下"分开，是因为两者的出路完全相反：前者等一等就好，
/// 后者等到天亮也没用，得去降画幅或换小模型。只有分得清，上一层才敢排队
/// ——对着一张装不下的卡排队，等于把一个立刻能看见的报错拖成三分钟的转圈。
struct SlotBusy {
    /// 挡路的是哪几个槽，拼好的。**给人看的名字**（`slot_label`）——它一路
    /// 走到顶栏那句「排队中，等「图像」用完」（`pipeline::note_queued`），
    /// 没有谁拿它当键比。
    std::string why;
    std::string message;  ///< 给用户看的那整段话
};

}  // namespace

const char* to_string(Slot s) {
    // ⚠️ 这是键，不是话。理由写在头文件上。
    switch (s) {
        case Slot::LLM:   return "LLM";
        case Slot::Image: return SAY_NEVER("图像");
        case Slot::Video: return SAY_NEVER("视频");
        case Slot::TTS:   return SAY_NEVER("配音");
    }
    return "?";
}

std::string slot_label(Slot s) {
    switch (s) {
        case Slot::LLM:   return "LLM";
        case Slot::Image: return SAY("图像");
        case Slot::Video: return SAY("视频");
        case Slot::TTS:   return SAY("配音");
    }
    return "?";
}

// ---- Lease ----

Lease::~Lease() { release(); }

Lease::Lease(Lease&& other) noexcept : sched_(other.sched_), slot_(other.slot_) {
    other.sched_ = nullptr;
}

Lease& Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release();
        sched_ = other.sched_;
        slot_ = other.slot_;
        other.sched_ = nullptr;
    }
    return *this;
}

void Lease::release() {
    if (sched_) {
        sched_->give_back(slot_);
        sched_ = nullptr;
    }
}

// ---- Scheduler ----

Scheduler::~Scheduler() {
    // 析构时把还加载着的都卸掉。引用计数不为零也卸——
    // 到这个时候进程要么在退出要么在重建调度器，留着更糟。
    std::lock_guard lg(mu_);
    for (Entry& e : entries_) {
        if (e.is_loaded) do_unload(e);
    }
}

void Scheduler::set_budget(std::size_t vram_bytes) {
    std::lock_guard lg(mu_);
    budget_ = vram_bytes;
}

std::size_t Scheduler::budget() const {
    std::lock_guard lg(mu_);
    return budget_;
}

void Scheduler::register_slot(SlotSpec spec) {
    std::lock_guard lg(mu_);
    for (Entry& e : entries_) {
        if (e.spec.slot == spec.slot) {
            if (e.is_loaded) {
                // 覆盖一个已加载的槽，新的 unload 会去卸一个不是它加载的东西。
                // 让它响亮地失败，比留一个对不上的账好。
                throw std::runtime_error(
                    SAYF("槽 %1 已经加载了，不能重新注册。先 evict 再注册。",
                         slot_label(spec.slot)));
            }
            e.spec = std::move(spec);
            return;
        }
    }
    Entry e;
    e.spec = std::move(spec);
    entries_.push_back(std::move(e));
}

Scheduler::Entry* Scheduler::find(Slot slot) {
    for (Entry& e : entries_) {
        if (e.spec.slot == slot) return &e;
    }
    return nullptr;
}

const Scheduler::Entry* Scheduler::find(Slot slot) const {
    for (const Entry& e : entries_) {
        if (e.spec.slot == slot) return &e;
    }
    return nullptr;
}

void Scheduler::do_unload(Entry& e) {
    if (!e.is_loaded) return;
    // unload 声明为不抛异常。真抛了也不能让它穿出去——
    // 这个函数在析构和驱逐路径上都被调用，那两条路上抛异常都是灾难。
    if (e.spec.unload) {
        try {
            e.spec.unload();
        } catch (...) {
            // 吞掉。账还是要平，否则后面的预算计算全错。
        }
    }
    e.is_loaded = false;
}

void Scheduler::set_free_vram_probe(FreeVramProbe probe) {
    std::lock_guard lg(mu_);
    free_vram_ = std::move(probe);
}

void Scheduler::record_measured_vram(Slot slot, std::size_t bytes,
                                     std::size_t work) {
    if (bytes == 0) return;
    MeasuredSink sink;
    {
        std::lock_guard lg(mu_);
        // **只往上记，不往下调。** 同一个槽不同镜头的占用会有出入（帧数、
        // 分辨率、有没有挂 LoRA），取见过的最大值才安全——按最近一次记的话，
        // 一个小镜头会把上限拉低，下一个大镜头就 OOM 了。
        //
        // work 同样只往上记，而且**和 bytes 各记各的**：
        // bytes 是见过的最大占用，work 是见过的最大的活。两者合起来说的是
        // "干到这么大的活为止，没见过超过这么多字节"——正好是用的时候要
        // 问的那句。分开记还避免一种长期悲观：先在 2K 量到 74 GB、后来在
        // 720p 量到 60 GB，如果 work 跟着 bytes 走就会被拉回 720p，
        // 此后每一镜 2K 都当没量过办。
        auto& cur = measured_[slot];
        const bool up = bytes > cur.bytes || work > cur.work;
        if (!up) return;   // 没长高，不用惊动落盘
        cur.bytes = std::max(cur.bytes, bytes);
        cur.work = std::max(cur.work, work);
        bytes = cur.bytes;
        sink = measured_sink_;
    }
    // **锁外调。** 落盘要写文件，拿着调度器的锁做 IO 会把别的借槽请求
    // 一起卡住，而借槽是出图出片的关键路径。
    if (sink) sink(slot, bytes);
}

std::map<Slot, Scheduler::Measured> Scheduler::all_measured() const {
    std::lock_guard lg(mu_);
    return measured_;
}

void Scheduler::set_measured_sink(MeasuredSink sink) {
    std::lock_guard lg(mu_);
    measured_sink_ = std::move(sink);
}

void Scheduler::set_total_vram(std::size_t bytes) {
    std::lock_guard lg(mu_);
    total_vram_ = bytes;
}

Scheduler::RoomDecision Scheduler::last_room_decision() const {
    std::lock_guard lg(mu_);
    return last_decision_;
}

std::string Scheduler::room_note(Slot slot) const {
    RoomDecision d;
    {
        std::lock_guard lg(mu_);
        d = last_decision_;
    }
    // 判的不是这个槽就别说——上一条很可能是别的阶段留下的，
    // 挂在这一镜下面会让人以为刚刚为它卸过模型。
    if (!d.valid || d.slot != slot) return {};
    // 压根没判过——槽本来就装着，画幅也没超过量过的。照实说，
    // 别说成"显存够"（那会让人以为刚做过一次判断）。
    if (d.already_loaded) return SAY("模型本来就装着，没动别的");
    if (!d.kept) {
        return SAYN("腾显存：卸了 %n 个模型", d.evicted);
    }
    // **"够"是按估算判的时候必须说出来。** 出片这一路的估算被实测推翻过
    // 两次，都是往小了错五倍，而判错的后果是 CUDA OOM 把整个服务带走。
    // 用户看到这句就知道：这一镜是在没量过的情况下赌了一把。
    return d.live_measured ? SAY("显存够，没动别的模型")
                           : SAY("显存够（按估算判的），没动别的模型");
}

std::size_t Scheduler::measured_vram(Slot slot) const {
    std::lock_guard lg(mu_);
    const auto it = measured_.find(slot);
    return it == measured_.end() ? 0 : it->second.bytes;
}

std::string out_of_vram_message(Slot slot) {
    // **每个槽给的出路不一样。** 只说一句"显存不够"的话，用户下一步
    // 无从下手——尤其是配音：它是唯一一个有现成外部服务可换的，
    // 而那条路不用改一行代码，改个配置就行。
    const std::string base =
        SAYF("显存不够加载 %1。腾不出空间——别的槽正被借用着，或者这张卡"
             "确实太小。",
             slot_label(slot));
    switch (slot) {
        case Slot::TTS:
            return base +
                   SAY("\n配音这一步**不必占显存**：把 changji.toml 里的 "
                       "[tts].backend 改成 \"http\"，再填 [tts].base_url "
                       "指向一个外部配音服务，本机就不用装配音模型了。"
                   // **别提 comfy。** 那条路 2026-09-10 拆了，现在填它
                   // 连配置校验都过不去。把人指到一个不存在的取值上，
                   // 比只说一句"显存不够"更糟。
                       "\n（另一个取值是 \"local\"，就是现在这条、进程内跑的。）");
        case Slot::LLM:
            // ⚠️ **这一支跑不到。** 2026-09-14 起没有任何东西注册这一档
            // （见 scheduler.hpp 上 Slot::LLM 那段：进程内大模型删了，编剧
            // 只走外接 API，`loaded(Slot::LLM)` 恒为 false）——借不到的槽
            // 首先得有人去借。留着 case 是因为这个 switch 没有 default，
            // 删了会触发 -Wswitch。
            //
            // 话还是要对：原来前半句写的是「在设置页把大模型的「跑在哪」改成
            // 外接 API」，那两样都没了——大模型那一节 2026-09-14 从设置页搬去
            // 了项目页那个弹窗，而「跑在哪」这个开关随着进程内那条路一起删了
            // （引擎只收 backend = "remote"）。后半句「填 [llm].base_url」
            // 到今天仍然成立，留着。
            return base +
                   SAY("\n写剧本这一步本来就不占显存：进程内跑大模型那条路已经"
                       "删了，编剧一律走外接 API——[llm].base_url 填任何一个兼容 "
                       "OpenAI 接口的服务都行，本地的云上的都可以。");
        case Slot::Image:
        case Slot::Video:
            // 分辨率**不在设置页**：2026-09-10 搬到项目页上。指错地方用户
            // 会在设置页翻半天。**但那儿不叫「画面」那张卡**——项目页上是
            // 一行，标题写着「这部电影」（整行是个按钮，点哪儿都开那个窗，
            // 窗名「这部电影长什么样」）。界面上没有一张叫「画面」的卡。
            //
            // **档位的名字要和界面上写的一样。** 取值仍然叫 "720p"（存在
            // 项目的 changji.toml 里，改了名老项目读不出来），但那个字符串
            // 不该出现在给人看的话里——2026-09-10 画幅从 704×1280 改成
            // 544×928 之后，再管它叫 720p 就是假的（720p 是 720 行）。
            //
            // **尺寸跟着画幅翻。** 下拉框里那几个数是
            // `qualitySize(档位, orientation)` 现算的：竖屏是短边×长边，
            // 横屏反过来。这句话原来把 2K 写成 2560×1440——那是**横屏**的
            // 写法，而前两档写的又是竖屏的（544×928、704×1280），三个数
            // 一句话里混着两种画幅，而默认的竖屏项目在下拉框里看到的是
            // 1440×2560。统一按竖屏写，并说明横屏反过来。
            return base +
                   SAY("\n出图出片是躲不掉的显存开销。能调的两处：在项目页"
                       "「这部电影」那一行点开，把清晰度降一档——三档分别是"
                       "「标准（544×928）」「高清（704×1280）」「2K（1440×2560）」"
                       "（竖屏项目是这几个数，横屏项目宽高反过来），"
                   // **别再教人手填 weights。** 它默认是 "smart"，装不下时
                   // 程序自己就会把权重放内存——用户 2026-09-10 的原话：
                   // "都应该让程序自己算"。这里只对"手动写死过"的人有意义。
                       "或者检查 [models].weights 是不是被手动写死了——"
                       "它默认 \"smart\"，会按这张卡和模型大小自己决定权重放哪，"
                       "写死成别的值就把这份判断关掉了。");
    }
    return base;
}

bool Scheduler::measurement_covers(Slot slot, std::size_t work) const {
    if (work == 0) return true;   // 调用方没说这次多大，按老规矩认
    const auto it = measured_.find(slot);
    return it != measured_.end() && it->second.bytes > 0 &&
           it->second.work >= work;
}

bool Scheduler::make_room(std::size_t need, Slot keep, std::size_t work) {
    if (budget_ == 0) return true;  // 不限制

    std::size_t used = 0;
    for (const Entry& e : entries_) {
        if (e.is_loaded) used += e.spec.vram_estimate;
    }
    if (used + need <= budget_) {
        // **这一支也要留痕。** 不记的话界面上显示的还是更早那次的结论，
        // 而那次很可能是"卸了"——用户看着以为刚才又卸了一回，实际这次
        // 根本没压力。留一条"够，没动"比留一条过期的准。
        last_decision_ = RoomDecision{true,  keep, need, need, 0,
                                      false, /*kept=*/true, 0,
                                      /*live_measured=*/false};
        return true;
    }

    // **静态估算说装不下之前，先真去问一眼卡上还空着多少。**
    //
    // vram_estimate 是每个槽按整份预算估的（"同时只装得下一个"），
    // 那是保守的：权重放内存时显存里其实只有计算缓冲，两个槽同时在也没事。
    // 只信估算的话，每次切阶段都要卸一个再装一个——一次重装是几十秒到几分钟，
    // 而卡上可能一直空着一大半。
    //
    // 问不到就退回估算。**"问不到"不等于"没空间"**，但那时也没有更好的依据。
    // ⚠️ 下面这一族 `[vram]` 只有 `CHANGJI_DEBUG_VRAM` 开着才打，而且走
    // stderr：**是日志不是界面**，不包 `SAY()`。同 `sd_image.cpp` 里
    // `[出片]` / `[出图]` 那几条。
    const bool dbg = !paths::env("CHANGJI_DEBUG_VRAM").empty();
    const FreeVramProbe probe = free_vram_;
    if (dbg) {
        std::fprintf(stderr,
                     "[vram] make_room slot=%s used=%.1fG need=%.1fG budget=%.1fG probe=%s\n",
                     to_string(keep), used / 1073741824.0, need / 1073741824.0,
                     budget_ / 1073741824.0, probe ? SAY_NEVER("有") : SAY_NEVER("没装"));
    }
    // 这个槽跑起来到底要多少：实测优先，没量过才退回估算。
    // 估算那条只会让我们更保守——多卸一次，不会 OOM。
    const Entry* self = find(keep);
    std::size_t live = need;
    bool live_measured = false;
    // **量过的活得不小于这次要干的活，那个数才算数。**
    //
    // 画幅档位从 544×928 到 2560×1440 差七倍多，占用跟着画幅和帧数走。
    // 在 720p 量到的数拿去给 2K 判"够，不卸"，是拿一个偏小的数去赌，
    // 赌输了是 CUDA OOM——abort() 把整个服务带走，不是能读的报错。
    //
    // work == 0 表示调用方没说这次多大（大模型、配音这些和画幅无关），
    // 那就按老规矩认。measured work == 0 是老持久化文件里的数，不知道
    // 当时量的是多大的活，这次又明确说了大小 —— 不认，跑一镜就自己补上。
    const auto usable = [&](const Measured& m) {
        if (m.bytes == 0) return false;
        if (work == 0) return true;
        return m.work >= work;
    };
    if (const auto it = measured_.find(keep);
        it != measured_.end() && usable(it->second)) {
        live = it->second.bytes;
        live_measured = true;
    } else if (const auto it2 = measured_.find(keep);
               it2 != measured_.end() && it2->second.bytes > 0 && work > 0 &&
               it2->second.work > 0 && it2->second.work < work) {
        // **量过、但量的是个更小的活：按活的比例放大，不要扔掉退回估算。**
        //
        // 2026-09-13 q4_full 实见：出片刚在 73 帧的镜头上量到 24 GB，下一镜
        // 106 帧，活更大、量到的数"不算数"，于是退回静态估算——那个数比
        // 卡上剩的 9.5 GB 还小，判成"够，不卸"，首帧那 16 GB 的图像模型留
        // 在卡上，sd.cpp 当场 "cannot make enough memory"，三次全败降级。
        // 而实测峰值随活只会涨不会跌（H3 分段跑，帧数多了显存其实不涨，
        // 画幅大了会涨），按比例放大是偏保守的那一边：多卸一次，不会 OOM。
        const double scale = static_cast<double>(work) /
                             static_cast<double>(it2->second.work);
        double scaled = static_cast<double>(it2->second.bytes) * scale;
        if (total_vram_ > 0 && scaled > static_cast<double>(total_vram_)) {
            scaled = static_cast<double>(total_vram_);
        }
        live = static_cast<std::size_t>(scaled);
        // 放大过的不算"量到的"：决策记录里仍写"估的"——它确实是推的，
        // 只是推的起点是实测而不是那张静态表。
        live_measured = false;
    } else if (self && self->spec.live_vram) {
        // 每次现问：模型可能已经被换过了。见 SlotSpec::live_vram。
        const std::size_t got = self->spec.live_vram();
        if (got > 0) live = got;
    }

    // 先问卡。
    std::optional<std::size_t> free_bytes;
    bool probed = false;
    if (probe) {
        if (const auto free_gb = probe(); free_gb.has_value()) {
            free_bytes = static_cast<std::size_t>(
                *free_gb * 1024.0 * 1024.0 * 1024.0);
            probed = true;
        }
    }
    if (dbg) {
        std::fprintf(stderr, SAY_NEVER("[vram]   探到空闲=%s\n"),
                     free_bytes ? (std::to_string(*free_bytes / 1073741824.0)
                                   + "G").c_str()
                                : SAY_NEVER("问不到"));
    }

    // **问不到就用量到的推算。**
    //
    // nvidia-smi 不是永远问得到：free_vram_gb 是 fork + exec 去跑它的，
    // 而这个进程初始化 CUDA 之后映射着十几 GB，在这种进程里 fork 本来就是
    // NVIDIA 明确不支持的做法。以前问不到就一路走到驱逐，于是
    // "显存够就不清理"在真机上等于从来没生效过。
    //
    // 推算优先用**已经量到的**数。没量过的槽退回它自己的老实数估算，
    // 而且只往高了算——把别人占的算大，推出来的空闲就偏小，顶多多卸一次，
    // 不会因为算多了空闲去撞 OOM。
    //
    // **这条估算的口子是必须留的。** 大模型那份实测是"装之前问一次、装完
    // 再问一次"的差值，问的还是同一个 nvidia-smi。探针问不到的时候那两次
    // 也一样问不到，大模型就永远没有实测值——只认实测的话，这条推算在
    // "探针失灵"这个它唯一要救的场景里从来不会生效。
    //
    // 两样都没有才放弃推算、回到保守那条去卸。
    if (!free_bytes && total_vram_ > 0) {
        std::size_t others = 0;
        bool all_known = true;
        for (const Entry& e : entries_) {
            if (!e.is_loaded || e.spec.slot == keep) continue;
            std::size_t take = 0;
            if (const auto it = measured_.find(e.spec.slot);
                it != measured_.end() && it->second.bytes > 0) {
                // 这里不卡 work：算的是**别人现在占了多少**，那和这次
                // 要干多大的活无关，而且已经装在卡上了，量到多少就是多少。
                take = it->second.bytes;
            } else if (e.spec.live_vram) {
                // 每次现问，理由同上面那处：模型可能已经被换过了。
                take = e.spec.live_vram();
            }
            if (take == 0) {
                all_known = false;
                break;
            }
            others += take;
        }
        if (all_known && others < total_vram_) {
            free_bytes = total_vram_ - others;
            if (dbg) {
                std::fprintf(stderr,
                             SAY_NEVER("[vram]   问不到，按量到/估到的推算"
                                       "空闲=%.1fG（总量 %.1fG − 别人占的 "
                                       "%.1fG）\n"),
                             *free_bytes / 1073741824.0,
                             total_vram_ / 1073741824.0,
                             others / 1073741824.0);
            }
        } else if (dbg) {
            std::fprintf(stderr,
                         SAY_NEVER("[vram]   问不到，且有槽既没量过也估不出，推算不了 -> 保守驱逐\n"));
        }
    }

    if (free_bytes) {
        if (dbg) {
            std::fprintf(stderr, SAY_NEVER("[vram]   老实数=%.1fG（%s） 空闲=%.1fG -> %s\n"),
                         live / 1073741824.0,
                         live_measured ? SAY_NEVER("量到的") : SAY_NEVER("估的，没量过"),
                         *free_bytes / 1073741824.0,
                         live <= *free_bytes ? SAY_NEVER("够，不卸") : SAY_NEVER("不够，要卸"));
        }
        if (live <= *free_bytes) {
            last_decision_ = RoomDecision{true, keep, need, live, *free_bytes,
                                          probed, /*kept=*/true, 0,
                                          live_measured};
            return true;
        }
    }

    // 候选：已加载、没被借用、不是要保住的那个。
    //
    // 被借用的一律不动——正在用的模型被抽走，表现是段错误，
    // 而不是一个能读的报错。宁可这次加载失败。
    std::vector<Entry*> cands;
    // **谁因为正被借用而没能成为候选**，记下来。腾不出地方时这一条决定了
    // 对用户说什么：「等它干完再点一次」和「这张卡太小，换个小模型」
    // 是两件完全不同的事，而以前只笼统说一句"别的槽正被借用着，或者
    // 这张卡确实太小"，用户没法判断该等还是该改配置。
    //
    // 会撞上的典型场景：一边在写字（大模型借着），一边点出片。等它写完
    // 那一章就腾得出来了，所以那句话是"等那件事干完，过会儿再点一次"。
    busy_.clear();
    for (Entry& e : entries_) {
        if (!e.is_loaded || e.spec.slot == keep) continue;
        if (e.leases > 0) {
            busy_.push_back(e.spec.slot);
            continue;
        }
        cands.push_back(&e);
    }

    // 先按驱逐优先级（小的先走），同优先级按最久没用的先走。
    std::sort(cands.begin(), cands.end(), [](const Entry* a, const Entry* b) {
        if (a->spec.evict_priority != b->spec.evict_priority) {
            return a->spec.evict_priority < b->spec.evict_priority;
        }
        return a->last_used < b->last_used;
    });

    int evicted = 0;
    for (Entry* e : cands) {
        if (used + need <= budget_) break;
        const std::size_t freed = e->spec.vram_estimate;
        do_unload(*e);
        used -= std::min(used, freed);
        ++evicted;
    }
    // 记下这一次是怎么判的，界面上读得到。见 RoomDecision。
    last_decision_ = RoomDecision{true,
                                  keep,
                                  need,
                                  live,
                                  free_bytes ? *free_bytes : 0,
                                  probed,
                                  /*kept=*/false,
                                  evicted,
                                  live_measured};
    return used + need <= budget_;
}

Lease Scheduler::acquire_once(Slot slot, std::size_t work) {
    // **装模型这件事，一次只能有一个在干。** 见 load_mu_ 上那段——两个槽
    // 同时往卡上装，第二个撞到的是 abort()，不是一个能读的报错。
    //
    // 锁在最外面，整个 acquire 持着（包括慢吞吞的 load）。这样
    // make_room 问到的空闲显存到真正分配那一刻还作数——中间没人插得进来。
    std::lock_guard load_lk(load_mu_);
    std::unique_lock lk(mu_);
    Entry* e = find(slot);
    if (!e) {
        throw std::runtime_error(SAYF("槽 %1 还没注册", slot_label(slot)));
    }

    e->last_used = ++clock_;

    if (e->is_loaded) {
        // **装着不等于这次跑得下。**
        //
        // 同一个槽，画幅一换要占的显存差好几倍（标准 544×928 到 2K
        // 2560×1440 差七倍多）。而"装着就直接放行"会让画幅门
        // （见 record_measured_vram）在**最常见的那条路上完全不生效**：
        // 标准档跑过一镜（模型已装、已量），用户切成 2K 再出片，槽是装着
        // 的，于是一次判断都不做——大模型留在显存里，下一步 OOM。
        //
        // 所以量过的活不够大时，重新腾一次地方。**不看返回值**：模型已经
        // 装着了，腾不出来也只能照跑（和以前一样），但能腾就腾——
        // 这样只会比以前多卸一个该卸的，不会把原来跑得通的变成报错。
        //
        // **量过的活够大也要看一眼卡上现在还剩多少**（2026-09-13 q4_full
        // sh004 实见）：出片槽早就装着、峰值也量过（独占时 24.9 GB），
        // 中间跑了首帧阶段，图像模型 16 GB 也装上了；回头出片时槽是装着的、
        // 量过的活也够大，于是一次判断都不做——卡上只剩 9.5 GB，sd.cpp
        // 当场 "cannot make enough memory"，三次全败降级。量到的峰值是
        // 独占时的数，别的槽后来装了多少它不知道，只有 make_room 里那道
        // "问卡"能知道。所以只要说了这次多大的活，就重走一遍 make_room。
        if (work > 0) {
            (void)make_room(e->spec.vram_estimate, slot, work);
        } else {
            // **这一镜什么都没干，就不能让上一镜的结论继续挂在那儿。**
            // 上一镜要是卸过模型，进度条上这一镜会凭空多出一句
            // "腾显存：卸了 1 个模型"，用户看到的是每镜都在卸，
            // 回头问"不是说够就不清理吗"。
            const auto it = measured_.find(slot);
            const std::size_t live =
                it == measured_.end() ? 0 : it->second.bytes;
            last_decision_ = RoomDecision{true,
                                          slot,
                                          e->spec.vram_estimate,
                                          live,
                                          0,
                                          /*probed=*/false,
                                          /*kept=*/true,
                                          0,
                                          /*live_measured=*/live > 0,
                                          /*already_loaded=*/true};
        }
        ++e->leases;
        return Lease(this, slot);
    }

    if (!make_room(e->spec.vram_estimate, slot, work)) {
        // **先说是被谁挡住的。** 有槽正借着的时候，"再等等"往往就好了，
        // 而通用那段会把人引去降画幅、换小模型——白折腾。
        // 没有"起服务时预装"这回事了——用的时候才装。所以挡路的一定是
        // 另一件真在干的活，让人等它干完，比让人去调参数有用。
        std::string why;
        for (const Slot b : busy_) {
            why += (why.empty() ? std::string{} : SAY("、")) + slot_label(b);
        }
        if (!why.empty()) {
            // **内部异常**：上一层看见它才知道"这件事等得到"，从而去排队。
            // 抛 runtime_error 的话上一层分不清该等还是这张卡真的装不下。
            throw SlotBusy{
                why,
                SAYF("显存不够加载 %1：「%2」正用着，腾不动它。\n"
                     "等那件事干完就腾出来了（出一张图几十秒，出一个镜头一两"
                     "分钟），过会儿再点一次。\n"
                     "要是一直这样，才是这张卡真的装不下——那时候看下面这些"
                     "路：\n",
                     slot_label(slot), why) +
                    out_of_vram_message(slot)};
        }
        throw std::runtime_error(out_of_vram_message(slot));
    }

    // 加载可能很慢（要读几个 GB），不能一直占着锁。
    // 先标成已加载再解锁，防止另一个线程同时也去加载同一个槽。
    e->is_loaded = true;
    ++e->leases;
    auto load_fn = e->spec.load;
    lk.unlock();

    if (load_fn) {
        try {
            load_fn();
        } catch (...) {
            // 加载失败，把账退回去。**不自动重试也不自动加载回被驱逐的槽**——
            // 那会把一次失败变成一串连锁加载，而且日志里看不出源头。
            std::lock_guard lg(mu_);
            Entry* again = find(slot);
            if (again) {
                again->is_loaded = false;
                if (again->leases > 0) --again->leases;
            }
            throw;
        }
    }
    return Lease(this, slot);
}

// ---------------------------------------------------------------------------
// 排队
// ---------------------------------------------------------------------------

int Scheduler::queue_ahead(std::uint64_t ticket) const {
    int ahead = 0;
    for (const std::uint64_t t : waiters_) {
        if (t == ticket) return ahead;
        ++ahead;
    }
    return 0;  // 不在队里就是排头（拿到了或者退了票）
}

void Scheduler::leave_queue(std::uint64_t ticket) {
    {
        std::lock_guard lg(mu_);
        for (auto it = waiters_.begin(); it != waiters_.end(); ++it) {
            if (*it == ticket) {
                waiters_.erase(it);
                break;
            }
        }
    }
    // 队伍动了就叫醒所有人：新的排头要去试一次。
    queue_cv_.notify_all();
}

Lease Scheduler::acquire(Slot slot, std::size_t work) {
    try {
        return acquire_once(slot, work);
    } catch (const SlotBusy& b) {
        // 不排队这条路上，"被占着"和"卡太小"对调用方是一回事：都得现在
        // 就回一句话。
        throw std::runtime_error(b.message);
    }
}

Lease Scheduler::acquire(Slot slot, const AcquireOptions& opt) {
    if (opt.wait <= std::chrono::milliseconds::zero()) {
        return acquire(slot, opt.work);
    }

    const auto deadline = std::chrono::steady_clock::now() + opt.wait;

    std::uint64_t ticket = 0;
    {
        std::lock_guard lg(mu_);
        ticket = next_ticket_++;
        waiters_.push_back(ticket);
    }
    // **退票必须万无一失。** 半路抛异常（加载失败、上层取消）不退票的话，
    // 队伍里留一张永远排在前面的死票，后面所有人一律超时——而那时候机器
    // 明明是闲着的，从现象上完全看不出是排队的问题。
    struct Ticket {
        Scheduler* s;
        std::uint64_t t;
        ~Ticket() { s->leave_queue(t); }
    } guard{this, ticket};

    // 只在情况变了的时候通知上层：不然界面上那句话每醒一次重写一遍，
    // 而它多半一个字都没变。
    bool said = false;
    int said_ahead = 0;
    std::string said_blocker;
    const auto tell = [&](int ahead, const std::string& blocker) {
        if (!opt.on_queued) return;
        if (said && ahead == said_ahead && blocker == said_blocker) return;
        said = true;
        said_ahead = ahead;
        said_blocker = blocker;
        opt.on_queued(ahead, blocker);
    };

    std::string last_message;
    for (;;) {
        int ahead = 0;
        {
            std::unique_lock lk(mu_);
            ahead = queue_ahead(ticket);
            if (ahead > 0) {
                // 还没轮到。**等的时候不去试**——试了就是插队，而插队一多
                // 就会有人一直排不上。
                queue_cv_.wait_until(lk, deadline);
            }
        }
        if (ahead > 0) {
            tell(ahead, "");
            if (std::chrono::steady_clock::now() >= deadline) break;
            continue;
        }

        // 轮到自己了，去试。
        try {
            Lease got = acquire_once(slot, opt.work);
            // **排完了要说一声。** 不说的话界面上那句"排队中"会一直挂着，
            // 而活其实已经在干了——比不显示更误导。
            tell(-1, "");
            return got;
        } catch (const SlotBusy& b) {
            last_message = b.message;
            tell(0, b.why);
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        // 排头也借不到：显存被正在干的活占着。等它还回来，或者等到超时。
        // **这里必须带上限**：还回槽时会 notify，但万一那件活是被 kill 掉
        // 的（没走到 give_back），没有上限就永远醒不过来。
        std::unique_lock lk(mu_);
        queue_cv_.wait_until(lk, deadline);
    }

    if (!last_message.empty()) throw std::runtime_error(last_message);
    // 一次都没轮到自己。**这句话不能说成"显存不够"**——显存够不够根本
    // 还没轮到我们去问，说它就是在猜。
    throw std::runtime_error(
        SAYF("排了一会儿还没轮到 %1：前面的活还没干完。\n"
             "等它们干完再点一次就行；要是一直排不上，多半是有一件长任务"
             "（出片、写全片）一直占着卡。\n"
             "顶栏那块「AI 作业中」点开能看到现在在跑什么。",
             slot_label(slot)));
}

void Scheduler::give_back(Slot slot) {
    std::lock_guard lg(mu_);
    Entry* e = find(slot);
    if (!e || e->leases <= 0) return;
    --e->leases;
    // 用完就放的槽，最后一个借用还回来时立刻卸载。
    // 留着纯粹是占地方——它按定义不会再被用了。
    if (e->leases == 0 && e->spec.residency == Residency::Ephemeral) {
        do_unload(*e);
    }
    // **还回来了就叫醒排队的。** 排头正等着的就是这一下。
    queue_cv_.notify_all();
}

bool Scheduler::evict(Slot slot) {
    {
        std::lock_guard lg(mu_);
        Entry* e = find(slot);
        if (!e) return false;
        if (e->leases > 0) return false;  // 正在用，不强卸
        do_unload(*e);
    }
    queue_cv_.notify_all();  // 腾出地方了，排队的该去试一次
    return true;
}

void Scheduler::evict_all() {
    std::lock_guard lg(mu_);
    for (Entry& e : entries_) {
        if (e.leases == 0) do_unload(e);
    }
    // **卸干净之后那条判断就过期了。** 它说的是"上一次要不要腾地方"，
    // 而现在什么都没装着，界面上再显示"刚才卸了 1 个模型"是在说一件
    // 已经不成立的事。
    //
    // 顺带这也是测试唯一的复位钩子：不清的话，一个用例摆的局面会顺着
    // 全局单例漏给下一个用例——下一个要是逐字比对进度文案，就会被凭空
    // 多出来的"（腾显存：卸了 1 个模型）"挂掉，而且看不出是谁干的。
    last_decision_ = RoomDecision{};
    busy_.clear();
    queue_cv_.notify_all();
}

bool Scheduler::loaded(Slot slot) const {
    std::lock_guard lg(mu_);
    const Entry* e = find(slot);
    return e && e->is_loaded;
}

int Scheduler::lease_count(Slot slot) const {
    std::lock_guard lg(mu_);
    const Entry* e = find(slot);
    return e ? e->leases : 0;
}

std::size_t Scheduler::resident_bytes() const {
    std::lock_guard lg(mu_);
    std::size_t used = 0;
    for (const Entry& e : entries_) {
        if (e.is_loaded) used += e.spec.vram_estimate;
    }
    return used;
}

std::vector<Slot> Scheduler::loaded_slots() const {
    std::lock_guard lg(mu_);
    std::vector<const Entry*> loaded;
    for (const Entry& e : entries_) {
        if (e.is_loaded) loaded.push_back(&e);
    }
    std::sort(loaded.begin(), loaded.end(), [](const Entry* a, const Entry* b) {
        if (a->spec.evict_priority != b->spec.evict_priority) {
            return a->spec.evict_priority < b->spec.evict_priority;
        }
        return a->last_used < b->last_used;
    });
    std::vector<Slot> out;
    out.reserve(loaded.size());
    for (const Entry* e : loaded) out.push_back(e->spec.slot);
    return out;
}

Scheduler& scheduler() {
    static Scheduler s;
    return s;
}

}  // namespace changji::infer
