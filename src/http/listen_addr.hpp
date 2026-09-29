#pragma once

// 这台引擎在哪个口上听、别的机器从哪个地址连得进来（2026-09-28，桌面端改成对外监听那一轮）。

#include <string>
#include <vector>

namespace changji::http {

/// 这个端口能不能拿来在 `host` 上听：本机回环上没人在听，而且 `host:port` 绑得上。
///
/// ⚠️ **两样都要问。** macOS 上 `0.0.0.0:P` 和别人的 `127.0.0.1:P` 能同时绑上（Crow 设了
/// SO_REUSEADDR），而连 `127.0.0.1:P` 的会落到**更具体的那一个**——桌面端连的正是回环，于是
/// 它说的每一句话都进了另一台引擎，往别人的片子里写东西。所以先连一下回环，有人接就算占着。
bool port_usable(const std::string& host, int port);

/// 这是不是一个局域网地址：10/8、172.16/12、192.168/16。
///
/// **只认这三段**：代理软件的虚拟网卡（198.18/15）、169.254（没拿到地址时系统自己给的）、
/// 公网地址都不是"同一个网段上的人照着填就连得上"的那个。
bool is_lan_ipv4(unsigned char a, unsigned char b);

/// 这台机器在局域网上的地址（IPv4），最多三个。家里、办公室常见的 192.168 排前面，然后 10，
/// 172.16 那一段排最后（虚拟机、WSL、Docker 的虚拟网卡多半在那儿）。问不出来回空。
std::vector<std::string> lan_ipv4s();

/// 服务起来之后在终端上打的那一块：在浏览器里打开哪几个地址。对外监听时每一条都带着口令，
/// 再单独说一遍口令是什么、在哪儿改；只听本机时说不用口令、怎么让别的电脑也打得开。
///
/// **打在标准输出上，不走日志**（2026-09-28，用户：「新用户第一次打开应该在终端里直接显示
/// token，不然 webapp 怎么访问？」）。原来是一条 `[WARNING ]` 日志，夹在带时间戳的请求日志
/// 中间、两秒就被刷上去，地址还写成 `http://<这台的地址>:8080/?token=…` 要人自己换；只听本机
/// 时一句「打开什么」都没有。地址要是**点得开的**。
std::string ready_banner(const std::string& bind_host, int port, const std::string& token,
                         const std::vector<std::string>& lan_ips);

}  // namespace changji::http
