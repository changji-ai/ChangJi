#include "llm/chat_pick.hpp"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "config/model_index.hpp"
#include "config/runtime.hpp"
#include "util/paths.hpp"
#include "util/say.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace changji::llm {

json to_json(const ChatPick& p) {
    json j = json::object();
    if (!p.backend.empty()) j["backend"] = p.backend;
    if (!p.base_url.empty()) j["base_url"] = p.base_url;
    if (!p.model.empty()) j["model"] = p.model;
    if (!p.file.empty()) j["file"] = p.file;
    if (p.effort.has_value() && !p.effort->empty()) j["effort"] = *p.effort;
    return j;
}

ChatPick pick_from_json(const json& j) {
    ChatPick p;
    if (!j.is_object()) return p;
    const auto str = [&](const char* k) {
        const auto it = j.find(k);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    p.backend = str("backend");
    if (p.backend != "remote" && p.backend != "local") p.backend.clear();
    p.base_url = str("base_url");
    p.model = str("model");
    p.file = str("file");
    // **空串不算挑过。** 2026-09-27 之前它是一档（「爱想不想」= 不发那个字段、随
    // 服务默认），而智谱那家的默认是 max：一条对话存着空串，拆分镜一场想八九万字、
    // 一刻钟到半小时，全局配的 low 一个字都管不到它。那一档去掉了；盘上还留着空串
    // 的旧文件按没挑读，落回全局和按任务分档的那张表。
    if (const auto it = j.find("effort");
        it != j.end() && it->is_string() && !it->get<std::string>().empty()) {
        p.effort = it->get<std::string>();
    }
    // 后端没认出来，地址、模型那几栏也就不作数了——留着的话 `remote` 的地址会
    // 被当成"没挑后端"那一种半截状态读回来。
    if (p.backend.empty()) {
        p.base_url.clear();
        p.model.clear();
        p.file.clear();
    }
    return p;
}

config::LLMConfig with_pick(const config::Settings& s, const ChatPick& p) {
    config::LLMConfig c = s.llm;
    if (p.backend == "remote" && !p.base_url.empty()) {
        c.backend = "remote";
        if (p.base_url != c.base_url) {
            c.base_url = p.base_url;
            c.api_key = config::read_api_key_for(p.base_url);
        }
        if (!p.model.empty()) c.model = p.model;
        c.task_models.clear();
    } else if (p.backend == "local") {
        c.backend = "local";
        c.local_weights.clear();
        if (!p.file.empty()) {
            const auto dir = s.models.dir_path(s.workspace_path());
            if (const auto hit = config::find_model(dir, p.file)) {
                c.local_weights = paths::to_utf8(*hit);
            }
        }
        c.task_models.clear();
    }
    if (p.effort.has_value() && !p.effort->empty()) {
        c.reasoning_effort = *p.effort;
        c.task_efforts.clear();
    }
    return c;
}

ChatPick pick_of(const config::Settings& s) {
    ChatPick p;
    if (s.llm.backend == "remote" && !s.llm.base_url.empty()) {
        p.backend = "remote";
        p.base_url = s.llm.base_url;
        p.model = s.llm.model;
    } else if (s.llm.backend == "local" && !s.models.llm.empty()) {
        p.backend = "local";
        p.file = s.models.llm;
        p.model = paths::to_utf8(paths::from_utf8(s.models.llm).stem());
    } else {
        return p;
    }
    if (!s.llm.reasoning_effort.empty()) p.effort = s.llm.reasoning_effort;
    return p;
}

namespace {

std::vector<std::optional<ChatPick>>& pick_stack() {
    thread_local std::vector<std::optional<ChatPick>> s;
    return s;
}

}  // namespace

std::optional<ChatPick> current_pick() {
    const auto& s = pick_stack();
    return s.empty() ? std::nullopt : s.back();
}

PickScope::PickScope(std::optional<ChatPick> p) { pick_stack().push_back(std::move(p)); }
PickScope::~PickScope() { pick_stack().pop_back(); }

config::LLMConfig effective_llm(const config::Settings& s) {
    const auto p = current_pick();
    return p ? with_pick(s, *p) : s.llm;
}

config::LLMConfig effective_llm() { return effective_llm(config::runtime().snapshot()); }

// ---- 落盘 ----

fs::path chat_root(const std::string& project) {
    return project.empty() ? paths::user_data_dir("changji") / "chat"
                           : paths::from_utf8(project);
}

fs::path chat_pick_path(const fs::path& root, const std::string& chat) {
    if (chat.empty()) return root / "chat.model.json";
    return root / "chats" / paths::from_utf8(chat + ".model.json");
}

namespace {

bool linked(const fs::path& file, const std::string& chat) {
    std::error_code ec;
    if (fs::is_symlink(file, ec)) return true;
    return !chat.empty() && fs::is_symlink(file.parent_path(), ec);
}

}  // namespace

std::optional<ChatPick> load_chat_pick(const fs::path& root, const std::string& chat) {
    const fs::path file = chat_pick_path(root, chat);
    if (linked(file, chat)) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    const json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return std::nullopt;
    const ChatPick p = pick_from_json(j);
    if (p.empty()) return std::nullopt;
    return p;
}

void save_chat_pick(const fs::path& root, const std::string& chat, const ChatPick& p) {
    const fs::path file = chat_pick_path(root, chat);
    std::error_code ec;
    if (p.empty()) {
        if (!linked(file, chat)) fs::remove(file, ec);
        return;
    }
    fs::create_directories(file.parent_path(), ec);
    if (!chat.empty() && fs::is_symlink(file.parent_path(), ec)) {
        throw std::runtime_error(SAY("这条对话的目录是一条链接，不往里写"));
    }
    fs::path tmp = file;
    tmp += ".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error(SAYF("无法写入：%1", paths::to_utf8(tmp)));
        }
        out << to_json(p).dump(1) << "\n";
        out.flush();
        if (!out.good()) {
            out.close();
            fs::remove(tmp, ec);
            throw std::runtime_error(SAYF("无法写入：%1", paths::to_utf8(tmp)));
        }
    }
    // rename 换掉的是那个位置本身；那儿是链接的话换掉的是链接，不是它指着的文件。
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw std::runtime_error(SAYF("无法写入：%1", paths::to_utf8(file)));
    }
}

void move_chat_pick(const fs::path& from, const fs::path& to, const std::string& chat) {
    const auto p = load_chat_pick(from, chat);
    if (!p) return;
    try {
        save_chat_pick(to, chat, *p);
    } catch (const std::exception&) {
        return;   // 新家写不进去：原来那份留着，别两头都丢
    }
    std::error_code ec;
    fs::remove(chat_pick_path(from, chat), ec);
}

void copy_chat_pick(const fs::path& root, const std::string& from_chat,
                    const std::string& to_chat) {
    const auto p = load_chat_pick(root, from_chat);
    if (!p) return;
    try {
        save_chat_pick(root, to_chat, *p);
    } catch (const std::exception&) {
    }
}

void remove_chat_pick(const fs::path& root, const std::string& chat) {
    const fs::path file = chat_pick_path(root, chat);
    if (linked(file, chat)) return;
    std::error_code ec;
    fs::remove(file, ec);
}

}  // namespace changji::llm
