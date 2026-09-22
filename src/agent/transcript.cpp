#include "agent/transcript.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace changji::agent {

std::int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

json to_json(const Turn& t) {
    json j;
    j["role"] = t.role;
    j["text"] = t.text;
    j["at"] = t.at;
    // 空的就不写。**一行一条的文件要耐读**：十几条里每条都拖着三个空串，
    // 人翻这个文件查问题时会先被噪音挡一道。
    if (!t.tool_name.empty()) j["tool_name"] = t.tool_name;
    if (!t.tool_id.empty()) j["tool_id"] = t.tool_id;
    if (!t.episode.empty()) j["episode"] = t.episode;
    if (!t.tool_calls.is_null() && !t.tool_calls.empty()) j["tool_calls"] = t.tool_calls;
    return j;
}

Turn turn_from_json(const json& j) {
    Turn t;
    t.role = j.value("role", "");
    t.text = j.value("text", "");
    t.at = j.value("at", static_cast<std::int64_t>(0));
    t.tool_name = j.value("tool_name", "");
    t.tool_id = j.value("tool_id", "");
    t.episode = j.value("episode", "");
    if (j.contains("tool_calls")) t.tool_calls = j.at("tool_calls");
    return t;
}

fs::path transcript_path(const fs::path& project_root,
                         const std::string& chat_id) {
    if (chat_id.empty()) return project_root / "chat.jsonl";
    return project_root / "chats" / (chat_id + ".jsonl");
}

std::vector<std::string> list_chat_ids(const fs::path& project_root) {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path dir = project_root / "chats";
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".jsonl") continue;
        out.push_back(e.path().stem().string());
    }
    // **排一下。** 目录序是文件系统说了算，同一份东西在两台机器上顺序不同，
    // 界面上那张表就会莫名其妙地换序。
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Turn> load_transcript(const fs::path& project_root,
                                  const std::string& chat_id) {
    std::vector<Turn> out;
    std::ifstream in(transcript_path(project_root, chat_id));
    if (!in) return out;   // 还没说过话，不是错

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            out.push_back(turn_from_json(json::parse(line)));
        } catch (const std::exception&) {
            // 坏行跳过。进程被杀在写到一半那一下，坏的就是最后这一行。
        }
    }
    return out;
}

bool remove_transcript(const fs::path& project_root, const std::string& chat_id) {
    std::error_code ec;
    return fs::remove(transcript_path(project_root, chat_id), ec);
}

void append_turn(const fs::path& project_root, const Turn& t,
                 const std::string& chat_id) {
    const fs::path file = transcript_path(project_root, chat_id);
    std::error_code ec;
    // **建的是那个文件所在的目录**，不是项目目录：新开的那几条落在
    // `chats/` 底下，而那个子目录第一次写之前不存在。
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::app);
    if (!out) return;
    out << to_json(t).dump() << "\n";
}

void write_transcript(const fs::path& project_root, const std::vector<Turn>& turns,
                      const std::string& chat_id) {
    const fs::path file = transcript_path(project_root, chat_id);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::trunc);
    if (!out) return;
    for (const auto& t : turns) out << to_json(t).dump() << "\n";
}

}  // namespace changji::agent
