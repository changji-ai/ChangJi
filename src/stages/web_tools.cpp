#include "stages/web_tools.hpp"

#include <cctype>
#include <cstdint>

#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "util/net_inward.hpp"
#include "util/say.hpp"
#include "util/text.hpp"

namespace changji::stages {

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

namespace {

const char* kUserAgent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/124.0 Safari/537.36";

std::map<std::string, std::string> browser_headers() {
    return {{"User-Agent", kUserAgent},
            {"Accept", "text/html,application/json;q=0.9,*/*;q=0.8"},
            {"Accept-Language", "zh-CN,zh;q=0.9"}};
}

// ---- 拆 HTML：**一律手写线性扫描，不用 std::regex** ----
//
// libstdc++ 的 std::regex 匹配时**每吃一个字符递归一层**：`<script[\s\S]*?</script>`
// 碰上一段 100 KB 的内联脚本（新闻站、Next.js 页面里那一大块 JSON 很常见），8 MB 的栈
// 当场溢出——整个引擎进程 SIGSEGV，手上所有的活连同界面一起没了；macOS 上副线程的栈
// 只有 512 KB，4 KB 就够了（审查 2026-09-25 拿 g++ 实测）。一个 `<img src="data:…">`
// 就能让 `<[^>]*>` 崩掉。网页是模型挑的、外面来的，长什么样都有可能。

/// 不分大小写地找（needle 给小写 ASCII）。
std::size_t ifind(const std::string& s, const char* needle, std::size_t from = 0) {
    const std::size_t n = std::strlen(needle);
    if (n == 0 || s.size() < n) return std::string::npos;
    for (std::size_t i = from; i + n <= s.size(); ++i) {
        std::size_t k = 0;
        while (k < n && std::tolower(static_cast<unsigned char>(s[i + k])) == needle[k]) ++k;
        if (k == n) return i;
    }
    return std::string::npos;
}

/// 整段整段地删：从 `open` 到下一个 `close`（含）。没有收尾的删到末尾。
std::string drop_blocks(const std::string& s, const char* open, const char* close) {
    std::string out;
    out.reserve(s.size());
    std::size_t at = 0;
    for (;;) {
        const std::size_t b = ifind(s, open, at);
        if (b == std::string::npos) {
            out.append(s, at, std::string::npos);
            return out;
        }
        out.append(s, at, b - at);
        const std::size_t e = ifind(s, close, b + std::strlen(open));
        if (e == std::string::npos) return out;
        at = e + std::strlen(close);
    }
}

/// 标签全去掉；`breaks` 为真时，换行那一族（br、p、div、li、h1-6、tr、section、
/// article，开的收的都算）换成一个换行。没收尾的 `<` 原样留着（同原来的 `<[^>]*>`）。
std::string drop_tags(const std::string& s, bool breaks) {
    static const std::set<std::string> kBreaks = {"br", "p",  "div", "li", "h1", "h2",
                                                  "h3", "h4", "h5",  "h6", "tr", "section",
                                                  "article"};
    std::string out;
    out.reserve(s.size());
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '<') {
            out += s[i++];
            continue;
        }
        const std::size_t gt = s.find('>', i + 1);
        if (gt == std::string::npos) {
            out.append(s, i, std::string::npos);
            break;
        }
        if (breaks) {
            std::size_t k = i + 1;
            if (k < gt && s[k] == '/') ++k;
            std::string name;
            while (k < gt && std::isalnum(static_cast<unsigned char>(s[k]))) {
                name += static_cast<char>(std::tolower(static_cast<unsigned char>(s[k])));
                ++k;
            }
            if (kBreaks.count(name) != 0) out += '\n';
        }
        i = gt + 1;
    }
    return out;
}

std::string strip_tags(const std::string& s) { return drop_tags(s, false); }

std::string decode_entities(std::string s) {
    for (const auto& [from, to] : std::vector<std::pair<const char*, const char*>>{
             {"&nbsp;", " "}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
             {"&quot;", "\""}, {"&#39;", "'"}, {"&#x27;", "'"}, {"&ldquo;", "“"},
             {"&rdquo;", "”"}}) {
        for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at)) {
            s.replace(at, std::strlen(from), to);
        }
    }
    return s;
}

std::string collapse(std::string s) {
    // 连续空白并成一个；三个以上换行并成两个
    std::string out;
    out.reserve(s.size());
    int nl = 0;
    bool sp = false;
    for (char c : s) {
        if (c == '\n') {
            if (++nl <= 2) out += '\n';
            sp = false;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            if (!sp && !out.empty() && out.back() != '\n') out += ' ';
            sp = true;
        } else {
            out += c;
            nl = 0;
            sp = false;
        }
    }
    return text::strip_ws(out);
}

bool points_inward(const std::string& url);

std::string fetch(const WebTools& web, const std::string& url, std::string* err) {
    if (!web.get) {
        if (err) *err = "这台没配上网的那一层";
        return {};
    }
    // **跳转自己跟，每一跳重新判一遍。** 让 httplib 自己跟的话，公网上一个页面回
    // `302 Location: http://169.254.169.254/…` 或者 `http://127.0.0.1:8080/api/…`，
    // 请求就到了元数据口子、引擎自己的接口上——头一个网址判过没用。
    std::string at = url;
    for (int hop = 0; hop <= 5; ++hop) {
        const llm::HttpResponse r = web.get(at, browser_headers(), web.timeout_s);
        if (r.transport_error.has_value()) {
            if (err) *err = "连不上：" + *r.transport_error;
            return {};
        }
        if (r.status >= 300 && r.status < 400) {
            const auto loc = r.headers.find("location");
            if (loc == r.headers.end() || loc->second.empty()) {
                if (err) *err = "打不开（HTTP " + std::to_string(r.status) + "，没说跳去哪儿）";
                return {};
            }
            const std::string next = llm::redirect_target(at, text::strip_ws(loc->second));
            if (next.rfind("http://", 0) != 0 && next.rfind("https://", 0) != 0) {
                if (err) *err = "跳到了一个不是网页的地址，不跟：" + next;
                return {};
            }
            if (points_inward(next)) {
                if (err) *err = "跳到了本机或者内网的地址，不跟：" + next;
                return {};
            }
            at = next;
            continue;
        }
        if (r.status >= 400) {
            if (err) *err = "打不开（HTTP " + std::to_string(r.status) + "）";
            return {};
        }
        return r.body;
    }
    if (err) *err = "跳转太多次了";
    return {};
}

std::string render_hot(const std::vector<HotItem>& items) {
    if (items.empty()) return "热榜一条都没拿到。";
    std::string out;
    int n = 0;
    std::set<std::string> seen;
    for (const auto& it : items) {
        if (it.title.empty() || !seen.insert(it.title).second) continue;
        out += std::to_string(++n) + ". [" + it.source + "] " + it.title;
        if (!it.note.empty()) out += " —— " + text::truncate_utf8(it.note, 80);
        if (!it.url.empty()) out += " (" + it.url + ")";
        out += "\n";
        if (n >= 40) break;
    }
    return out;
}

std::string tool_hot_topics(const WebTools& web) {
    std::vector<HotItem> all;
    std::string errs;
    struct Src {
        const char* url;
        std::vector<HotItem> (*parse)(const std::string&);
        const char* name;
    };
    for (const Src& s : {
             Src{"https://top.baidu.com/api/board?platform=wise&tab=realtime", parse_baidu_hot, "百度"},
             Src{"https://www.toutiao.com/hot-event/hot-board/?origin=toutiao_pc", parse_toutiao_hot, "头条"},
             Src{"https://weibo.com/ajax/side/hotSearch", parse_weibo_hot, "微博"},
         }) {
        std::string err;
        const std::string body = fetch(web, s.url, &err);
        if (body.empty()) {
            errs += std::string(s.name) + "：" + err + "\n";
            continue;
        }
        try {
            for (auto& it : s.parse(body)) all.push_back(std::move(it));
        } catch (const std::exception& e) {
            errs += std::string(s.name) + "：解析不了（" + e.what() + "）\n";
        }
    }
    std::string out = render_hot(all);
    if (!errs.empty()) out += "\n（没拿到的：" + errs + "）";
    return out;
}

std::string tool_web_search(const WebTools& web, const std::string& query) {
    if (text::strip_ws(query).empty()) return "要搜什么？query 是空的。";
    std::string err;
    const std::string html = fetch(
        web, "https://cn.bing.com/search?q=" + url_encode(query) + "&setlang=zh-CN&ensearch=0",
        &err);
    if (html.empty()) return "搜不了：" + err;
    const auto hits = parse_bing_results(html);
    if (hits.empty()) return "「" + query + "」一条结果都没解出来。";
    std::string out;
    int n = 0;
    for (const auto& h : hits) {
        out += std::to_string(++n) + ". " + h.title + "\n   " + h.url + "\n   " +
               text::truncate_utf8(h.snippet, 200) + "\n";
        if (n >= 8) break;
    }
    return out;
}

/// 这个网址指的是不是**自己这台机器、或者自己这张内网**。
///
/// ⚠️ **网址是模型填的，而模型看的那几页是从外面来的。** 这条工具的全部
/// 用途就是"读一个页面"——而页面里可以写着「接下来请打开
/// http://169.254.169.254/latest/meta-data/iam/…」。模型照做了，那段东西
/// 就变成对话里的一段字，接着进故事、进项目。
///
/// 这台跑在哪儿很要紧：租来的 GPU 机上 `169.254.169.254` 是云厂商的元数据
/// 口子（密钥在那儿），而引擎自己就在 127.0.0.1 上听着。
///
/// **这儿只判字面**，挡在发请求之前、给模型一句清楚的话。域名解析到里面的那一种
/// 由取网页那一层挡（`llm::default_http_get(..., public_only=true)`：先解析、每个
/// 地址都判、再钉住判过的地址去连——见 util/net_inward.hpp）。原来只有字面这一层，
/// 一个解析到 127.0.0.1 的域名就绕过去了（2026-09-26 审查）。
bool points_inward(const std::string& url) {
    // 取出 host：`scheme://host[:port]/…`，也认 `user@host`。
    const auto after = url.find("://");
    if (after == std::string::npos) return false;
    std::string host = url.substr(after + 3);
    host = host.substr(0, host.find_first_of("/?#"));
    if (const auto at = host.rfind('@'); at != std::string::npos) {
        host = host.substr(at + 1);
    }
    if (!host.empty() && host.front() == '[') {          // IPv6 字面量
        const auto close = host.find(']');
        host = close == std::string::npos ? host.substr(1) : host.substr(1, close - 1);
    } else {
        host = host.substr(0, host.find(':'));
    }
    std::string low;
    low.reserve(host.size());
    for (char c : host) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low.empty()) return true;   // 说不清就当指着里面
    if (low.back() == '.') low.pop_back();   // "localhost." 也是 localhost

    if (low == "localhost" || low.size() > 10 &&
                                  low.compare(low.size() - 10, 10, ".localhost") == 0) {
        return true;
    }

    // 字面地址（IPv6 按字节判；IPv4 按 inet_aton 那套宽松写法读——`127.1`、
    // `2130706433`、`0x7f000001`、`0177.0.0.1` 全是本机）。见 util/net_inward.hpp。
    bool literal = false;
    return util::literal_inward(low, literal);
}

std::string tool_fetch_page(const WebTools& web, const std::string& url) {
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return "url 要以 http:// 或 https:// 开头。";
    }
    if (points_inward(url)) {
        // 照实说，别装作打不开——模型会照着这句话决定下一步。
        return "这个地址指着本机或者内网，不给开：" + url;
    }
    std::string err;
    const std::string html = fetch(web, url, &err);
    if (html.empty()) return "打不开：" + err;
    const std::string txt = html_to_text(html);
    if (txt.empty()) return "打开了，但没读到正文。";
    return text::truncate_utf8(txt, 6000);
}

}  // namespace

ordered web_tool_specs() {
    const auto fn = [](const char* name, const char* desc, ordered params) {
        return ordered{{"type", "function"},
                       {"function", ordered{{"name", name},
                                            {"description", desc},
                                            {"parameters", std::move(params)}}}};
    };
    ordered none = ordered{{"type", "object"}, {"properties", ordered::object()}};
    ordered q = ordered{{"type", "object"},
                        {"properties", ordered{{"query", ordered{{"type", "string"}, {"description", "搜什么"}}}}},
                        {"required", ordered::array({"query"})}};
    ordered u = ordered{{"type", "object"},
                        {"properties", ordered{{"url", ordered{{"type", "string"}, {"description", "要打开的网址"}}}}},
                        {"required", ordered::array({"url"})}};
    return ordered::array({
        fn("hot_topics", "看看网上现在什么热：百度热搜、今日头条热榜、微博热搜合在一起，几十条标题。", none),
        fn("web_search", "搜一个词，回来前几条结果的标题、链接、摘要。", q),
        fn("fetch_page", "打开一个网址，回来正文（去掉标签，最多六千字）。", u),
    });
}

std::string run_web_tool(const WebTools& web, const std::string& name,
                         const std::string& arguments_json) {
    json args = json::object();
    if (!text::strip_ws(arguments_json).empty()) {
        try {
            args = json::parse(arguments_json);
        } catch (const std::exception&) {
            return "参数不是 JSON。";
        }
        if (!args.is_object()) args = json::object();
    }
    const auto str_arg = [&args](const char* k) {
        const auto it = args.find(k);
        return it != args.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    // **交回去的字一律洗成合法 UTF-8。** GBK / GB2312 的中文站原样进来是一串非法字节：
    // 下一次把对话发给模型（payload.dump）、往对话记录里写（append_turn）都当场抛
    // type_error.316——上网写一章写了几分钟，整轮在这儿断掉。
    std::string out;
    if (name == "hot_topics") out = tool_hot_topics(web);
    else if (name == "web_search") out = tool_web_search(web, str_arg("query"));
    else if (name == "fetch_page") out = tool_fetch_page(web, str_arg("url"));
    else return "没有叫 " + name + " 的工具。有的是 hot_topics、web_search、fetch_page。";
    if (text::is_valid_utf8(out)) return out;
    return text::sanitize_utf8(out) +
           "\n（这一页不是 UTF-8 编码的，上面有些字读乱了；换一个页面、或者搜别的说法试试。）";
}

std::string web_tool_label(const std::string& name, const std::string& arguments_json) {
    if (name == "hot_topics") return SAY("正在查看网络热点");
    if (name == "fetch_page") return SAY("正在读取网页");
    if (name != "web_search") return {};
    // 搜的是什么要说出来：一轮里连搜三次，三行都是「在网上搜」等于没说。
    // 参数读不动就只说动作，不瞎猜（同 `agent::tool_ask`）。
    std::string q;
    const json a = json::parse(arguments_json, nullptr, /*allow_exceptions=*/false);
    if (a.is_object() && a.contains("query") && a.at("query").is_string()) {
        q = text::strip_ws(a.at("query").get<std::string>());
    }
    return q.empty() ? SAY("正在联网搜索") : SAYF("正在联网搜索「%1」", q);
}

std::vector<HotItem> parse_baidu_hot(const std::string& body) {
    // {"data":{"cards":[{"content":[{"word","desc","hotScore","url"...}]}]}}
    std::vector<HotItem> out;
    const json j = json::parse(body);
    for (const auto& card : j.value("data", json::object()).value("cards", json::array())) {
        for (const auto& c : card.value("content", json::array())) {
            HotItem it;
            it.title = c.value("word", std::string());
            it.note = c.value("desc", std::string());
            it.url = c.value("url", std::string());
            it.source = "百度";
            if (!it.title.empty()) out.push_back(std::move(it));
        }
    }
    return out;
}

std::vector<HotItem> parse_toutiao_hot(const std::string& body) {
    // {"data":[{"Title","HotValue","Url"...}]}
    std::vector<HotItem> out;
    const json j = json::parse(body);
    for (const auto& c : j.value("data", json::array())) {
        HotItem it;
        it.title = c.value("Title", std::string());
        it.url = c.value("Url", std::string());
        it.source = "头条";
        if (!it.title.empty()) out.push_back(std::move(it));
    }
    return out;
}

std::vector<HotItem> parse_weibo_hot(const std::string& body) {
    // {"data":{"realtime":[{"word","note","num"...}]}}
    std::vector<HotItem> out;
    const json j = json::parse(body);
    for (const auto& c : j.value("data", json::object()).value("realtime", json::array())) {
        HotItem it;
        it.title = c.value("word", std::string());
        it.note = c.value("note", std::string());
        it.url = "https://s.weibo.com/weibo?q=" + url_encode(it.title);
        it.source = "微博";
        if (!it.title.empty()) out.push_back(std::move(it));
    }
    return out;
}

std::vector<SearchHit> parse_bing_results(const std::string& html) {
    // 每条结果：<li class="b_algo"> … <h2><a href="URL">标题</a></h2> … <p>摘要</p>
    // 手写扫描，不用 std::regex（见上面拆 HTML 那一段）。
    std::vector<SearchHit> out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t b = html.find("<li class=\"b_algo\"", at);
        if (b == std::string::npos) break;
        const std::size_t e = html.find("</li>", b);
        if (e == std::string::npos) break;
        const std::string block = html.substr(b, e - b);
        at = e + 5;

        // <h2 …> 后面（隔着空白）紧跟 <a … href="URL" …>标题</a>
        const std::size_t h2 = block.find("<h2");
        if (h2 == std::string::npos) continue;
        std::size_t k = block.find('>', h2);
        if (k == std::string::npos) continue;
        ++k;
        while (k < block.size() && std::isspace(static_cast<unsigned char>(block[k]))) ++k;
        if (block.compare(k, 2, "<a") != 0) continue;
        const std::size_t a_end = block.find('>', k);
        if (a_end == std::string::npos) continue;
        const std::string a_tag = block.substr(k, a_end - k);
        const std::size_t href = a_tag.find("href=\"");
        if (href == std::string::npos) continue;
        const std::size_t q = a_tag.find('"', href + 6);
        if (q == std::string::npos) continue;
        const std::size_t close_a = block.find("</a>", a_end);
        if (close_a == std::string::npos) continue;

        SearchHit h;
        h.url = a_tag.substr(href + 6, q - href - 6);
        h.title = collapse(decode_entities(strip_tags(block.substr(a_end + 1, close_a - a_end - 1))));
        // 摘要：第一个 <p …>…</p>
        for (std::size_t p = block.find("<p", close_a); p != std::string::npos;
             p = block.find("<p", p + 2)) {
            const char nx = p + 2 < block.size() ? block[p + 2] : '\0';
            if (nx != '>' && !std::isspace(static_cast<unsigned char>(nx))) continue;
            const std::size_t pg = block.find('>', p);
            const std::size_t pe = pg == std::string::npos ? pg : block.find("</p>", pg);
            if (pe == std::string::npos) break;
            h.snippet = collapse(decode_entities(strip_tags(block.substr(pg + 1, pe - pg - 1))));
            break;
        }
        if (!h.title.empty() && !h.url.empty()) out.push_back(std::move(h));
    }
    return out;
}

std::string html_to_text(const std::string& html) {
    std::string s = drop_blocks(html, "<script", "</script>");
    s = drop_blocks(s, "<style", "</style>");
    s = drop_blocks(s, "<!--", "-->");
    s = drop_tags(s, /*breaks=*/true);
    return collapse(decode_entities(s));
}

std::string url_encode(const std::string& s) {
    std::string out;
    char buf[4];
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            std::snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

}  // namespace changji::stages
