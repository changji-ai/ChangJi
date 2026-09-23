#include "agent/attachments.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

#include <zlib.h>

#include "util/paths.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace changji::agent {

namespace {

/// 一件附件给模型看的正文最多多少**字**。一章小说三五千字，一部三十来万——
/// 整部贴进去的话这一句话就把上下文撑爆了，而且每一轮都要重发一次。
/// 截掉的那半照实说，模型会去问。
constexpr std::size_t kTextKeep = 20000;
/// 界面上那张文字卡摆多少字（收着时六行，点开看全——这儿只是个开头）。
constexpr std::size_t kCardKeep = 1500;

/// 给模型看的那几句**不翻**：它们是提示词的一部分（同 `agent/tools.cpp` 那一族），
/// 换一种界面语言就换一份提示词，而模型读中文本来就没问题。
const i18n::Audience kModel = i18n::Audience::model();

struct Kind {
    const char* ext;
    const char* kind;
};

/// **唯一的一张表。** 选文件框、拖进来那一层、引擎这头都问它。
constexpr Kind kKinds[] = {
    {"png", "image"},  {"jpg", "image"},   {"jpeg", "image"}, {"webp", "image"},
    {"bmp", "image"},  {"gif", "image"},
    {"mp4", "video"},  {"mov", "video"},   {"m4v", "video"},  {"webm", "video"},
    {"mkv", "video"},  {"avi", "video"},
    {"wav", "audio"},  {"mp3", "audio"},   {"m4a", "audio"},  {"flac", "audio"},
    {"ogg", "audio"},  {"aac", "audio"},
    {"txt", "text"},   {"md", "text"},     {"markdown", "text"}, {"json", "text"},
    {"csv", "text"},   {"tsv", "text"},    {"srt", "text"},   {"vtt", "text"},
    {"ass", "text"},   {"toml", "text"},   {"yaml", "text"},  {"yml", "text"},
    {"xml", "text"},   {"html", "text"},   {"htm", "text"},   {"log", "text"},
    {"fountain", "text"},
    {"docx", "doc"},
};

std::string ext_of(const std::string& path) {
    const std::string base = path.substr(path.find_last_of("/\\") + 1);
    const auto dot = base.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = base.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

std::string name_of(const std::string& path) {
    return path.substr(path.find_last_of("/\\") + 1);
}

bool read_all(const std::string& path, std::string& out) {
    std::ifstream in(paths::from_utf8(path), std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

/// 文件多大，说成人话。
std::string size_word(std::uintmax_t n) {
    char buf[32];
    if (n >= 1024ull * 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f GB", n / 1073741824.0);
    else if (n >= 1024ull * 1024) std::snprintf(buf, sizeof buf, "%.1f MB", n / 1048576.0);
    else if (n >= 1024) std::snprintf(buf, sizeof buf, "%.0f KB", n / 1024.0);
    else std::snprintf(buf, sizeof buf, "%u B", static_cast<unsigned>(n));
    return buf;
}

/// 字稿：UTF-8 就原样；**不是 UTF-8 的多半是 GBK**（中文 txt 小说大半是它）。
///
/// Windows 上交给系统按 GB18030 转；别的平台没有现成的转换表，把坏字节换掉，
/// 至少不让一段非法 UTF-8 一路漏到 JSON 那一步炸掉（text.hpp 上那段教训）。
std::string as_utf8(std::string raw) {
    // UTF-8 的 BOM 剥掉。
    if (raw.size() >= 3 && static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB && static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }
    if (text::is_valid_utf8(raw)) return raw;
#ifdef _WIN32
    const int wn = MultiByteToWideChar(54936, 0, raw.data(), static_cast<int>(raw.size()),
                                       nullptr, 0);
    if (wn > 0) {
        std::wstring w(static_cast<std::size_t>(wn), L'\0');
        MultiByteToWideChar(54936, 0, raw.data(), static_cast<int>(raw.size()), w.data(), wn);
        const int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
        if (un > 0) {
            std::string u(static_cast<std::size_t>(un), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, u.data(), un, nullptr, nullptr);
            return u;
        }
    }
#endif
    return text::sanitize_utf8(raw);
}

/// 一段字稿的正文（字稿原样、Word 稿抽正文）。读不出来回空串。
std::string text_of(const std::string& path, const std::string& kind) {
    if (kind == "doc") return docx_text(path);
    std::string raw;
    if (!read_all(path, raw)) return {};
    return as_utf8(std::move(raw));
}

std::string keep(const std::string& s, std::size_t n) {
    if (text::utf8_len(s) <= n) return s;
    return text::truncate_utf8(s, n) + "\n…";
}

// ---- docx：一个最小的 zip 读法 ----

std::uint32_t u32(const std::string& b, std::size_t at) {
    if (at + 4 > b.size()) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(b.data() + at);
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t u16(const std::string& b, std::size_t at) {
    if (at + 2 > b.size()) return 0;
    const auto* p = reinterpret_cast<const unsigned char*>(b.data() + at);
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

/// 从 zip 里取出一个文件。取不出来回 false。
bool unzip_one(const std::string& zip, const std::string& want, std::string& out) {
    // 中央目录的尾巴（EOCD）在最后 64 KB 里，倒着找它的签名。
    const std::size_t floor = zip.size() > 65557 ? zip.size() - 65557 : 0;
    std::size_t eocd = std::string::npos;
    for (std::size_t i = zip.size() >= 22 ? zip.size() - 22 : 0; ; --i) {
        if (u32(zip, i) == 0x06054b50) { eocd = i; break; }
        if (i == floor || i == 0) break;
    }
    if (eocd == std::string::npos) return false;
    const std::uint16_t count = u16(zip, eocd + 10);
    std::size_t at = u32(zip, eocd + 16);

    for (std::uint16_t n = 0; n < count; ++n) {
        if (u32(zip, at) != 0x02014b50) return false;
        const std::uint16_t method = u16(zip, at + 10);
        const std::uint32_t csize = u32(zip, at + 20);
        const std::uint32_t usize = u32(zip, at + 24);
        const std::uint16_t nlen = u16(zip, at + 28);
        const std::uint16_t xlen = u16(zip, at + 30);
        const std::uint16_t clen = u16(zip, at + 32);
        const std::uint32_t local = u32(zip, at + 42);
        const std::string name = zip.substr(at + 46, nlen);
        at += 46 + nlen + xlen + clen;
        if (name != want) continue;

        // 本地头后面才是数据；本地头自己的名字、附加字段长度可能和中央目录不一样。
        if (u32(zip, local) != 0x04034b50) return false;
        const std::size_t data = local + 30 + u16(zip, local + 26) + u16(zip, local + 28);
        if (data + csize > zip.size()) return false;
        if (method == 0) {
            out = zip.substr(data, csize);
            return true;
        }
        if (method != 8) return false;
        // 一个 Word 稿的正文再大也就几十兆，给个上限免得一个坏文件要一块天大的内存。
        if (usize > 256u * 1024 * 1024) return false;
        out.assign(usize, '\0');
        z_stream zs{};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(zip.data() + data));
        zs.avail_in = csize;
        zs.next_out = reinterpret_cast<Bytef*>(out.data());
        zs.avail_out = usize;
        const int rc = inflate(&zs, Z_FINISH);
        inflateEnd(&zs);
        if (rc != Z_STREAM_END) return false;
        out.resize(zs.total_out);
        return true;
    }
    return false;
}

/// `&amp;` 那几个实体换回来。
std::string unescape_xml(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') { out += s[i]; continue; }
        const auto semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 8) { out += s[i]; continue; }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out += '&';
        else if (ent == "lt") out += '<';
        else if (ent == "gt") out += '>';
        else if (ent == "quot") out += '"';
        else if (ent == "apos") out += '\'';
        else { out += s.substr(i, semi - i + 1); }
        i = semi;
    }
    return out;
}

}  // namespace

std::string attachment_kind(const std::string& path) {
    const std::string ext = ext_of(path);
    for (const Kind& k : kKinds) {
        if (ext == k.ext) return k.kind;
    }
    return {};
}

std::vector<std::string> attachment_extensions() {
    std::vector<std::string> out;
    for (const Kind& k : kKinds) out.emplace_back(k.ext);
    return out;
}

std::string docx_text(const std::string& path) {
    std::string zip;
    if (!read_all(path, zip)) return {};
    std::string xml;
    if (!unzip_one(zip, "word/document.xml", xml)) return {};

    // 只要字：`<w:t>…</w:t>` 里的是正文，`</w:p>` 是一段的尽头，
    // `<w:tab/>`、`<w:br/>` 照样子换成制表和换行。别的标签一律跳过。
    std::string out;
    for (std::size_t i = 0; i < xml.size();) {
        if (xml[i] != '<') {
            const auto next = xml.find('<', i);
            // 标签外面的字只在 <w:t> 里才算（下面进了 <w:t> 会直接取），这儿跳过。
            i = next == std::string::npos ? xml.size() : next;
            continue;
        }
        const auto close = xml.find('>', i);
        if (close == std::string::npos) break;
        const std::string tag = xml.substr(i + 1, close - i - 1);
        i = close + 1;
        if (tag == "w:t" || tag.rfind("w:t ", 0) == 0) {
            const auto end = xml.find("</w:t>", i);
            if (end == std::string::npos) break;
            out += unescape_xml(xml.substr(i, end - i));
            i = end + 6;
        } else if (tag == "/w:p") {
            out += '\n';
        } else if (tag == "w:tab/" || tag.rfind("w:tab ", 0) == 0) {
            out += '\t';
        } else if (tag == "w:br/" || tag.rfind("w:br ", 0) == 0) {
            out += '\n';
        }
    }
    return text::sanitize_utf8(out);
}

json attachment_media(const std::string& path) {
    const std::string kind = attachment_kind(path);
    json m;
    m["path"] = path;
    m["title"] = name_of(path);
    if (kind == "image" || kind == "video") {
        m["kind"] = kind;
    } else if (kind == "text" || kind == "doc") {
        m["kind"] = "text";
        const std::string body = text::strip_ws(text_of(path, kind));
        if (!body.empty()) m["text"] = keep(body, kCardKeep);
    } else {
        m["kind"] = "file";
    }
    return m;
}

std::string attachment_for_model(const std::string& path) {
    const std::string kind = attachment_kind(path);
    const std::string name = name_of(path);
    std::error_code ec;
    const fs::path p = paths::from_utf8(path);
    const bool here = fs::is_regular_file(p, ec);
    const std::string kind_word = kind == "image"   ? kModel("图片")
                                  : kind == "video" ? kModel("视频")
                                  : kind == "audio" ? kModel("音频")
                                  : kind == "doc"   ? kModel("Word 文档")
                                  : kind == "text"  ? kModel("文字稿")
                                                    : kModel("文件");
    if (!here) {
        // **照实说**：路径在人那台机器上，引擎在别的机器上时这儿读不到。
        return kModel.f("【附件】%1（%2）路径：%3——这台机器上读不到这个文件。",
                    name, kind_word, path);
    }
    const std::string size = size_word(fs::file_size(p, ec));
    std::string out = kModel.f("【附件】%1（%2，%3）路径：%4", name, kind_word, size, path);
    if (kind == "text" || kind == "doc") {
        const std::string body = text::strip_ws(text_of(path, kind));
        if (body.empty()) {
            out += "\n" + kModel("（读不出正文。）");
        } else {
            const std::size_t n = text::utf8_len(body);
            out += "\n" + kModel.f("正文（%1 字）：", std::to_string(n)) + "\n" + keep(body, kTextKeep);
            if (n > kTextKeep) {
                out += "\n" + kModel.f("（只贴了前 %1 字，后面还有。）", std::to_string(kTextKeep));
            }
        }
    }
    return out;
}

}  // namespace changji::agent
