#pragma once

// 场记云：这台引擎登没登场记账号、拿什么钥匙去用云上的大模型和出片。
//
// 方案在 docs/账号与云服务方案.md。这一层**只有纯逻辑**（凭据文件、PKCE、授权地址、回调地址
// 认不认、虚的那一家 / 那一台）；发请求的那一半在 http/cloud_api.cpp，请求函数是注入的，
// 所以这儿进得了单元测试。
//
// **账号在云上，片子在本机**：登录只是多开一扇门。没登录（桌面端点了跳过、网页用本机口令进）
// 一个功能都不少，只是没有「场记云」那一家大模型、那一台机器。
//
// 凭据是**设备钥匙**（`cjk_…`，一台设备一把、长期有效、账号页能吊销），不是一小时过期的票：
// 引擎里大模型的密钥、机器表的口令本来就是长期的一串，设备钥匙直接当它们用，大模型客户端、
// 派活那一层一行不改。存在 `<配置目录>/cloud.json`（0600）。

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace changji::cloud {

struct Account {
    std::string id;
    std::string name;
    std::string avatar;
    std::string provider;  ///< wechat / google
    std::string status;    ///< active / pending / blocked
    std::string role;      ///< admin / member
};

/// 登着的那一份。`url` 是哪一朵云（`[cloud].url` 那时候的值）——换了地址就不认了，免得把
/// 这一家的钥匙发到另一家去（同「大模型密钥只给存它的那一家」）。
struct Creds {
    std::string url;
    std::string device_key;
    std::string device_id;
    Account account;
    std::int64_t bound_at = 0;  ///< 毫秒
};

nlohmann::json to_json(const Account& a);
Account account_from_json(const nlohmann::json& j);

// ---- 凭据文件 ----

std::filesystem::path creds_path();
std::optional<Creds> load_creds();
/// 0600、整份换。写不下抛 std::runtime_error。
void save_creds(const Creds& c);
void clear_creds();

// ---- 地址 ----

/// 去掉尾巴上的 `/`，前后空白。
std::string bare_url(const std::string& url);
/// 云上那一家大模型的地址：`<云>/v1`。
std::string llm_base(const std::string& cloud_url);
/// 云上那一台机器的地址：`<云>/render`（长得就是一台工作节点，见 infer/worker_server.hpp）。
std::string render_url(const std::string& cloud_url);

/// **按地址取钥匙**的那一处问这个（config::read_api_key_for）：地址是登着的那朵云的 `/v1`
/// 才给设备钥匙，别的一律没有。
std::optional<std::string> key_for(const std::string& base_url);
/// 这个地址是不是登着的那朵云的出片节点。机器表写盘时跳过它：它是虚的，退出登录就该没了，
/// 写进 config.toml 的话就留下半截配置（一台带着作废钥匙、永远 401 的机器）。
bool is_cloud_node(const std::string& url);

/// 登着、而且登的就是 `cloud_url` 这一朵：机器表里多出来的那一台。
struct VirtualNode {
    std::string url;
    std::string token;
};
std::optional<VirtualNode> virtual_node(const std::string& cloud_url);

// ---- 登录那一趟 ----

/// 随机串（base64url，`bytes` 个随机字节）。PKCE 的校验码、state 都用它。
std::string random_token(std::size_t bytes = 32);
/// PKCE S256：base64url(sha256(verifier))，不带 `=`（RFC 7636）。
std::string pkce_challenge(const std::string& verifier);

/// 回调落在哪个主机上场记云才认：回环（127.0.0.1 / localhost / [::1]）或者局域网
/// （10/8、172.16/12、192.168/16）。公网域名不认——那种地址上的授权码能被别的网站截走。
/// `host` 可以带端口。
bool redirect_host_ok(const std::string& host);

enum class Purpose {
    bind,   ///< 把这台引擎登到场记账号上（第一次登录即绑主人）
    enter,  ///< 没进门的浏览器用场记账号进这台引擎（账号得是主人）
};

struct Pending {
    std::string verifier;
    std::string redirect_uri;
    std::string provider;
    std::string cloud_url;
    Purpose purpose = Purpose::bind;
    std::int64_t at = 0;  ///< 毫秒
};

/// 发出去还没回来的那几趟。**一次性、10 分钟**：回调带回来的 state 必须是这儿发出去、还没
/// 用过的（防 CSRF：别人拿一个他自己的授权码骗你的引擎绑到他的账号上）。
class PendingLogins {
public:
    std::string issue(Pending p, std::int64_t now_ms);
    std::optional<Pending> take(const std::string& state, std::int64_t now_ms);

private:
    std::mutex mu_;
    std::unordered_map<std::string, Pending> map_;
};
PendingLogins& pending();

/// `GET <云>/oauth/authorize?…`。`provider` 空就不带（那边默认摆微信）。
std::string authorize_url(const std::string& cloud_url, const std::string& redirect_uri,
                          const std::string& state, const std::string& challenge,
                          const std::string& device_name, const std::string& platform,
                          const std::string& provider);

/// `POST <云>/oauth/token` 回来的那一份 → 凭据。缺钥匙回空。
std::optional<Creds> creds_from_token_reply(const nlohmann::json& j, const std::string& cloud_url,
                                            std::int64_t now_ms);

/// 这台设备在账号页「我的设备」上叫什么：机器名，取不到就「场记」。
std::string device_name();
/// windows / macos / linux
std::string platform_name();

std::int64_t now_ms();

}  // namespace changji::cloud
