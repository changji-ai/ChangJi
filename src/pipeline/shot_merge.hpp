#pragma once
// 出片那一轮把一章镜头写回盘上时，别把中途别人改的那几栏冲掉。
//
// 出片一轮手里那份镜头是**开跑那一刻**读的，而一轮要几十分钟到几小时。
// 这期间人在镜头页改了同一章的某一镜（改字幕、改台词、改首帧提示词），
// 或者对话里的场记调 `/api/shot` 改了一镜——原来存盘是「重新读一份、把
// 这一章的 shots 整格换成手里这份」，于是那一笔被静默冲回去，全程 200。
//
// 做法是三方比：记住**自己上一次写下去的那份**（开跑时就是盘上那份）。
// 存盘时逐镜逐栏比盘上的和它——不一样，就是这一轮以外的人改的，这一栏以
// 盘上的为准，并且**在这一轮之后的每一次存盘里一直保持**（不记住的话，
// 下一次存盘看盘上和自己上次写的又一样了，就又冲回去了）。
//
// 改了状态的一并带上重试次数和闸门备注：人改画面那几栏时 `/api/shot` 会把
// 这一镜退回 PLANNED——那是在说"手里这版片子是旧的，重出"。出片这一轮之后
// 把它推成 DRAFT_DONE 就又把人的意思抹掉了。
//
// 镜头的**名单**归这一轮：拆出来的新镜头、重排过的 order 照手里这份写。
// 盘上有而手里没有的镜头（这一轮里被拆掉或合并的）不回来。

#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "models/shot.hpp"

namespace changji::pipeline {

class ShotWriteBack {
public:
    /// `on_disk`：开跑那一刻盘上这一章的镜头（**在任何改动之前**，包括
    /// 开跑时那几笔归位——那几笔是这一轮自己的，不能被当成别人改的）。
    explicit ShotWriteBack(const std::vector<models::Shot>& on_disk) {
        for (const auto& s : on_disk) written_[s.shot_id] = nlohmann::json(s);
    }

    /// 拿手里这份 `ours` 和盘上刚读的 `disk` 合出要写下去的那份。
    /// 每调一次就把结果记成"自己上一次写下去的"。
    std::vector<models::Shot> merge(const std::vector<models::Shot>& ours,
                                    const std::vector<models::Shot>& disk) {
        std::map<std::string, const models::Shot*> by_id;
        for (const auto& s : disk) by_id[s.shot_id] = &s;

        std::vector<models::Shot> out;
        out.reserve(ours.size());
        for (const auto& mine : ours) {
            nlohmann::json j = mine;
            const auto d = by_id.find(mine.shot_id);
            const auto w = written_.find(mine.shot_id);
            if (d != by_id.end() && w != written_.end()) {
                const nlohmann::json theirs = *d->second;
                auto& keep = overrides_[mine.shot_id];
                for (auto it = theirs.begin(); it != theirs.end(); ++it) {
                    const auto was = w->second.find(it.key());
                    if (was != w->second.end() && *was == it.value()) continue;
                    keep[it.key()] = it.value();
                    if (it.key() == "status") {
                        for (const char* k : {"attempts", "gate_notes"}) {
                            if (theirs.contains(k)) keep[k] = theirs.at(k);
                        }
                    }
                }
            }
            const auto o = overrides_.find(mine.shot_id);
            if (o != overrides_.end()) {
                for (const auto& [k, v] : o->second) j[k] = v;
                if (!o->second.empty()) edited_.insert(mine.shot_id);
            }
            written_[mine.shot_id] = j;
            out.push_back(j.get<models::Shot>());
        }
        return out;
    }

    /// 这一轮里被别人改过、以别人的为准的那几镜。
    const std::set<std::string>& edited() const { return edited_; }

private:
    std::map<std::string, nlohmann::json> written_;
    std::map<std::string, std::map<std::string, nlohmann::json>> overrides_;
    std::set<std::string> edited_;
};

}  // namespace changji::pipeline
