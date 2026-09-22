#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""租卡的界面：AutoDL 和 PPIO 两个平台，建、开、关、释放四个按钮。

**为什么要这个。** 跑一部片要的卡是一阵一阵的——出首帧、出片那几个小时要卡，
其余时间机器开着就是在烧钱。控制台一天点十几次，手一抖就忘了关。这个界面把
四个动作摆出来，SSH 那行点详情就进剪贴板，下一步直接喂 install_workers.sh
（见 setup_gpu_box.md）。

**为什么是两家。** AutoDL 便宜、镜像现成；PPIO 有**抢占式**（spot），同一块卡
常常只要按量的一半，批量出片这种断了能重跑的活正合适。两家都在手边才挑得动。

**为什么用 Qt 而不是 tkinter。** 2026-09-18 试过 tkinter，在这台机器上跑不动：
系统自带的 python3 配的是 Tk 8.5.9（2010 年的版本），在 macOS 27 上**每次事件
循环唤醒漏 160 KB**，CPU 和内存跟唤醒频率严格成正比（每 80ms 唤醒一次 →
18.8% CPU、2 MB/s；每 1000ms → 1.5%），窗口一个字都画不出来。一个光摆着
tk.Text 的空窗口就烧 46% CPU，而同一个窗口里 Label / Listbox / Treeview /
Canvas / Entry 都是 0%。那不是布局写错了，是那套 Tk 本身坏的。

跑法：

    python3 changji/cpp/tools/autodl_gui.py

PySide6 装在一个独立 venv 里（~/.local/share/changji/venv，约 350 MB），
**不用记这条路径**——随便哪个 python3 起它，发现自己没有 PySide6 就会自己
换到那个 venv 再跑一遍。venv 不在的话会把该敲的两条命令印出来。

两家的钥匙分开存在 ~/.config/changji/cloud.json（0600），也认环境变量
AUTODL_TOKEN / PPIO_API_KEY：

  · AutoDL：www.autodl.com → 控制台 → 账号 → 设置 → 开发者Token。
            用 Pro API 要求账号已实名，没实名第一次调用就会被顶回来。
  · PPIO：  ppio.com/settings#key-management → API 密钥。

接口出处：
  https://www.autodl.com/docs/instance_pro_api/
  https://ppio.com/docs/gpus/reference-start
"""

import base64
import calendar
import concurrent.futures
import hashlib
import hmac
import json
import os
import re
import socket
import stat
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

VENV_PY = os.path.expanduser("~/.local/share/changji/venv/bin/python3")

try:
    from PySide6 import QtCore, QtGui, QtWidgets
except ImportError:
    # 用错 python 起的，别让人自己去猜是哪个——认得出来就自己换过去再跑一遍。
    # 防死循环只能靠这个环境变量，**不能比路径**：venv 里的 python3 是个软链，
    # realpath 出来跟 Homebrew 那个是同一个文件，比 realpath 会判成「已经在
    # venv 里了」，于是这一步整个不生效。
    if os.path.exists(VENV_PY) and not os.environ.get("CHANGJI_QT_REEXEC"):
        os.environ["CHANGJI_QT_REEXEC"] = "1"
        os.execv(VENV_PY, [VENV_PY, os.path.abspath(__file__)] + sys.argv[1:])
    sys.stderr.write(
        "这个界面要 PySide6，当前这个 python 没有。装一次就好：\n\n"
        "    python3 -m venv ~/.local/share/changji/venv\n"
        "    ~/.local/share/changji/venv/bin/pip install PySide6-Essentials\n\n"
        "装完再跑这个脚本，它会自己找过去。\n")
    raise SystemExit(1)


# 打包时 CI 会把这一行的 dev 换成真版本号（见 .github/workflows/release.yml
# 的「工具包」那一格）。源码里跑就是 dev。**别改这行的形状**，CI 认的是它。
VERSION = "dev"

CONFIG_PATH = os.path.expanduser("~/.config/changji/cloud.json")
LEGACY_CONFIG = os.path.expanduser("~/.config/changji/autodl.json")

# 关机到什么程度算「关好了」。各家的状态名都不一样，文档又都只举了 running
# 一个例子，所以这里认一组而不是认一个词——多认几个总比把「exited」当成没关完、
# 干等到超时强。**大小写也得无关**：阿里云回的是 `Stopped`、`Running`，
# 拿它去比小写的 "stopped" 永远不相等，于是「等停稳」会一直等到超时。
OFF_STATES = ("shutdown", "stopped", "stop", "exited", "halted", "off")


def is_off(status):
    return (status or "").strip().lower() in OFF_STATES


def human_bytes(n):
    """字节数说成人话。GB 用十进制（跟云厂商账单一个口径）。"""
    n = float(n or 0)
    for unit, step in (("TB", 1e12), ("GB", 1e9), ("MB", 1e6), ("KB", 1e3)):
        if n >= step:
            return "%.1f %s" % (n / step, unit)
    return "%d B" % n


def check_password(pw, kinds_needed):
    """本地先判一遍实例登录密码。

    两家的接口都只回一句「格式不对」（阿里云 InvalidPassword.Malformed），
    照着那句话猜要猜很久。要求的类数不一样：阿里云要四类占三类，腾讯云两类。
    """
    if not (8 <= len(pw) <= 30):
        raise CloudError("登录密码要 8 到 30 位，现在是 %d 位" % len(pw))
    kinds = sum([any(c.islower() for c in pw), any(c.isupper() for c in pw),
                 any(c.isdigit() for c in pw), any(not c.isalnum() for c in pw)])
    if kinds < kinds_needed:
        raise CloudError("登录密码要在「小写/大写/数字/符号」四类里占满 %d 类，"
                         "现在只占了 %d 类" % (kinds_needed, kinds))


class CloudError(Exception):
    """接口那头说的不行。msg 是接口自己的原话，别在外面重新编一句。"""


# ---- HTTP：两家共用这一层 ----------------------------------------------

def _error_text(raw):
    """错误回包里那句人话在哪一栏，各家不一样，挨个试。"""
    try:
        d = json.loads(raw)
    except ValueError:
        return raw[:300]
    if not isinstance(d, dict):
        return raw[:300]
    # 阿里云回的是 {"Code": "...", "Message": "...", "Recommend": "..."}，
    # 大写开头，所以这张表两种写法都要有。Code 往往比 Message 更能指出毛病
    # （比如 InvalidAccessKeyId.NotFound），所以两个都带上。
    msg = ""
    for k in ("msg", "message", "Message", "reason", "detail"):
        v = d.get(k)
        if isinstance(v, str) and v:
            msg = v
            break
    code = d.get("Code")
    if msg:
        return ("%s（%s）" % (msg, code)) if isinstance(code, str) and code else msg
    if isinstance(code, str) and code:
        return code
    err = d.get("error")
    if isinstance(err, str) and err:
        return err
    if isinstance(err, dict):
        for k in ("message", "msg", "reason", "code"):
            if err.get(k):
                return str(err[k])
    return raw[:300]


def request_json(method, url, headers, body=None, timeout=30, form=None, raw=None):
    if raw is not None:
        # 腾讯云签名签的是**这串字节本身**，重新序列化一次（哪怕只差一个空格）
        # 签名就对不上，而接口只会回一句 AuthFailure。
        data = raw.encode("utf-8") if isinstance(raw, str) else raw
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        return _send(req, url, timeout)
    if form is not None:
        # 阿里云那套 RPC 接口收的是 form-urlencoded，不是 JSON
        data = urllib.parse.urlencode(form).encode("utf-8")
        headers = dict(headers)
        headers["Content-Type"] = "application/x-www-form-urlencoded"
    else:
        data = None if body is None else json.dumps(body).encode("utf-8")
    return _send(urllib.request.Request(url, data=data, method=method,
                                        headers=headers), url, timeout)


def _send(req, url, timeout):
    try:
        resp = urllib.request.urlopen(req, timeout=timeout)
        raw = resp.read().decode("utf-8", "replace")
        resp.close()
    except urllib.error.HTTPError as e:
        raise CloudError("HTTP %d：%s"
                         % (e.code, _error_text(e.read().decode("utf-8", "replace"))))
    except urllib.error.URLError as e:
        host = urllib.parse.urlsplit(url).netloc
        raise CloudError("连不上 %s：%s" % (host, e.reason))
    if not raw.strip():
        return {}          # 删除、启停这类接口回 200 空body
    try:
        return json.loads(raw)
    except ValueError:
        raise CloudError("回包不是 JSON：%s" % raw[:200])


# ---- 表单：两家的建机表单字段不同，但都是「一行一个控件」---------------

class Field(object):
    def __init__(self, key, label, kind, choices=None, default="",
                 lo=0, hi=0, step=1, hint=""):
        self.key = key
        self.label = label
        self.kind = kind          # combo / pick / spin / entry / checks
        self.choices = choices or []   # [(显示, 值)]
        self.default = default
        self.lo, self.hi, self.step = lo, hi, step
        # 输入框里那句灰字。密码的格式要求各家不同（阿里云四类占三类、
        # 腾讯云两类），写死在控件里就会有一家是错的。
        self.hint = hint


# ---- 平台：两家各实现一份，界面只认这组方法 -----------------------------

class Platform(object):
    NAME = ""
    CONFIG_KEY = ""
    TOKEN_HINT = ""
    BILLING_HINT = ""
    DYNAMIC = ()          # 要现拉才有内容的字段
    REFRESH_ON = ()       # 改了这些字段要把 DYNAMIC 那几栏重拉（阿里云的地域）

    # 钥匙有几段、各自叫什么、认哪个环境变量。AutoDL / PPIO 一段就够，
    # 阿里云要 AccessKey ID + Secret 两段。
    CREDS = (("token", "Token", ()),)

    def __init__(self, creds=None):
        # 传字典（新格式）和传一段字符串（老配置、单段钥匙的家）都认，
        # 归一在 as_creds 这一处。
        creds = as_creds(creds)
        self.creds = {}
        for key, _label, envs in self.CREDS:
            v = ""
            for name in envs:                # 环境变量优先，临时换账号不用动文件
                v = os.environ.get(name, "").strip()
                if v:
                    break
            if not v:
                v = (creds.get(key) or "").strip()
            self.creds[key] = v

    @property
    def token(self):
        return self.creds.get("token", "")

    def _need_token(self):
        missing = [label for key, label, _ in self.CREDS if not self.creds.get(key)]
        if missing:
            raise CloudError("%s 的%s还没填" % (self.NAME, "、".join(missing)))

    PRICES = False        # 这家有没有「列出可用规格和价格」的接口
    TRAFFIC = False       # 这家报不报「已用流量」
    # ⚠️ 别叫 IMAGES——AutoDL 那个类里 IMAGES 已经是**公共镜像表**了。
    # 撞名之后 setEnabled(一个列表) 当场炸，而「非空列表是真值」让用例
    # 一路绿着混过去（用例里比真值不比 is True，就会漏这种）。
    METRICS = False       # 能不能报实时的 CPU / 内存 / 网络

    # 返回 {"rows": [(名称, 值, 百分比或 None)], "notes": [说明]}。
    # **各家能给的差很远**，所以不硬凑成同一组字段：给得出什么报什么，给不出
    # 的在 notes 里说清为什么——留一栏空着会让人以为是坏了。
    def metrics(self, iid):
        raise CloudError("%s 的接口不报实时数据" % self.NAME)

    # 列表里那一行的「地区」对这家来说是什么，能不能倒推出地域。
    # **看一台机器的实时数据要用它自己所在的地域**，不是建机表单里选的那个
    # ——两者常常不是一个（表单停在乌兰察布、机器在上海），查错地域的表现是
    # 「一条数据都没有」，而那跟「刚开机还没出点」长得一模一样。
    @staticmethod
    def region_of(row):
        return ""

    CAN_IMAGE = False        # 能不能把跑着的机器存成自定义镜像
    CAN_COPY_IMAGE = False   # 存好的镜像能不能复制到别的地域

    # 模型权重四五十 GB，公网 100 Mbps 下要一个多小时——而抢占式随时会被
    # 回收。所以正路是**一次性把模型烤进自定义镜像**，之后每台新机从镜像
    # 开机，零下载。下面三个方法就是为这条路开的。
    #
    # 另一条是开机从对象存储走内网拉（boot_script），适合常换的那几个模型。

    USERDATA_KB = 0       # UserData 上限（编码前），0 表示这家不支持

    def boot_script(self, v):
        """开机脚本。返回空串表示这家 / 这次不装这一套。"""
        return ""

    def encode_userdata(self, script):
        """脚本 → base64。**顺手判尺寸**：两家上限不一样（阿里云 32 KB、
        腾讯云 16 KB），超了接口只回一句参数非法，不说是哪个参数。"""
        raw = script.encode("utf-8")
        limit = self.USERDATA_KB * 1024
        if limit and len(raw) > limit:
            raise CloudError("开机脚本 %d 字节，超过 %s 的 %d KB 上限了"
                             % (len(raw), self.NAME, self.USERDATA_KB))
        return base64.b64encode(raw).decode("ascii")
    def list_images(self):
        """自己的自定义镜像。"""
        raise CloudError("%s 不支持自定义镜像" % self.NAME)

    # 镜像多大（拉下拉的时候顺手记下来）。烤了模型的镜像两三百 GB，而系统盘
    # 默认才 100 GB——**系统盘小于镜像，建机必失败**，而云厂商回的那句
    # （InvalidSystemDiskSize 之类）根本不说该填多少。本地先拦，把数说出来。
    # 可用区必须属于选中的地域。对不上的后果不只是建机失败——开机脚本里的
    # 内网 endpoint 是按「地域」算的，地域错了就会去另一个地域的 OSS 拿东西，
    # 那既走公网又多半拿不到。截图时就撞见过一次（规格在乌兰察布、地域还停在
    # 杭州）。
    def check_zone(self, zone, region):
        if zone and region and not str(zone).startswith(str(region)):
            raise CloudError("可用区「%s」不在地域「%s」里——地域和规格对不上了。"
                             "重新点一下「拉规格 / 镜像」再选规格" % (zone, region))

    def check_disk(self, image_id, disk_gb):
        size = (getattr(self, "_image_sizes", None) or {}).get(image_id)
        if size and disk_gb < size:
            raise CloudError("系统盘 %d GB 装不下这个镜像（镜像本身就 %s GB）——"
                             "把系统盘调到 %d GB 以上" % (disk_gb, size, int(size)))

    def save_image(self, iid, name):
        """把这台机器存成自定义镜像，返回镜像 ID。**异步**，做完要等。"""
        raise CloudError("%s 不支持把实例存成镜像" % self.NAME)

    def copy_image(self, image_id, regions, log):
        """把镜像复制到别的地域。抢占式最便宜的地域会漂，镜像得跟着过去。"""
        raise CloudError("%s 不支持跨地域复制镜像" % self.NAME)

    # ---- 延迟 ----------------------------------------------------------
    #
    # 量的是**到该地域 OSS 接入点的 TCP 握手时间**。为什么用 OSS 不用 ECS 的
    # API 接入点：ECS 那个是就近接入的，实测九个地域量出来 2~12 ms 全挤在一起，
    # 区分不了；OSS 的地域接入点落在那个机房。
    #
    # ⚠️ **本机挂着代理/TUN 时这个数没有意义**：所有域名会被解析成
    # 198.18.x.x 这类假 IP，TCP 握手停在本地虚拟网卡上，量出来美西和杭州都是
    # 个位数毫秒。这种情况直接报「量不了」，**不填一个看着像真的数**——
    # 一栏假延迟比没有这一栏更坏。

    FAKE_IP_PREFIXES = ("198.18.", "198.19.", "240.", "10.24.")
    # 各家自己的「那个机房里的接入点」。**不能共用**：拿阿里云的 oss- 域名去
    # 探腾讯云的 ap-beijing，探的是阿里云北京机房，跟你要租的那台没关系。
    LATENCY_HOST = ""

    @classmethod
    def _probe_latency(cls, region, tries=2, timeout=3.0):
        """返回 (毫秒, 量不了的原因)。量到了原因是 None。"""
        if not cls.LATENCY_HOST:
            return None, "这家没有可用来探延迟的地域接入点"
        host = cls.LATENCY_HOST % region
        try:
            ip = socket.getaddrinfo(host, 443, socket.AF_INET)[0][4][0]
        except OSError:
            return None, "解析不了 %s" % host
        if ip.startswith(cls.FAKE_IP_PREFIXES):
            return None, ("本机走着代理/TUN（%s 解析成假 IP %s），"
                          "量到的是代理不是机房" % (host, ip))
        best = None
        for _ in range(tries):
            t0 = time.time()
            try:
                s = socket.create_connection((ip, 443), timeout=timeout)
                s.close()
            except OSError as e:
                return None, "连不上 %s：%s" % (ip, e)
            ms = (time.time() - t0) * 1000
            best = ms if best is None else min(best, ms)
        return max(1, int(round(best))), None

    def _fill_latency(self, rows, log):
        regions = sorted({r["region"] for r in rows})
        lat, why = {}, None
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            for region, (ms, reason) in zip(
                    regions, pool.map(self._probe_latency, regions)):
                lat[region] = ms
                if ms is None and why is None:
                    why = reason
        got = [v for v in lat.values() if v is not None]
        # 假 IP 那条判据挡不住所有代理（有的不用 198.18 段），再加一条物理判据：
        # 这些地域跨着大洲，**不可能全在 3 毫秒以内**。全都这么快就是被截了，
        # 与其报一列看着像真的数，不如说量不了。
        if got and len(lat) >= 3 and max(got) <= 3:
            why = ("量出来 %d 个地域全在 %d ms 以内——跨洲不可能这么快，"
                   "本机多半有代理/TUN 在中间截，这个数不作数" % (len(got), max(got)))
            lat = {k: None for k in lat}
            got = []
        if got:
            log("延迟：%d 个地域量到了，%d~%d ms" % (len(got), min(got), max(got)))
        else:
            log("延迟量不了：%s" % why)
        for r in rows:
            r["lat"] = lat.get(r["region"])
            r["lat_why"] = why


    def price_table(self, log, regions=None):
        raise CloudError("%s 没有公开的价目接口，价格只能去控制台看" % self.NAME)

    def has_creds(self):
        return all(self.creds.get(k) for k, _, _ in self.CREDS)

    # 界面每次要干活之前都把表单当前值推进来。阿里云的一切都按地域，
    # 而地域是表单上选的——不推进来它只能问默认那个地域，答非所问。
    def set_context(self, values):
        pass

    # 下面这些由子类实现
    def fields(self):                      raise NotImplementedError
    def load_choices(self):                return {}
    def instances(self):                   raise NotImplementedError
    def create(self, v):                   raise NotImplementedError
    def power_on(self, iid, command=""):   raise NotImplementedError
    def power_off(self, iid):              raise NotImplementedError
    def status(self, iid):                 raise NotImplementedError
    def detail(self, iid):                 raise NotImplementedError   # -> (行列表, ssh)
    def release(self, iid):                raise NotImplementedError

    # 建机之前要拦的东西写在这儿一份就够。**确认框弹出来之前也走这一份**——
    # 分两处写的话，规格没填也照样弹「确定要创建吗」，点了确定才报错。
    def validate(self, v):
        pass

    def confirm_release_text(self, iid, name):
        return ("要释放 %s（%s）吗？\n\n"
                "释放 = 删除，机器和盘上的数据一起没，这一步不可逆。"
                % (iid, name or "无备注名"))

    # 关机 → 等停稳 → 释放。谁调用都能复用。
    def _wait_off(self, iid, log, wait_s):
        deadline = time.time() + wait_s
        st = ""
        while time.time() < deadline:
            time.sleep(3)
            st = self.status(iid)
            log("  …… %s" % st)
            if is_off(st):
                return st
        return st

    def release_safely(self, iid, log, wait_s=180):
        raise NotImplementedError


class AutoDLPlatform(Platform):
    """AutoDL 容器实例 Pro API。

    形状上跟别家都不一样：一律 POST（连查询也是），Authorization 直接放裸
    token 不带 Bearer，而 snapshot / status 两个接口是 **GET 带 body**。
    """

    NAME = "AutoDL"
    CONFIG_KEY = "autodl"
    CREDS = (("token", "Token", ("AUTODL_TOKEN",)),)
    HOST = "https://api.autodl.com"
    TOKEN_HINT = "控制台 → 账号 → 设置 → 开发者Token（要先实名认证）"
    BILLING_HINT = ("只能按量计费建机，开机那刻开始算钱，关机只留存储费。\n"
                    "AutoDL 的规则是连续关机 15 天自动释放。\n"
                    "API 只能有卡开机，无卡模式得去控制台点。")
    DYNAMIC = ("image",)
    CAN_IMAGE = True

    # 算力规格 ID 抄自文档附录。前台网页上显示的是「4090-48G 通用型」这种名字，
    # API 要的是右边那个 ID，两边对不上是这套接口最容易卡住的一步。
    SPECS = [
        ("PRO6000-96G 性能型", "pro6000-p"),
        ("H800-80G 通用型", "h800"),
        ("5090-32G 性能型", "5090-p"),
        ("4090-48G 通用型", "v-48g"),
        ("4090D 通用型", "4090D"),
        ("4080(S)-32G 性能型", "v-32g-p"),
        ("3090-48G 通用型", "v-48g-350w"),
    ]
    IMAGES = [
        ("PyTorch 2.0.0 / cuda11.8 / py38", "base-image-l2t43iu6uk"),
        ("PyTorch 1.11.0 / cuda11.3 / py38", "base-image-l374uiucui"),
        ("PyTorch 1.10.0 / cuda11.3 / py38", "base-image-u9r24vthlk"),
        ("PyTorch 1.9.0 / cuda11.1 / py38", "base-image-12be412037"),
        ("Miniconda / cuda11.6 / py38", "base-image-mbr2n4urrc"),
        ("Miniconda / cuda11.1 / py38", "base-image-h041hn36yt"),
        ("Miniconda / cudagl11.3 / py38", "base-image-7bn8iqhkb5"),
        ("TensorFlow 2.9.0 / cuda11.2 / py38", "base-image-uxeklgirir"),
        ("TensorRT 8.5.1 / cuda11.8 / py38", "base-image-l2843iu23k"),
    ]
    CUDA = [("cuda ≥ 12.4", 124), ("cuda ≥ 12.1", 121), ("cuda ≥ 12.0", 120),
            ("cuda ≥ 11.8", 118), ("cuda ≥ 11.3", 113)]
    REGIONS = [("西北区", "westDC3"), ("北京区", "beijingDC2")]

    def _call(self, method, path, body=None):
        self._need_token()
        url = self.HOST + path
        data = body or {}
        if method == "GET":
            # ⚠️ **文档写的是「GET + 请求 Body」，实际不是。** 2026-09-20 拿真
            # 账号试出来的：GET 带 body 回 `RequestParameterIsWrong`，参数改成
            # query string 才通；同样两个接口换成 POST 是 404。
            # 这一处错了的后果不止「详情」按钮不出东西——`release_safely` 第一
            # 步就是查状态，所以**释放整条也是坏的**，而它报的是「请求参数
            # 错误」，跟释放看不出关系。
            url += "?" + urllib.parse.urlencode(data)
            data = None
        return self._unwrap(request_json(
            method, url,
            {"Authorization": self.token, "Content-Type": "application/json"},
            data))

    @staticmethod
    def _unwrap(payload):
        if payload.get("code") != "Success":
            raise CloudError(payload.get("msg") or ("code=%s" % payload.get("code")))
        return payload.get("data")

    def fields(self):
        return [
            Field("spec", "算力规格", "combo", self.SPECS, self.SPECS[0][0]),
            Field("gpu_num", "GPU 数量 (1-4)", "spin", default="1", lo=1, hi=4),
            Field("disk", "系统盘扩容 GB", "spin", default="0", lo=0, hi=500, step=10),
            Field("image", "镜像", "pick", self.IMAGES, self.IMAGES[0][0]),
            Field("cuda", "CUDA 下限", "combo", self.CUDA, self.CUDA[3][0]),
            Field("regions", "地区（都不勾=自动）", "checks", self.REGIONS),
            Field("name", "实例备注名", "entry", default="changji"),
            Field("command", "开机命令（选填）", "entry"),
        ]

    def list_images(self):
        rows = (self._call("POST", "/api/v1/dev/instance/pro/image/private/list",
                           {"page_index": 1, "page_size": 50}) or {}).get("list") or []
        return [{"id": it.get("image_uuid"), "name": it.get("name") or "",
                 "size": round((it.get("image_size") or 0) / 1e9, 1),
                 "status": it.get("status") or "", "region": ""} for it in rows]

    def save_image(self, iid, name):
        r = self._call("POST", "/api/v1/dev/instance/pro/image/save",
                       {"instance_uuid": iid, "image_name": name}) or {}
        return r.get("image_uuid")

    def load_choices(self):
        rows = (self._call("POST", "/api/v1/dev/instance/pro/image/private/list",
                           {"page_index": 1, "page_size": 50}) or {}).get("list") or []
        out = []
        for it in rows:
            n = "[私有] %s" % (it.get("name") or it.get("image_uuid"))
            if it.get("status") and it["status"] != "finished":
                n += "（%s）" % it["status"]
            out.append((n, it.get("image_uuid")))
        # 自己存的那份才是常用的，排在公共镜像前面。
        return {"image": out + self.IMAGES}

    def instances(self):
        rows = (self._call("POST", "/api/v1/dev/instance/pro/list",
                           {"page_index": 1, "page_size": 50}) or {}).get("list") or []
        out = []
        for it in rows:
            out.append({
                "id": it.get("uuid", ""),
                "name": it.get("name", ""),
                "status": it.get("status", ""),
                "spec": it.get("gpu_spec_uuid", ""),
                "gpu": it.get("req_gpu_amount", ""),
                "region": it.get("region_name") or it.get("region_sign", ""),
                "billing": "按量" if it.get("charge_type") == "payg" else (it.get("charge_type") or ""),
                "created": (it.get("created_at") or "")[:19].replace("T", " "),
            })
        return out

    def validate(self, v):
        if not v.get("image"):
            raise CloudError("镜像没填")

    def create(self, v):
        self.validate(v)
        body = {
            "req_gpu_amount": v["gpu_num"],
            "expand_system_disk_by_gb": v["disk"],
            "gpu_spec_uuid": v["spec"],
            "image_uuid": v["image"],
            "cuda_v_from": int(v["cuda"]),
        }
        # 选填的字段不填就别发空值——发了空串接口未必当作「没填」。
        if v.get("name"):
            body["instance_name"] = v["name"]
        if v.get("command"):
            body["start_command"] = v["command"]
        if v.get("regions"):
            body["data_center_list"] = v["regions"]
        return self._call("POST", "/api/v1/dev/instance/pro/create", body)

    def create_summary(self, v):
        return ("规格 %s × %d 卡\n镜像 %s\n地区 %s\n\n按量计费，创建后开始算钱。"
                % (v["spec"], v["gpu_num"], v["image"],
                   "、".join(v.get("regions") or []) or "自动"))

    def power_on(self, iid, command=""):
        body = {"instance_uuid": iid, "payload": "gpu"}   # API 起不了无卡模式
        if command:
            body["start_command"] = command
        self._call("POST", "/api/v1/dev/instance/pro/power_on", body)

    def power_off(self, iid):
        self._call("POST", "/api/v1/dev/instance/pro/power_off", {"instance_uuid": iid})

    def status(self, iid):
        return self._call("GET", "/api/v1/dev/instance/pro/status", {"instance_uuid": iid})

    def detail(self, iid):
        d = self._call("GET", "/api/v1/dev/instance/pro/snapshot", {"instance_uuid": iid}) or {}
        ssh = d.get("ssh_command") or "ssh -p %s root@%s" % (d.get("ssh_port", ""),
                                                             d.get("proxy_host", ""))
        lines = [ssh, "密码 %s" % d.get("root_password", "")]
        if d.get("jupyter_domain"):
            lines.append("JupyterLab https://%s  token=%s"
                         % (d["jupyter_domain"], d.get("jupyter_token", "")))
        u = d.get("usage_info") or {}
        if u.get("pull_image_progress", 1) < 1:
            lines.append("镜像还在拉：%.0f%%" % (u["pull_image_progress"] * 100))
        return lines, ssh

    def release(self, iid):
        self._call("POST", "/api/v1/dev/instance/pro/release", {"instance_uuid": iid})

    METRICS = True

    def metrics(self, iid):
        """CPU / 内存 / 磁盘。**这家不报 GPU 也不报网络**——2026-09-20 拿真
        GPU 机（pro6000-p）把 snapshot 的全部字段看了一遍，就这些。"""
        d = self._call("GET", "/api/v1/dev/instance/pro/snapshot",
                       {"instance_uuid": iid}) or {}
        u = d.get("usage_info") or {}
        rows = []
        cpu = u.get("cpu_usage_percent")
        if cpu is not None:
            rows.append(("CPU", "%.1f%%" % cpu, cpu))
        mu, ml = u.get("mem_usage"), u.get("mem_limit")
        if mu is not None and ml:
            rows.append(("内存", "%s / %s" % (human_bytes(mu), human_bytes(ml)),
                         mu * 100.0 / ml))
        du, dt = u.get("root_fs_used_size"), u.get("root_fs_total_size")
        if du is not None and dt:
            rows.append(("系统盘", "%s / %s" % (human_bytes(du), human_bytes(dt)),
                         du * 100.0 / dt))
        if u.get("data_disk_total_size"):
            used = u.get("data_disk_used_size") or 0
            rows.append(("数据盘", "%s / %s" % (human_bytes(used),
                                             human_bytes(u["data_disk_total_size"])),
                         used * 100.0 / u["data_disk_total_size"]))
        if d.get("snapshot_gpu_alias_name"):
            rows.append(("显卡", d["snapshot_gpu_alias_name"], None))
        prog = u.get("pull_image_progress")
        if prog is not None and prog < 1:
            rows.append(("拉镜像", "%.0f%%" % (prog * 100), prog * 100))
        notes = ["AutoDL 的接口只报 CPU / 内存 / 磁盘，"
                 "不报 GPU 利用率、也不报网络——那两样得 ssh 上去看。"]
        return {"rows": rows, "notes": notes}

    # 文档说「释放前请先关机，否则可能无法释放」。那就自己去关，别把这句话
    # 原样弹给用户——开着的机器点释放点不动，用户还得回控制台关机、等、再回
    # 来点一次，这一串本来就是机器该干的。关不掉才报，报的是真情况。
    def release_safely(self, iid, log, wait_s=180):
        st = self.status(iid)
        log("当前状态：%s" % st)
        if st not in OFF_STATES:
            log("AutoDL 释放前必须先关机，替你关了……")
            try:
                self.power_off(iid)
            except CloudError as e:
                # 已经在关机途中时 power_off 会报错，这不算失败，接着等状态。
                log("关机接口说：%s（接着等状态）" % e)
            st = self._wait_off(iid, log, wait_s)
            if st not in OFF_STATES:
                raise CloudError("等了 %d 秒还停在「%s」，没敢释放。去控制台看一眼这台机器"
                                 % (wait_s, st))
            log("已关机。")
        self.release(iid)
        log("已释放：%s" % iid)


class PPIOPlatform(Platform):
    """PPIO 派欧云 GPU 容器实例 v2。

    正经 REST：POST 建、PUT start/stop、DELETE 删，Bearer 鉴权。跟 AutoDL
    最大的不同是**规格不是写死的表**——product_id 得现拉，回包里带实时可用
    卡数和价格，所以换到这个平台会自动拉一次。
    """

    NAME = "PPIO"
    CONFIG_KEY = "ppio"
    CREDS = (("token", "API 密钥", ("PPIO_API_KEY", "PPIO_TOKEN")),)
    HOST = "https://api.ppio.com"
    TOKEN_HINT = "ppio.com → 设置 → API 密钥管理"
    BILLING_HINT = ("抢占式（spot）常常只要按量的一半，但会被抢走——\n"
                    "批量出片这种断了能重跑的活用它，别拿它当常驻机。\n"
                    "镜像填 docker 地址，不是 UUID。")
    DYNAMIC = ("product", "image")

    BILLING = [("按量 postpaid", "postpaid"), ("抢占式 spot（便宜、会被抢）", "spot")]

    def _call(self, method, path, body=None, query=None):
        self._need_token()
        url = self.HOST + path
        if query:
            url += "?" + urllib.parse.urlencode(query)
        return request_json(method, url,
                            {"Authorization": "Bearer " + self.token,
                             "Content-Type": "application/json"}, body)

    def fields(self):
        return [
            Field("product", "算力规格", "pick", [], ""),
            Field("gpu_num", "GPU 数量", "spin", default="1", lo=1, hi=8),
            Field("disk", "系统盘 GB", "spin", default="50", lo=20, hi=500, step=10),
            Field("image", "镜像（docker 地址）", "pick", [], ""),
            Field("billing", "计费方式", "combo", self.BILLING, self.BILLING[0][0]),
            Field("name", "实例名", "entry", default="changji"),
            Field("command", "启动命令（选填）", "entry"),
        ]

    @staticmethod
    def _price(block, precision):
        """价格是整数 + 小数位数，自己还原成「元」。没这档就是不支持。"""
        if not block:
            return None
        v = block.get("final_price")
        if v is None:
            return None
        return v / float(10 ** int(precision or 0))

    def load_choices(self):
        out = {}
        prods = (self._call("GET", "/gpus/v2/products",
                            query={"type": "gpu", "category": "instance",
                                   "limit": 100}) or {}).get("data") or []
        rows = []
        for p in prods:
            spec = p.get("resource_spec") or {}
            gpu = spec.get("gpu") or {}
            pricing = p.get("pricing") or {}
            prec = pricing.get("precision", 0)
            pay = self._price(pricing.get("postpaid"), prec)
            spot = self._price(pricing.get("spot"), prec)
            bits = [p.get("name") or p.get("id", "")]
            if gpu.get("memory_gb"):
                bits.append("%sG" % gpu["memory_gb"])
            # 可用卡数是实时的，挑规格时最想先看这个：0 就别选了。
            if gpu.get("available") is not None:
                bits.append("可用%s卡" % gpu["available"])
            if pay is not None:
                bits.append("按量¥%.2f/时" % pay)
            if spot is not None:
                bits.append("抢占¥%.2f/时" % spot)
            rows.append((" ".join(bits), p.get("id", "")))
        out["product"] = rows

        imgs = (self._call("GET", "/gpus/v2/images",
                           query={"limit": 100}) or {}).get("data") or []
        picks = []
        for im in imgs:
            for t in (im.get("tags") or []):
                full = t.get("image") or ""
                if full:
                    picks.append(("%s:%s" % (im.get("name", "?"), t.get("tag", "")), full))
            if not (im.get("tags") or []):
                if im.get("image"):
                    picks.append((im.get("name") or im["image"], im["image"]))
        out["image"] = picks
        return out

    PRICES = True

    def price_table(self, log, regions=None):
        """PPIO 的价目跟产品表是同一份，一次请求就有，不用按地域一个个问。"""
        prods = (self._call("GET", "/gpus/v2/products",
                            query={"type": "gpu", "category": "instance",
                                   "limit": 100}) or {}).get("data") or []
        log("产品 %d 种" % len(prods))
        rows = []
        for p in prods:
            spec = p.get("resource_spec") or {}
            g = spec.get("gpu") or {}
            pricing = p.get("pricing") or {}
            prec = pricing.get("precision", 0)
            spot = self._price(pricing.get("spot"), prec)
            pay = self._price(pricing.get("postpaid"), prec)
            best = spot if spot is not None else pay
            if best is None:
                continue          # 两档都没价就没什么可比的
            rows.append({
                "region": "、".join(p.get("region_ids") or []) or "自动调度",
                "zone": "", "spec": p.get("name") or p.get("id"),
                "gpu": g.get("name", ""), "n": g.get("max", ""),
                "vram": g.get("memory_gb", ""),      # 单卡显存，跟系统内存两回事
                "cpu": spec.get("cpu", ""), "mem": spec.get("memory_gb", ""),
                "spot": best, "origin": pay,
                "note": ("可用 %s 卡" % g["available"]) if g.get("available") is not None else "",
                "pick": {"product": p.get("id", "")},
            })
        rows.sort(key=lambda r: r["spot"])
        return rows

    def instances(self):
        rows = (self._call("GET", "/gpus/v2/instances",
                           query={"limit": 100}) or {}).get("data") or []
        out = []
        for it in rows:
            spec = it.get("resource_specs") or it.get("resource") or {}
            ts = it.get("created_at") or 0
            out.append({
                "id": it.get("id", ""),
                "name": it.get("name", ""),
                "status": (it.get("status") or {}).get("status", ""),
                "spec": it.get("product_id", ""),
                "gpu": spec.get("gpu_num", ""),
                "region": it.get("region", ""),
                "billing": (it.get("billing") or {}).get("mode", ""),
                "created": time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(ts)) if ts else "",
            })
        return out

    def validate(self, v):
        if not v.get("product"):
            raise CloudError("算力规格没填。先点「拉规格 / 镜像」——PPIO 的 product_id 要现拉")
        if not v.get("image"):
            raise CloudError("镜像没填（PPIO 要 docker 地址，像 registry.xxx/ns/img:tag）")

    def create(self, v):
        self.validate(v)
        body = {
            "name": v.get("name") or "changji",
            "product_id": v["product"],
            "image": v["image"],
            "type": "gpu",
            "billing": {"mode": v["billing"]},
            "resource": {"rootfs_size_gb": v["disk"], "gpu_num": v["gpu_num"]},
        }
        if v.get("command"):
            body["command"] = v["command"]
        return (self._call("POST", "/gpus/v2/instances", body) or {}).get("id")

    def create_summary(self, v):
        mode = "抢占式（会被抢走）" if v["billing"] == "spot" else "按量"
        return ("规格 %s × %d 卡\n镜像 %s\n系统盘 %d GB\n计费 %s\n\n创建后开始算钱。"
                % (v["product"], v["gpu_num"], v["image"], v["disk"], mode))

    def power_on(self, iid, command=""):
        # 启动接口不收启动命令，那是建机时定的。收了也没处放，所以不假装收。
        self._call("PUT", "/gpus/v2/instances/%s/start" % iid)

    def power_off(self, iid):
        self._call("PUT", "/gpus/v2/instances/%s/stop" % iid)

    def _get(self, iid):
        return self._call("GET", "/gpus/v2/instances/%s" % iid) or {}

    def status(self, iid):
        return (self._get(iid).get("status") or {}).get("status", "")

    def detail(self, iid):
        d = self._get(iid)
        tools = d.get("tools") or {}
        ssh = (tools.get("ssh") or {}).get("command", "")
        lines = []
        if ssh:
            lines.append(ssh)
            pw = (tools.get("ssh") or {}).get("password")
            if pw:
                lines.append("密码 %s" % pw)
        else:
            lines.append("这台还没给出 SSH（可能还没跑起来，或者没开 SSH 工具）")
        jup = (tools.get("jupyter") or {})
        if jup.get("endpoint"):
            lines.append("Jupyter %s" % jup["endpoint"])
        for p in (d.get("ports") or []):
            if p.get("endpoint"):
                lines.append("端口 %s/%s → %s" % (p.get("port"), p.get("protocol"), p["endpoint"]))
        st = d.get("status") or {}
        if st.get("message"):
            lines.append("状态说明：%s" % st["message"])
        return lines, ssh

    def release(self, iid):
        self._call("DELETE", "/gpus/v2/instances/%s" % iid)

    def confirm_release_text(self, iid, name):
        return ("要删除 %s（%s）吗？\n\n"
                "删除 = 实例和系统盘一起没，这一步不可逆。\n"
                "开着也能删；删不掉的话会先替你停机再删一次。"
                % (iid, name or "无名"))

    # PPIO 没说删除前必须停机，所以**先直接删**——常见情况下这一步就成了，
    # 没必要让用户多等一轮停机。删不动才退回「停机 → 等停稳 → 再删」。
    def release_safely(self, iid, log, wait_s=180):
        try:
            self.release(iid)
        except CloudError as e:
            log("直接删没成：%s" % e)
            log("那就先停机再删……")
            self.power_off(iid)
            st = self._wait_off(iid, log, wait_s)
            if st not in OFF_STATES:
                raise CloudError("等了 %d 秒还停在「%s」，没敢再删。去控制台看一眼这台机器"
                                 % (wait_s, st))
            self.release(iid)
        log("已删除：%s" % iid)


class AliyunPlatform(Platform):
    """阿里云 ECS 抢占式实例（API 版本 2014-05-26）。

    跟前两家差得最远的三处：

    1. **钥匙是两段**（AccessKey ID + Secret），而且请求要**签名**——不是塞个
       header 就行。签的是排序后的整串参数，见 `_sign`。
    2. **一切按地域**。规格、镜像、安全组、交换机全是某个地域里的东西，换了
       地域这几张表都得重拉，所以 REFRESH_ON 里写着 region。
    3. **建机之前得先有网**：VPC 交换机和安全组这个界面不负责创建，只负责让你
       从已有的里面挑。控制台上建过一次 ECS 的账号都已经有了。

    抢占式的脾气也要说清：**它会被回收**（官方给 5 分钟通知），而且**关机之后
    再开机不保证开得起来**——容量被别人买走就开不回来了。所以这一家适合
    「跑完就释放」，不适合当常驻协调机。
    """

    NAME = "阿里云"
    CONFIG_KEY = "aliyun"
    CREDS = (("key_id", "AccessKey ID", ("ALIBABA_CLOUD_ACCESS_KEY_ID", "ALICLOUD_ACCESS_KEY")),
             ("key_secret", "AccessKey Secret",
              ("ALIBABA_CLOUD_ACCESS_KEY_SECRET", "ALICLOUD_SECRET_KEY")))
    TOKEN_HINT = "控制台右上角头像 → AccessKey 管理（建议用 RAM 子账号，只给 ECS 权限）"
    # 提示语是给 QLabel 用的，它不认 markdown——写 ** 就真的显示两个星号。
    # 要强调用「」，这也是这个仓库文案的写法。
    BILLING_HINT = ("抢占式最便宜，但「会被回收」，官方提前 5 分钟通知。\n"
                    "关机之后再开机不保证开得起来——容量被别人买走就回不来了，\n"
                    "所以这家适合「跑完就释放」，不适合当常驻协调机。\n"
                    "公网按流量计费，带宽那个数只是峰值上限、本身不花钱，\n"
                    "所以默认拉满 100——填 5 和填 100 账单一样，下载快 20 倍。\n"
                    "交换机和安全组要账号里已经有（控制台建过一次 ECS 就有）；\n"
                    "交换机的可用区必须和规格的可用区是同一个。\n"
                    "带宽填 0 就没有公网 IP，SSH 连不上。")
    DYNAMIC = ("region", "spec", "image", "sg", "vsw", "ram_role")
    REFRESH_ON = ("region",)
    CAN_IMAGE = True
    CAN_COPY_IMAGE = True
    VERSION = "2014-05-26"
    LATENCY_HOST = "oss-%s.aliyuncs.com"
    USERDATA_KB = 32
    # 服务 → (域名模板, 版本)。RAM 的域名**不带地域**，版本也跟 ECS 不是一个。
    SERVICES = {"ecs": ("ecs.%s.aliyuncs.com", "2014-05-26"),
                "ram": ("ram.aliyuncs.com", "2015-05-01")}

    # 地域先给一张常见的表垫着：DescribeRegions 本身也要挑个地域的接入点才能
    # 发出去，空着的话第一次拉就无从拉起。拉回来之后这张表会被真的替换掉。
    SEED_REGIONS = [("华东1（杭州）", "cn-hangzhou"), ("华北2（北京）", "cn-beijing"),
                    ("华东2（上海）", "cn-shanghai"), ("华南1（深圳）", "cn-shenzhen"),
                    ("华北3（张家口）", "cn-zhangjiakou"), ("西南1（成都）", "cn-chengdu"),
                    ("中国香港", "cn-hongkong"), ("新加坡", "ap-southeast-1")]
    DISKS = [("ESSD 云盘", "cloud_essd"), ("SSD 云盘", "cloud_ssd"),
             ("高效云盘", "cloud_efficiency")]
    STRATEGY = [("随市场价（SpotAsPriceGo）", "SpotAsPriceGo"),
                ("设价格上限（SpotWithPriceLimit）", "SpotWithPriceLimit")]

    def __init__(self, creds=None):
        Platform.__init__(self, creds)
        self.region = "cn-hangzhou"

    def set_context(self, values):
        # 列表、拉表、详情都得知道问哪个地域，而那是表单上选的。
        if values.get("region"):
            self.region = values["region"]

    @staticmethod
    def region_of(row):
        # 列表里那一栏存的是可用区（cn-shanghai-l），砍掉最后一段就是地域
        zone = str(row.get("region") or "")
        return zone.rsplit("-", 1)[0] if zone.count("-") >= 2 else zone

    # ---- 签名 ---------------------------------------------------------

    @staticmethod
    def _percent(v):
        # RFC3986：只有 A-Za-z0-9-_.~ 不编码。urllib 的 quote 给了这份 safe
        # 之后就是对的（空格→%20、*→%2A、~ 保持原样），不用再手工替换。
        return urllib.parse.quote(str(v), safe="-_.~")

    @classmethod
    def _string_to_sign(cls, method, params):
        canon = "&".join("%s=%s" % (cls._percent(k), cls._percent(params[k]))
                         for k in sorted(params))
        return "%s&%s&%s" % (method, cls._percent("/"), cls._percent(canon))

    @classmethod
    def _sign(cls, method, params, secret):
        """签名 = base64(HMAC-SHA1(secret + "&", StringToSign))。

        那个**多出来的 & 不是笔误**，是阿里云规定的：密钥要拼一个 & 再当
        HMAC 的 key。漏了它签出来的东西永远不对，而接口只会回一句
        SignatureDoesNotMatch。

        ⚠️ **别照着文档末尾那个签名常量"修"这段代码。** 阿里云签名文档
        （help.aliyun.com/zh/ros/signature-method）里那个例子，
        AccessKeySecret=testsecret、StringToSign 是
        `GET&%2F&AccessKeyId%3Dtestid%26Action%3DDescribeRegions%26…`，
        它标的签名值是 `OLeaidS1JvxuMvnyHOwuJ+uX5qY=`——**那个值是过期的**，
        例子参数改过而签名串没跟着改。同一份 StringToSign 用 openssl
        （`openssl dgst -sha1 -hmac 'testsecret&'`）算出来是
        `u5GLRDKD9xTcL8TpK+1XvnDlVx8=`，和这段代码一致。文档里能对的是
        **算法描述和那串 StringToSign**，不是那个常量。
        """
        sts = cls._string_to_sign(method, params)
        mac = hmac.new((secret + "&").encode("utf-8"),
                       sts.encode("utf-8"), hashlib.sha1)
        return base64.b64encode(mac.digest()).decode("ascii")

    def _call(self, action, params=None, region=None, service="ecs"):
        self._need_token()
        host_tpl, version = self.SERVICES[service]
        p = dict(params or {})
        p.update({
            "Action": action,
            "Version": version,
            "Format": "JSON",
            "AccessKeyId": self.creds["key_id"],
            "SignatureMethod": "HMAC-SHA1",
            "SignatureVersion": "1.0",
            "SignatureNonce": uuid.uuid4().hex,
            "Timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        })
        p = {k: v for k, v in p.items() if v not in (None, "")}
        # **用 POST 不用 GET**：建机要带实例登录密码，密码不能出现在 URL 里
        # （日志、代理、浏览器历史都会留下来）。POST 的话签的是 "POST"。
        p["Signature"] = self._sign("POST", p, self.creds["key_secret"])
        where = region or self.region or "cn-hangzhou"
        host = host_tpl % where if "%s" in host_tpl else host_tpl
        try:
            return request_json("POST", "https://" + host + "/", {}, form=p, timeout=45)
        except CloudError as e:
            # 光报一句 HTTP 500 没法查是哪一步出的事——把动作和接入点带上。
            raise CloudError("%s@%s：%s" % (action, where, e))

    @staticmethod
    def _dig(d, *path):
        """阿里云的回包套得深（Instances→Instance→[...]），一层层安全地挖。"""
        cur = d
        for k in path:
            if not isinstance(cur, dict):
                return []
            cur = cur.get(k)
        return cur if isinstance(cur, list) else ([] if cur is None else cur)

    # ---- 表单 ---------------------------------------------------------

    def fields(self):
        return [
            Field("region", "地域", "combo", self.SEED_REGIONS, self.SEED_REGIONS[0][0]),
            Field("spec", "可用区 · 规格", "pick", [], ""),
            Field("image", "镜像", "pick", [], ""),
            Field("sg", "安全组", "pick", [], ""),
            Field("vsw", "交换机", "pick", [], ""),
            Field("disk", "系统盘 GB", "spin", default="100", lo=40, hi=500, step=10),
            Field("disk_cat", "系统盘类型", "combo", self.DISKS, self.DISKS[0][0]),
            Field("strategy", "出价", "combo", self.STRATEGY, self.STRATEGY[0][0]),
            Field("price", "价格上限 元/时", "entry"),
            # 默认拉满 100。按流量计费下这个数**只是峰值上限，本身不花钱**，
            # 花的是实际跑掉的 GB——填 5 和填 100 账单一样，但拉模型权重时
            # 一个是 5 Mbps 一个是 100 Mbps。上限是 100（RunInstances 文档）。
            Field("bandwidth", "公网带宽上限 Mbps", "spin", default="100",
                  lo=0, hi=100, step=10),
            Field("name", "实例名", "entry", default="changji"),
            Field("password", "登录密码", "secret",
                  hint="8-30 位，四类字符里占三类"),
            Field("oss_src", "开机拉模型（选填）", "entry",
                  hint="oss://你的桶/models  留空=不拉"),
            Field("oss_dest", "拉到哪个目录", "entry", default="/root/models"),
            Field("ram_role", "RAM 角色", "pick", [], "",
                  hint="给实例读 OSS 的临时凭证；密钥不会落到机器上"),
        ]

    def load_choices(self):
        out = {}
        regions = [("%s（%s）" % (r.get("LocalName") or r.get("RegionId"), r.get("RegionId")),
                    r.get("RegionId"))
                   for r in self._dig(self._call("DescribeRegions"), "Regions", "Region")]
        if regions:
            out["region"] = regions

        # 哪些规格现在真的买得到抢占式：这一问直接把「没货」的挡在下拉外面。
        avail = {}
        res = self._call("DescribeAvailableResource",
                         {"RegionId": self.region, "DestinationResource": "InstanceType",
                          "InstanceChargeType": "PostPaid", "SpotStrategy": "SpotAsPriceGo"})
        for z in self._dig(res, "AvailableZones", "AvailableZone"):
            zid = z.get("ZoneId")
            for ar in self._dig(z, "AvailableResources", "AvailableResource"):
                for sr in self._dig(ar, "SupportedResources", "SupportedResource"):
                    if sr.get("Status") == "Available" and sr.get("Value"):
                        avail.setdefault(sr["Value"], []).append(zid)

        # 规格本身有几张什么卡，要另问一次，两边拼起来下拉里才看得懂。
        gpu = {}
        for t in self._dig(self._call("DescribeInstanceTypes"),
                           "InstanceTypes", "InstanceType"):
            if (t.get("GPUAmount") or 0) > 0:
                gpu[t.get("InstanceTypeId")] = t
        specs = []
        for itype, zones in avail.items():
            t = gpu.get(itype)
            if not t:
                continue            # 没卡的规格不往这个界面上放
            for zid in sorted(set(zones)):
                specs.append(("%s · %s ×%s · %sC%sG · %s"
                              % (zid, t.get("GPUSpec", "GPU"), t.get("GPUAmount"),
                                 t.get("CpuCoreCount"), t.get("MemorySize"), itype),
                              "%s|%s" % (zid, itype)))
        out["spec"] = sorted(specs)

        # **自定义镜像排在最前面**：模型烤进去的那个就在这儿，不列出来的话
        # 辛辛苦苦做好的镜像根本选不到（这是这个工具最早的一个缺口）。
        self._image_sizes = {}
        mine = []
        for im in self._dig(self._call(
                "DescribeImages", {"RegionId": self.region,
                                   "ImageOwnerAlias": "self", "PageSize": 100}),
                "Images", "Image"):
            self._image_sizes[im.get("ImageId")] = im.get("Size")
            mine.append(("[自定义] %s（%sGB）" % (im.get("ImageName"), im.get("Size")),
                         im.get("ImageId")))
        out["image"] = mine + [
            ("%s（%s）" % (im.get("ImageName"), im.get("OSName") or ""), im.get("ImageId"))
            for im in self._dig(self._call(
                "DescribeImages",
                {"RegionId": self.region, "ImageOwnerAlias": "system",
                 "OSType": "linux", "PageSize": 100}), "Images", "Image")]
        out["sg"] = [("%s（%s）" % (g.get("SecurityGroupName") or g.get("SecurityGroupId"),
                                  g.get("VpcId", "")), g.get("SecurityGroupId"))
                     for g in self._dig(self._call(
                         "DescribeSecurityGroups",
                         {"RegionId": self.region, "PageSize": 50}),
                         "SecurityGroups", "SecurityGroup")]
        try:
            out["ram_role"] = self.list_roles()
        except CloudError as e:
            # 没给 RAM 读权限也不该把整张表拖垮：这一栏空着，用的时候会提示
            out["ram_role"] = []
        # 交换机标上可用区：它必须和规格选的那个可用区一致，不标的话没法挑。
        out["vsw"] = [("%s · %s（%s）" % (v.get("ZoneId"),
                                        v.get("VSwitchName") or v.get("VSwitchId"),
                                        v.get("CidrBlock", "")), v.get("VSwitchId"))
                      for v in self._dig(self._call(
                          "DescribeVSwitches",
                          {"RegionId": self.region, "PageSize": 50}),
                          "VSwitches", "VSwitch")]
        return out

    # ---- 开机自动从 OSS 拉模型 ------------------------------------------
    #
    # 镜像适合「模型不常变」。常换的那几个走 OSS：**内网流量不计费、速度几百
    # MB/s**，50 GB 三五分钟，而且换模型不用重做镜像。
    #
    # 凭证怎么给，是这一段最要紧的决定：**绝不把 AccessKey 写进 UserData**。
    # UserData 存在实例配置里、机器上任何进程都能从元数据读出来，等于把账号
    # 的钥匙交出去。走 RAM 角色：实例自己去元数据换**临时**凭证，ossutil 的
    # EcsRamRole 模式认这个，机器上一个长期密钥都不落。

    BOOT_SCRIPT = """#!/bin/bash
# changji 开机拉模型（这个界面生成的，走内网不计流量费）
set -u
LOG=/var/log/changji-models.log
exec >>"$LOG" 2>&1
echo "=== $(date -Is) 开始 ==="
DEST=%(dest)s
mkdir -p "$DEST"
OSSUTIL=$(command -v ossutil64 || command -v ossutil || true)
if [ -z "$OSSUTIL" ]; then
  echo "装 ossutil……"
  curl -fsSL https://gosspublic.alicdn.com/ossutil/install.sh | bash \
    || { echo "装不上 ossutil：这台机器出不了公网？带宽是不是填了 0"; exit 1; }
  OSSUTIL=$(command -v ossutil64 || command -v ossutil)
fi
# --mode EcsRamRole：凭证从实例元数据现拿，会自动续期，机器上不存密钥
"$OSSUTIL" -e %(endpoint)s --mode EcsRamRole --ecs-role-name %(role)s \
  sync %(src)s "$DEST" -f -u --jobs 8 --parallel 8 \
  || { echo "同步失败。先手工跑一遍看报什么：$OSSUTIL -e %(endpoint)s --mode EcsRamRole --ecs-role-name %(role)s ls %(src)s"; exit 1; }
du -sh "$DEST"
# 拉完了留个记号，ssh 上去 ls 一下就知道能不能开工
touch /root/.changji-models-ready
echo "=== $(date -Is) 拉完了 ==="
"""

    def boot_script(self, v):
        """生成开机脚本。空的 oss_src 就不装这一套。"""
        src = (v.get("oss_src") or "").strip()
        if not src:
            return ""
        # 地域**直接从表单取**，不读 self.region：预览那条路（「看开机脚本」）
        # 不经过 set_context，读 self.region 会拿到构造时的默认值，于是预览
        # 出来的 endpoint 跟真正发出去的不是一个地域。截图时就是这么发现的。
        region = (v.get("region") or self.region or "cn-hangzhou")
        return self.BOOT_SCRIPT % {
            # 内网 endpoint：oss-<地域>-internal.aliyuncs.com。
            # 用公网那个的话，几十 GB 全按公网流量收费，而且慢得多。
            "endpoint": "oss-%s-internal.aliyuncs.com" % region,
            "role": (v.get("ram_role") or "").strip(),
            "src": src.rstrip("/") + "/",
            "dest": (v.get("oss_dest") or "/root/models").strip(),
        }

    def list_roles(self):
        """账号里的 RAM 角色。

        **自己建的排在前面。** ListRoles 把各种云产品的服务角色也一起回来了
        （实测一个普通账号 38 个，全是 AliyunXXXDefaultRole 这种），而给 ECS
        读 OSS 的那个角色是你自己建的。接口不回信任策略，没法真判「这个角色
        能不能挂给 ECS」，所以只按名字排个序——**是排序不是过滤**，别把真能用
        的挡在外面。
        """
        rows = self._dig(self._call("ListRoles", service="ram"), "Roles", "Role")
        rows.sort(key=lambda r: (str(r.get("RoleName") or "").startswith("Aliyun"),
                                 str(r.get("RoleName") or "")))
        return [("%s（%s）" % (r.get("RoleName"), (r.get("Description") or "")[:24]),
                 r.get("RoleName")) for r in rows]

    # ---- 自定义镜像 ----------------------------------------------------

    def list_images(self):
        return [{"id": im.get("ImageId"), "name": im.get("ImageName") or "",
                 "size": im.get("Size") or "", "status": im.get("Status") or "",
                 "region": self.region}
                for im in self._dig(self._call(
                    "DescribeImages", {"RegionId": self.region,
                                       "ImageOwnerAlias": "self", "PageSize": 100}),
                    "Images", "Image")]

    def save_image(self, iid, name):
        # 文档：实例要处于 Running 或 Stopped。**建议先关机**，不然缓存还没落盘，
        # 做出来的镜像里模型文件可能是半截的。
        r = self._call("CreateImage", {"RegionId": self.region, "InstanceId": iid,
                                       "ImageName": name})
        return (r or {}).get("ImageId")

    def copy_image(self, image_id, regions, log):
        # 同一个地域同时只能有 5 个复制任务，多了排队（OperationDenied.ImageCopyConflict）
        out = []
        for r in regions:
            try:
                new = (self._call("CopyImage",
                                  {"RegionId": self.region, "ImageId": image_id,
                                   "DestinationRegionId": r,
                                   "DestinationImageName": "changji-" + r}) or {})
                out.append((r, new.get("ImageId")))
                log("→ %s：已发起，新镜像 %s" % (r, new.get("ImageId")))
            except CloudError as e:
                log("→ %s：没发起来 %s" % (r, e))
        return out

    # ---- 建机 ---------------------------------------------------------

    @staticmethod
    def _check_password(pw):
        check_password(pw, 3)

    def validate(self, v):
        need = [("spec", "可用区 · 规格"), ("image", "镜像"),
                ("sg", "安全组"), ("vsw", "交换机")]
        empty = [label for key, label in need if not v.get(key)]
        if empty:
            raise CloudError("%s 没填。先点「拉规格 / 镜像」——阿里云这几样都得从"
                             "账号里现有的挑" % "、".join(empty))
        if "|" not in (v.get("spec") or ""):
            raise CloudError("规格要从下拉里挑（格式是 可用区|规格名）")
        self.check_zone(v["spec"].split("|", 1)[0], self.region)
        self._check_password(v.get("password") or "")
        self.check_disk(v.get("image"), v.get("disk") or 0)
        src = (v.get("oss_src") or "").strip()
        if src:
            if not src.startswith("oss://") or len(src) < 8:
                raise CloudError("「开机拉模型」要填 oss://桶名/前缀 这种写法，"
                                 "现在是「%s」" % src)
            if not (v.get("ram_role") or "").strip():
                raise CloudError("要开机拉模型就得给一个 RAM 角色——"
                                 "不然实例没有读 OSS 的凭证。\n"
                                 "（走角色是为了别把 AccessKey 塞进机器）")
            if not (v.get("bandwidth") or 0):
                raise CloudError("带宽填了 0 就没有公网 IP，装不了 ossutil，"
                                 "开机脚本会卡在第一步。拉模型的话带宽别填 0")
        if v.get("strategy") == "SpotWithPriceLimit":
            try:
                float(v.get("price") or "")
            except ValueError:
                raise CloudError("选了「设价格上限」就要填价格上限，单位是元/小时")

    def create(self, v):
        self.validate(v)
        zone, itype = v["spec"].split("|", 1)
        p = {
            "RegionId": self.region,
            "ZoneId": zone,
            "InstanceType": itype,
            "ImageId": v["image"],
            "SecurityGroupId": v["sg"],
            "VSwitchId": v["vsw"],
            "InstanceChargeType": "PostPaid",      # 抢占式属于按量付费
            "SpotStrategy": v["strategy"],
            "SystemDisk.Category": v["disk_cat"],
            "SystemDisk.Size": v["disk"],
            "InternetChargeType": "PayByTraffic",
            "InternetMaxBandwidthOut": v["bandwidth"],
            "InstanceName": v.get("name") or "changji",
            "Password": v["password"],
            "Amount": 1,
        }
        if v["strategy"] == "SpotWithPriceLimit":
            p["SpotPriceLimit"] = v["price"]
        script = self.boot_script(v)
        if script:
            # UserData 要 base64，编码前不超过 32 KB（RunInstances 文档）
            p["UserData"] = self.encode_userdata(script)
            p["RamRoleName"] = v["ram_role"]
        ids = self._dig(self._call("RunInstances", p), "InstanceIdSets", "InstanceIdSet")
        return ids[0] if ids else None

    def create_summary(self, v):
        zone, itype = (v.get("spec") or "|").split("|", 1)
        price = ("上限 %s 元/时" % v.get("price")) if v.get("strategy") == "SpotWithPriceLimit" \
            else "随市场价"
        return ("地域 %s · 可用区 %s\n规格 %s\n镜像 %s\n系统盘 %s GB %s\n"
                "公网带宽 %s Mbps\n出价 抢占式（%s）\n%s\n"
                "抢占式会被回收（提前 5 分钟通知）。确定？"
                % (self.region, zone, itype, v.get("image"), v.get("disk"),
                   v.get("disk_cat"), v.get("bandwidth"), price,
                   ("开机自动从 %s 拉模型到 %s（走内网，不计流量费）\n"
                    % (v.get("oss_src"), v.get("oss_dest") or "/root/models"))
                   if (v.get("oss_src") or "").strip() else ""))

    # ---- 列表 / 状态 / 详情 --------------------------------------------

    @staticmethod
    def _local_time(iso):
        """CreationTime 是 UTC 的 ISO 串，而且精度有时到分有时到秒，两种都认。"""
        for fmt in ("%Y-%m-%dT%H:%M:%SZ", "%Y-%m-%dT%H:%MZ"):
            try:
                t = time.strptime(iso, fmt)
            except (ValueError, TypeError):
                continue
            return time.strftime("%Y-%m-%d %H:%M:%S",
                                 time.localtime(calendar.timegm(t)))
        return iso or ""

    def _raw(self, iid=None):
        p = {"RegionId": self.region, "PageSize": 100}
        if iid:
            p["InstanceIds"] = json.dumps([iid])
        return self._dig(self._call("DescribeInstances", p), "Instances", "Instance")

    def instances(self):
        out = []
        for it in self._raw():
            spot = it.get("SpotStrategy") or ""
            out.append({
                "id": it.get("InstanceId", ""),
                "name": it.get("InstanceName", ""),
                "status": it.get("Status", ""),
                "spec": it.get("InstanceType", ""),
                "gpu": it.get("GPUAmount", "") or "",
                "billing": "抢占" if spot.startswith("Spot") else "按量",
                "region": it.get("ZoneId", ""),
                "created": self._local_time(it.get("CreationTime")),
            })
        self._fill_traffic(out)
        return out

    def status(self, iid):
        rows = self._raw(iid)
        return rows[0].get("Status", "") if rows else ""

    def detail(self, iid):
        rows = self._raw(iid)
        if not rows:
            return ["这台在 %s 里找不到（换个地域看看？）" % self.region], ""
        it = rows[0]
        ips = (it.get("PublicIpAddress") or {}).get("IpAddress") or []
        eip = ((it.get("EipAddress") or {}).get("IpAddress") or "")
        ip = (ips[0] if ips else "") or eip
        lines = []
        if ip:
            ssh = "ssh root@%s" % ip
            lines.append(ssh)
            lines.append("密码是你建机时填的那个（这个界面不存密码）")
        else:
            ssh = ""
            lines.append("没有公网 IP——建机时带宽填成 0 了，或者还没分配好")
        if it.get("GPUSpec"):
            lines.append("显卡 %s ×%s" % (it["GPUSpec"], it.get("GPUAmount")))
        lines.append("%s · %s核 %sMB · %s"
                     % (it.get("InstanceType"), it.get("Cpu"), it.get("Memory"),
                        it.get("Status")))
        if it.get("SpotPriceLimit"):
            lines.append("出价上限 %s 元/时" % it["SpotPriceLimit"])
        return lines, ssh

    # ---- 已用流量 ------------------------------------------------------
    #
    # 按流量计费花钱的是**出网**（InternetTX），入网不算钱，所以这一栏报的是
    # 出网。接口给的是每个 Period 段内的 kbits，自己累加再换成 GB。
    #
    # 为什么是「最近 24 小时」而不是「这台机器一共跑了多少」：监控接口只能按
    # 时间窗查，点数多了会直接回 InvalidParameter.TooManyDataQueried。24 小时
    # 配 Period=3600 就是 24 个点，稳、快，而且「跑飞了没有」本来就是看近况。
    TRAFFIC = True
    TRAFFIC_HOURS = 24
    TRAFFIC_TTL = 120        # 秒。自动刷新开着时别每 15 秒就问一遍监控接口

    @staticmethod
    def _utc(ts):
        # 秒不是 00 的话阿里云会自动进位到下一分钟，索性自己对齐到整分
        return time.strftime("%Y-%m-%dT%H:%M:00Z", time.gmtime(ts))

    def _traffic_gb(self, iid):
        now = time.time()
        rows = self._dig(self._call("DescribeInstanceMonitorData", {
            "InstanceId": iid,
            "StartTime": self._utc(now - self.TRAFFIC_HOURS * 3600),
            "EndTime": self._utc(now),
            "Period": 3600}), "MonitorData", "InstanceMonitorData")
        kbits = sum(r.get("InternetTX") or 0 for r in rows)
        # kbit → GB（十进制，跟账单口径一致）：×1000 bit ÷8 ÷1e9
        return kbits / 8e6

    METRICS = True

    @staticmethod
    def _latest(points, key):
        """从后往前找**第一个真有这一栏的采样点**。

        阿里云最新那个点常常只填了一半：2026-09-20 实测最后一个点的
        InternetTX / InternetRX / InternetBandwidth 全是 None，而上一个点是好的。
        直接取最后一个点的话，公网那几栏会时有时无地空掉，看着像网络断了。
        """
        for r in reversed(points or []):
            v = r.get(key)
            if v is not None:
                return v, r.get("TimeStamp")
        return None, None

    def metrics(self, iid):
        """CPU / 网络 / 磁盘 IO。**不报 GPU 也不报内存**——那两样要机器上装
        云监控插件才有，普通镜像没装。"""
        now = time.time()
        pts = self._dig(self._call("DescribeInstanceMonitorData", {
            "InstanceId": iid, "StartTime": self._utc(now - 900),
            "EndTime": self._utc(now), "Period": 60}),
            "MonitorData", "InstanceMonitorData")
        if not pts:
            return {"rows": [], "notes": ["这一刻没有监控数据。刚开机的话等一两分钟"
                                          "——监控是按分钟出点的。"]}
        rows = []
        cpu, ts = self._latest(pts, "CPU")
        if cpu is not None:
            rows.append(("CPU", "%s%%" % cpu, float(cpu)))
        # kbit/s → 更好读的单位；公网出网是要花钱的那一半，排前面
        for key, label in (("InternetBandwidth", "公网带宽"),
                           ("IntranetBandwidth", "内网带宽")):
            v, _ = self._latest(pts, key)
            if v is not None:
                rows.append((label, "%.2f Mbit/s" % (v / 1000.0), None))
        for key, label in (("InternetTX", "公网出（每分钟）"),
                           ("InternetRX", "公网入（每分钟）"),
                           ("IntranetTX", "内网出（每分钟）"),
                           ("IntranetRX", "内网入（每分钟）")):
            v, _ = self._latest(pts, key)
            if v is not None:
                rows.append((label, human_bytes(v * 1000 / 8.0), None))
        for key, label in (("BPSRead", "磁盘读"), ("BPSWrite", "磁盘写")):
            v, _ = self._latest(pts, key)
            if v is not None:
                rows.append((label, "%s/s" % human_bytes(v), None))
        for key, label in (("IOPSRead", "读 IOPS"), ("IOPSWrite", "写 IOPS")):
            v, _ = self._latest(pts, key)
            if v is not None:
                rows.append((label, str(v), None))
        notes = ["数据来自 ECS 自己的监控，按分钟出点，所以最快也是一分钟前的。"]
        if ts:
            notes.append("最后一个点：%s（UTC）" % ts)
        notes.append("GPU 利用率和内存这家不报——要在机器上装云监控插件才有，"
                     "普通镜像没装。GPU 还是 ssh 上去 nvidia-smi 最直接。")
        return {"rows": rows, "notes": notes}

    def _fill_traffic(self, rows):
        cache = getattr(self, "_tcache", None)
        if cache is None:
            cache = self._tcache = {}
        now = time.time()
        # 关着的机器不会再产生流量，缓存里有就直接用，没有也值得查一次
        todo = [r["id"] for r in rows
                if r["id"] and now - cache.get(r["id"], (0, None))[0] > self.TRAFFIC_TTL]

        def one(iid):
            try:
                return iid, self._traffic_gb(iid)
            except CloudError:
                return iid, None      # 查不到就空着，不要因为这一栏把整张表弄没

        if todo:
            with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
                for iid, gb in pool.map(one, todo):
                    if gb is not None:
                        cache[iid] = (now, gb)
        for r in rows:
            hit = cache.get(r["id"])
            r["traffic"] = hit[1] if hit else None

    # ---- 比价 ----------------------------------------------------------
    #
    # **抢占价按地域、按可用区各走各的**，同一块卡在两个地域能差好几倍，所以
    # 「哪儿最便宜」这件事只能问出来，不能猜。一次比价的代价：每个地域先问一次
    # 「有哪些 GPU 规格有货」，再按规格各取一次价（一次回全部可用区）。
    # 规格数不多，但地域一多就是几十个来回，所以并发发、逐条报进度。
    PRICES = True

    def _spot_prices(self, region, itype):
        """一个地域一个规格的抢占价，回的是各可用区最新的那一笔。

        默认时间窗是最近 3 小时，同一个可用区会回好几笔采样，所以按时间取最新。
        """
        rows = self._dig(self._call("DescribeSpotPriceHistory",
                                    {"RegionId": region, "NetworkType": "vpc",
                                     "InstanceType": itype, "OSType": "linux",
                                     "IoOptimized": "optimized"}, region=region),
                         "SpotPrices", "SpotPriceType")
        latest = {}
        for r in rows:
            z = r.get("ZoneId")
            if z and (z not in latest or (r.get("Timestamp") or "") > (latest[z].get("Timestamp") or "")):
                latest[z] = r
        return latest

    def _gpu_types(self):
        """规格 → 卡型号/卡数/核/内存。全局一张表，比价时只问一次。"""
        if getattr(self, "_gpu_cache", None):
            return self._gpu_cache
        out = {}
        for t in self._dig(self._call("DescribeInstanceTypes"),
                           "InstanceTypes", "InstanceType"):
            if (t.get("GPUAmount") or 0) > 0:
                out[t.get("InstanceTypeId")] = t
        self._gpu_cache = out
        return out

    def _retry(self, fn, *a, **kw):
        """限流或者服务端抽风就等一下再来。

        阿里云对这类查询接口有 QPS 门，几十个来回并发发出去必然撞上；5xx 也
        多半是一过性的。撞上就整张表少几行——那比慢几秒难查得多。
        """
        for i in range(3):
            try:
                return fn(*a, **kw)
            except CloudError as e:
                msg = str(e)
                transient = "Throttling" in msg or "HTTP 5" in msg
                if not transient or i == 2:
                    raise
                time.sleep(1.5 * (i + 1))

    def price_table(self, log, regions=None):
        gpu = self._gpu_types()
        log("规格表：%d 种带卡的" % len(gpu))
        if not regions:
            # **全部地域，不只国内。** 海外区常常比国内便宜一截，而多扫一倍
            # 地域也就多几秒——原来只扫 cn- 等于把便宜的那半截藏起来了。
            regions = [r.get("RegionId") for r in
                       self._dig(self._call("DescribeRegions"), "Regions", "Region")
                       if r.get("RegionId")]
        log("扫 %d 个地域（含海外）：%s" % (len(regions), "、".join(regions)))

        # 第一轮：每个地域有哪些带卡的规格现在真能买到抢占式
        def avail(region):
            # ⚠️ **region= 这个参数不能省。** 它定的是发到哪个接入点，而
            # DescribeAvailableResource 只认自己地域的接入点：拿
            # ecs.cn-hangzhou.aliyuncs.com 去问 cn-zhangjiakou，阿里云回
            # HTTP 500 UnknownError（16 个国内地域里 8 个这样）。更坑的是
            # cn-wuhan-lr / cn-zhongwei 这种——**不报错，直接回 0 条**，
            # 看着像「这地方没货」，其实是问错了地方。各发各的，16 个全通。
            res = self._retry(self._call, "DescribeAvailableResource",
                              {"RegionId": region, "DestinationResource": "InstanceType",
                               "InstanceChargeType": "PostPaid",
                               "SpotStrategy": "SpotAsPriceGo"}, region)
            got = set()
            for z in self._dig(res, "AvailableZones", "AvailableZone"):
                for ar in self._dig(z, "AvailableResources", "AvailableResource"):
                    for sr in self._dig(ar, "SupportedResources", "SupportedResource"):
                        if sr.get("Status") == "Available" and sr.get("Value") in gpu:
                            got.add(sr["Value"])
            return region, got

        # 一个地域问不出来，不该把整张表带走：记下来、说出来、接着问别的。
        pairs, broken = [], []
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            futs = {pool.submit(avail, r): r for r in regions}
            for fut in concurrent.futures.as_completed(futs):
                try:
                    region, types = fut.result()
                except CloudError as e:
                    broken.append("%s（%s）" % (futs[fut], e))
                    continue
                if types:
                    log("%s：%d 种有货" % (region, len(types)))
                pairs.extend((region, t) for t in sorted(types))
        if broken:
            log("这几个地域没问出来，跳过了：%s" % "；".join(broken))
        if not pairs:
            raise CloudError("这些地域现在都没有带卡的抢占式规格有货。"
                             "换个时间再看，或者去控制台确认账号能不能买 GPU 实例")
        log("要取价的（地域 × 规格）共 %d 笔……" % len(pairs))

        def one(pair):
            region, itype = pair
            return region, itype, self._retry(self._spot_prices, region, itype)

        rows = []
        done = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            futs = [pool.submit(one, pr) for pr in pairs]
            for fut in concurrent.futures.as_completed(futs):
                done += 1
                if done % 10 == 0:
                    log("  取价 %d/%d" % (done, len(pairs)))
                try:
                    region, itype, per_zone = fut.result()
                except CloudError as e:
                    broken.append(str(e))
                    continue
                t = gpu[itype]
                for zone, r in per_zone.items():
                    spot = r.get("SpotPrice")
                    origin = r.get("OriginPrice")
                    if spot is None:
                        continue
                    rows.append({
                        "region": region, "zone": zone, "spec": itype,
                        "gpu": t.get("GPUSpec", ""), "n": t.get("GPUAmount", ""),
                        "vram": (round(t["GPUMemorySize"])
                                 if t.get("GPUMemorySize") else ""),
                        "cpu": t.get("CpuCoreCount", ""),
                        "mem": round((t.get("MemorySize") or 0)),
                        "spot": float(spot),
                        "origin": float(origin) if origin is not None else None,
                        # 挑中之后要回填到表单里的那几栏，形状跟表单字段对齐
                        "pick": {"region": region, "spec": "%s|%s" % (zone, itype)},
                    })
        self._fill_latency(rows, log)
        rows.sort(key=lambda r: r["spot"])
        if broken:
            log("共 %d 笔没问出来（上面列了原因），下面这张表是问到的那些"
                % len(broken))
        log("拿到 %d 条报价，最便宜 %.3f 元/时（%s %s）"
            % (len(rows), rows[0]["spot"], rows[0]["region"], rows[0]["spec"])
            if rows else "一条报价都没拿到")
        return rows

    # ---- 开关机 / 释放 --------------------------------------------------

    def power_on(self, iid, command=""):
        # 抢占式开机不一定成：容量被别人买走就开不回来，接口会直接说。
        self._call("StartInstance", {"InstanceId": iid})

    def power_off(self, iid):
        self._call("StopInstance", {"InstanceId": iid})

    def release(self, iid):
        self._call("DeleteInstance", {"InstanceId": iid})

    def confirm_release_text(self, iid, name):
        return ("要释放 %s（%s）吗？\n\n"
                "释放 = 删除，实例和系统盘一起没，这一步不可逆。\n"
                "开着的话会先替你强制关机再删。"
                % (iid, name or "无名"))

    # 阿里云的 DeleteInstance 要求实例是 Stopped（除非加 Force）。
    # 关机用 ForceStop=true（相当于拔电）——反正下一步就是把这块盘删掉，
    # 优雅关机在这儿只会让人多等一轮，卡住了还不知道在等什么。
    def release_safely(self, iid, log, wait_s=180):
        st = self.status(iid)
        log("当前状态：%s" % (st or "查不到"))
        if not is_off(st):
            log("阿里云删实例前要求是 Stopped，替你关了（强制，盘马上就要删掉）……")
            try:
                self._call("StopInstance", {"InstanceId": iid, "ForceStop": "true"})
            except CloudError as e:
                log("关机接口说：%s（接着等状态）" % e)
            st = self._wait_off(iid, log, wait_s)
            if not is_off(st):
                raise CloudError("等了 %d 秒还停在「%s」，没敢删。去控制台看一眼这台机器"
                                 % (wait_s, st))
            log("已关机。")
        self.release(iid)
        log("已释放：%s" % iid)


class TencentPlatform(Platform):
    """腾讯云 CVM 竞价实例（API 3.0，版本 2017-03-12）。

    跟阿里云同样是签名式接口，但三处完全不同：

    1. **签名是 TC3-HMAC-SHA256**（AWS SigV4 那一套）：先拼规范请求串、再拼
       待签串、再逐层派生密钥。比阿里云那个 HMAC-SHA1 绕得多，见 `_sign`。
    2. **错误是用 HTTP 200 回的**，包在 `Response.Error` 里。不挑出来就会把
       「余额不足」当成建机成功——所有回包都得过 `_unwrap`。
    3. **地域走请求头 X-TC-Region，不换域名**。所以没有阿里云那个「发错接入点
       就 500」的坑。

    竞价实例的脾气跟阿里云一样：会被回收，适合「跑完就释放」。
    """

    NAME = "腾讯云"
    CONFIG_KEY = "tencent"
    CREDS = (("secret_id", "SecretId", ("TENCENTCLOUD_SECRET_ID",)),
             ("secret_key", "SecretKey", ("TENCENTCLOUD_SECRET_KEY",)))
    TOKEN_HINT = "控制台 → 访问管理 → API 密钥管理（建议用子账号，只给 CVM+VPC 权限）"
    BILLING_HINT = ("竞价实例（SPOTPAID）最便宜，但「会被回收」。\n"
                    "公网按流量计费，带宽那个数只是峰值上限、本身不花钱，\n"
                    "所以默认拉满 100。\n"
                    "私有网络子网和安全组要账号里已经有（控制台建过一次就有）；\n"
                    "子网的可用区必须和规格的可用区是同一个。\n"
                    "带宽填 0 就不分配公网 IP，SSH 连不上。\n"
                    "「出网24h」这一栏在这家是空的：流量要走另一套监控接口，\n"
                    "口径没核准之前不报一个可能是错的数。")
    DYNAMIC = ("region", "spec", "image", "sg", "subnet", "cam_role")
    REFRESH_ON = ("region",)
    PRICES = True
    TRAFFIC = False          # 监控是另一套接口，口径没核准之前不报
    CAN_IMAGE = True
    CAN_COPY_IMAGE = True
    LATENCY_HOST = "cos.%s.myqcloud.com"    # 腾讯云自己的地域接入点

    # 服务 → (域名, 版本)。安全组和子网在 vpc 那套里，不在 cvm。
    # 安全组和子网在 vpc 那套里、CAM 角色在 cam 那套里，都不在 cvm
    SERVICES = {"cvm": ("cvm.tencentcloudapi.com", "2017-03-12"),
                "vpc": ("vpc.tencentcloudapi.com", "2017-03-12"),
                "cam": ("cam.tencentcloudapi.com", "2019-01-16")}
    # UserData 上限 16 KB（编码前），比阿里云那边小一半
    USERDATA_KB = 16
    ALGO = "TC3-HMAC-SHA256"
    CT = "application/json; charset=utf-8"

    # 机型族 → (显卡型号, 单卡显存 GiB)。
    #
    # **为什么要这张表**：腾讯云的规格接口不报显存。`Gpu` 那个字段是**卡数
    # （整数）不是型号**，`GpuCount` 是物理卡数（vGPU 切片小于 1），两个都不是
    # 显存。查了一圈，DescribeZoneInstanceConfigInfos 和
    # DescribeInstanceTypeConfigs 都没有单卡显存这一项，只能按机型族对。
    # 出处：cloud.tencent.com/document/product/560/19700（2026-09-19 抄的）。
    #
    # 接口哪天真回了 GpuType / GpuMemory，代码会优先用接口的，这张表只兜底；
    # 族名不在表里就**留空**，不猜——一个错的显存比没有显存更坏（筛选会放行
    # 根本跑不动的机器）。
    GPU_FAMILY = {
        "PNV4": ("NVIDIA A10", 24), "GT4": ("NVIDIA A100 NVLink", 40),
        "GN10Xp": ("NVIDIA V100 NVLink", 32), "GN10X": ("NVIDIA V100 NVLink", 32),
        "GN7": ("NVIDIA T4", 16), "GN7vi": ("NVIDIA T4", 16),
        "GI3X": ("NVIDIA T4", 16), "GN8": ("NVIDIA P40", 24),
        "GN6": ("NVIDIA P4", 8), "GN6S": ("NVIDIA P4", 8),
        "PNV5b": ("NVIDIA", 48), "PNV6": ("NVIDIA", 96), "PNV6s": ("NVIDIA", 141),
    }

    # 从卡型号认显存。机型族一直在长（HCCPNV4h、HCCG5v、BMG5t 都是真实跑出来
    # 的、不在官方那张计算型表里），但卡就那么几种，按卡认更扛得住。
    # A100 有 40G 和 80G 两种，光看名字分不出来，所以**不填**。
    CARD_VRAM = {"T4": 16, "V100": 32, "P4": 8, "P40": 24, "A10": 24,
                 "L20": 48, "L40S": 48}

    @classmethod
    def _gpu_of(cls, q):
        """从一条规格里挖出（显卡型号, 单卡显存, 卡数）。接口给的优先。"""
        count = q.get("GpuCount")
        if count is None:
            count = q.get("Gpu")          # Gpu 是整数卡数，不是型号
        card, vram = cls.GPU_FAMILY.get(q.get("InstanceFamily") or "", ("", ""))
        # Remark 里常常直接写着卡（'4 颗 NVIDIA T4'、'8 * NVIDIA A100'），
        # 这是接口自己给的，比按机型族猜靠谱，优先用。
        m = re.search(r"NVIDIA\s+([A-Za-z0-9]+)", q.get("Remark") or "")
        if m:
            card = "NVIDIA " + m.group(1)
            vram = cls.CARD_VRAM.get(m.group(1).upper(), vram)
        card = q.get("GpuType") or card or (q.get("InstanceFamily") or "")
        vram = q.get("GpuMemory") or vram
        # vGPU 切片（物理卡数小于 1）按比例折算，不然 1/4 张 T4 会报成 16G，
        # 显存筛选就会把跑不动的机器放进来
        if vram and isinstance(count, (int, float)) and 0 < count < 1:
            vram = round(vram * count, 1)
        return card, vram, count

    SEED_REGIONS = [("华南地区（广州）", "ap-guangzhou"), ("华东地区（上海）", "ap-shanghai"),
                    ("华北地区（北京）", "ap-beijing"), ("西南地区（成都）", "ap-chengdu"),
                    ("华东地区（南京）", "ap-nanjing"), ("港澳台地区（中国香港）", "ap-hongkong"),
                    ("亚太东南（新加坡）", "ap-singapore"), ("美国西部（硅谷）", "na-siliconvalley")]
    DISKS = [("高性能云硬盘", "CLOUD_PREMIUM"), ("SSD 云硬盘", "CLOUD_SSD"),
             ("通用型 SSD", "CLOUD_BSSD"), ("增强型 SSD", "CLOUD_HSSD")]
    STRATEGY = [("随市场价", "market"), ("设价格上限", "limit")]

    def __init__(self, creds=None):
        Platform.__init__(self, creds)
        self.region = "ap-guangzhou"

    def set_context(self, values):
        if values.get("region"):
            self.region = values["region"]

    @staticmethod
    def region_of(row):
        # 腾讯云一样：ap-shanghai-2 → ap-shanghai
        zone = str(row.get("region") or "")
        return zone.rsplit("-", 1)[0] if zone.count("-") >= 2 else zone

    # ---- 签名 ---------------------------------------------------------

    @staticmethod
    def _hmac(key, msg):
        return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()

    @classmethod
    def _canonical_request(cls, action, host, body):
        """规范请求串。六行：方法、URI、查询串、请求头、签名头列表、请求体哈希。

        请求头那一段**每行结尾都要有换行**，所以最后会多出一个空行——
        少了那个空行整串就对不上，而接口只会回一句 AuthFailure。
        """
        headers = ("content-type:%s\nhost:%s\nx-tc-action:%s\n"
                   % (cls.CT, host, action.lower()))
        return "\n".join(["POST", "/", "", headers, "content-type;host;x-tc-action",
                          hashlib.sha256(body.encode("utf-8")).hexdigest()])

    @classmethod
    def _string_to_sign(cls, action, host, body, service, ts):
        date = time.strftime("%Y-%m-%d", time.gmtime(ts))
        return "\n".join([
            cls.ALGO, str(ts), "%s/%s/tc3_request" % (date, service),
            hashlib.sha256(cls._canonical_request(action, host, body)
                           .encode("utf-8")).hexdigest()])

    def _authorization(self, action, host, body, service, ts):
        date = time.strftime("%Y-%m-%d", time.gmtime(ts))
        # 逐层派生：TC3+密钥 → 日期 → 服务 → tc3_request。每一层的输出是下一层的 key。
        k = self._hmac(("TC3" + self.creds["secret_key"]).encode("utf-8"), date)
        k = self._hmac(k, service)
        k = self._hmac(k, "tc3_request")
        sig = hmac.new(k, self._string_to_sign(action, host, body, service, ts)
                       .encode("utf-8"), hashlib.sha256).hexdigest()
        return ("%s Credential=%s/%s/%s/tc3_request, "
                "SignedHeaders=content-type;host;x-tc-action, Signature=%s"
                % (self.ALGO, self.creds["secret_id"], date, service, sig))

    @staticmethod
    def _unwrap(payload):
        """**错误是 HTTP 200 回的**，包在 Response.Error 里。不挑出来就会把
        「余额不足」「没权限」当成建机成功。"""
        resp = (payload or {}).get("Response")
        if not isinstance(resp, dict):
            raise CloudError("回包里没有 Response：%s" % str(payload)[:200])
        err = resp.get("Error")
        if err:
            msg = err.get("Message") or ""
            code = err.get("Code") or ""
            raise CloudError(("%s（%s）" % (msg, code)) if code else msg)
        return resp

    def _call(self, action, payload=None, service="cvm", region=None):
        self._need_token()
        host, version = self.SERVICES[service]
        # 签名签的是**这串字节本身**，所以发出去的必须是同一串，不能重新序列化
        body = json.dumps(payload or {})
        ts = int(time.time())
        headers = {
            "Content-Type": self.CT,
            "Host": host,
            "X-TC-Action": action,
            "X-TC-Version": version,
            "X-TC-Timestamp": str(ts),
            "X-TC-Region": region or self.region,
            "Authorization": self._authorization(action, host, body, service, ts),
        }
        try:
            return self._unwrap(request_json("POST", "https://" + host + "/",
                                             headers, raw=body, timeout=45))
        except CloudError as e:
            raise CloudError("%s@%s：%s" % (action, region or self.region, e))

    # ---- 表单 ---------------------------------------------------------

    def fields(self):
        return [
            Field("region", "地域", "combo", self.SEED_REGIONS, self.SEED_REGIONS[0][0]),
            Field("spec", "可用区 · 规格", "pick", [], ""),
            Field("image", "镜像", "pick", [], ""),
            Field("sg", "安全组", "pick", [], ""),
            Field("subnet", "子网", "pick", [], ""),
            Field("disk", "系统盘 GB", "spin", default="100", lo=50, hi=1024, step=10),
            Field("disk_type", "系统盘类型", "combo", self.DISKS, self.DISKS[0][0]),
            Field("strategy", "出价", "combo", self.STRATEGY, self.STRATEGY[0][0]),
            Field("price", "价格上限 元/时", "entry"),
            Field("bandwidth", "公网带宽上限 Mbps", "spin", default="100",
                  lo=0, hi=100, step=10),
            Field("name", "实例名", "entry", default="changji"),
            Field("password", "登录密码", "secret",
                  hint="8-30 位，至少两类字符"),
            Field("cos_src", "开机拉模型（选填）", "entry",
                  hint="cos://桶名-APPID/models  留空=不拉"),
            Field("cos_dest", "拉到哪个目录", "entry", default="/root/models"),
            Field("cam_role", "CAM 角色", "pick", [], "",
                  hint="给实例读 COS 的临时凭证；密钥不会落到机器上"),
        ]

    @staticmethod
    def _real_price(q):
        """这一条实际每小时多少钱。

        UnitPrice 是**原价**，UnitPriceDiscount 才是实付：竞价那边是两折，
        按量那边有时也有折扣（实测 HCCG5v 155.48 → 93.29）。
        """
        price = (q or {}).get("Price") or {}
        v = price.get("UnitPriceDiscount")
        if v is None:
            v = price.get("UnitPrice")
        return v

    def _gpu_quota(self, region, charge):
        """某地域某计费方式下，带卡的规格。一次问全，回的里面就带价格和库存。"""
        rows = self._call("DescribeZoneInstanceConfigInfos",
                          {"Filters": [{"Name": "instance-charge-type",
                                        "Values": [charge]}]},
                          region=region).get("InstanceTypeQuotaSet") or []
        return [q for q in rows if (q.get("GpuCount") or 0) > 0]

    def load_choices(self):
        out = {}
        regions = [("%s（%s）" % (r.get("RegionName") or r.get("Region"), r.get("Region")),
                    r.get("Region"))
                   for r in (self._call("DescribeRegions").get("RegionSet") or [])
                   if r.get("RegionState") in (None, "AVAILABLE")]
        if regions:
            out["region"] = regions

        specs = []
        for q in self._gpu_quota(self.region, "SPOTPAID"):
            if q.get("Status") != "SELL":
                continue          # 没货的不往下拉里放
            card, vram, count = self._gpu_of(q)
            specs.append(("%s · %s ×%s%s · %sC%sG · %s"
                          % (q.get("Zone"), card or "GPU", count,
                             ("（%sG 显存）" % vram) if vram else "",
                             q.get("Cpu"), q.get("Memory"), q.get("InstanceType")),
                          "%s|%s" % (q.get("Zone"), q.get("InstanceType"))))
        out["spec"] = sorted(specs)

        self._image_sizes = {}
        mine = []
        for im in (self._call("DescribeImages", {
                "Filters": [{"Name": "image-type", "Values": ["PRIVATE_IMAGE"]}],
                "Limit": 100}).get("ImageSet") or []):
            self._image_sizes[im.get("ImageId")] = im.get("ImageSize")
            mine.append(("[自定义] %s（%sGB）" % (im.get("ImageName"),
                                              im.get("ImageSize")), im.get("ImageId")))
        out["image"] = mine + [
            ("%s（%s）" % (im.get("ImageName"), im.get("OsName") or ""), im.get("ImageId"))
            for im in (self._call("DescribeImages", {
                "Filters": [{"Name": "image-type", "Values": ["PUBLIC_IMAGE"]},
                            {"Name": "platform", "Values": ["Ubuntu"]}],
                "Limit": 100}).get("ImageSet") or [])]
        out["sg"] = [("%s（%s）" % (g.get("SecurityGroupName"), g.get("SecurityGroupId")),
                      g.get("SecurityGroupId"))
                     for g in (self._call("DescribeSecurityGroups", {"Limit": "50"},
                                          service="vpc").get("SecurityGroupSet") or [])]
        try:
            out["cam_role"] = self.list_roles()
        except CloudError:
            out["cam_role"] = []      # 没给 CAM 读权限不该把整张表拖垮
        # 子网标上可用区：它必须和规格选的那个可用区一致，不标的话没法挑
        out["subnet"] = [("%s · %s（%s）" % (v.get("Zone"), v.get("SubnetName"),
                                           v.get("CidrBlock", "")), v.get("SubnetId"))
                         for v in (self._call("DescribeSubnets", {"Limit": "50"},
                                              service="vpc").get("SubnetSet") or [])]
        return out

    # ---- 自定义镜像 ----------------------------------------------------

    def list_images(self):
        return [{"id": im.get("ImageId"), "name": im.get("ImageName") or "",
                 "size": im.get("ImageSize") or "",
                 "status": im.get("ImageState") or "", "region": self.region}
                for im in (self._call("DescribeImages", {
                    "Filters": [{"Name": "image-type", "Values": ["PRIVATE_IMAGE"]}],
                    "Limit": 100}).get("ImageSet") or [])]

    def save_image(self, iid, name):
        # ForcePoweroff=TRUE：跑着的机器先软关机再做。不关的话缓存没落盘，
        # 镜像里的模型文件可能是半截的。
        r = self._call("CreateImage", {"InstanceId": iid, "ImageName": name,
                                       "ForcePoweroff": "TRUE"})
        return (r or {}).get("ImageId")

    def copy_image(self, image_id, regions, log):
        # SyncImages 一次一个镜像、可以给多个目标地域；异步，做完 ImageState
        # 从 SYNCING 变 NORMAL
        self._call("SyncImages", {"ImageIds": [image_id],
                                  "DestinationRegions": list(regions)})
        log("→ 已发起同步到：%s" % "、".join(regions))
        return [(r, "同步中") for r in regions]

    # ---- 建机 ---------------------------------------------------------

    def validate(self, v):
        need = [("spec", "可用区 · 规格"), ("image", "镜像"),
                ("sg", "安全组"), ("subnet", "子网")]
        empty = [label for key, label in need if not v.get(key)]
        if empty:
            raise CloudError("%s 没填。先点「拉规格 / 镜像」——腾讯云这几样都得从"
                             "账号里现有的挑" % "、".join(empty))
        if "|" not in (v.get("spec") or ""):
            raise CloudError("规格要从下拉里挑（格式是 可用区|规格名）")
        self.check_zone(v["spec"].split("|", 1)[0], self.region)
        # 腾讯云 Linux 密码要 8-30 位、字母数字符号里占两类
        check_password(v.get("password") or "", 2)
        self.check_disk(v.get("image"), v.get("disk") or 0)
        src = (v.get("cos_src") or "").strip()
        if src:
            bucket = self.split_cos(src)[0] if src.startswith("cos://") else ""
            if not src.startswith("cos://") or not bucket:
                raise CloudError("「开机拉模型」要填 cos://桶名-APPID/前缀 这种写法，"
                                 "现在是「%s」" % src)
            if "-" not in bucket:
                raise CloudError("COS 的桶名要带 APPID，形如 changji-models-1250000000"
                                 "（控制台桶列表里就是这个全名）——现在是「%s」" % bucket)
            if not (v.get("cam_role") or "").strip():
                raise CloudError("要开机拉模型就得给一个 CAM 角色——"
                                 "不然实例没有读 COS 的凭证。\n"
                                 "（走角色是为了别把密钥塞进机器）")
            if not (v.get("bandwidth") or 0):
                raise CloudError("带宽填了 0 就没有公网 IP，装不了 coscli，"
                                 "开机脚本会卡在第一步。拉模型的话带宽别填 0")
        if v.get("strategy") == "limit":
            try:
                float(v.get("price") or "")
            except ValueError:
                raise CloudError("选了「设价格上限」就要填价格上限，单位是元/小时")

    def create(self, v):
        self.validate(v)
        zone, itype = v["spec"].split("|", 1)
        bw = v["bandwidth"]
        market = {"MarketType": "spot", "SpotOptions": {"SpotInstanceType": "one-time"}}
        if v["strategy"] == "limit":
            market["SpotOptions"]["MaxPrice"] = str(v["price"])
        script = self.boot_script(v)
        p = {
            "Placement": {"Zone": zone},
            "ImageId": v["image"],
            "InstanceType": itype,
            "InstanceChargeType": "SPOTPAID",
            "InstanceMarketOptions": market,
            "SystemDisk": {"DiskType": v["disk_type"], "DiskSize": v["disk"]},
            "VirtualPrivateCloud": {"VpcId": "", "SubnetId": v["subnet"]},
            "InternetAccessible": {
                "InternetChargeType": "TRAFFIC_POSTPAID_BY_HOUR",
                "InternetMaxBandwidthOut": bw,
                "PublicIpAssigned": bw > 0},
            "SecurityGroupIds": [v["sg"]],
            "LoginSettings": {"Password": v["password"]},
            "InstanceName": v.get("name") or "changji",
            "InstanceCount": 1,
        }
        if script:
            p["UserData"] = self.encode_userdata(script)
            p["CamRoleName"] = v["cam_role"]
        ids = self._call("RunInstances", p).get("InstanceIdSet") or []
        return ids[0] if ids else None

    def create_summary(self, v):
        zone, itype = (v.get("spec") or "|").split("|", 1)
        price = ("上限 %s 元/时" % v.get("price")) if v.get("strategy") == "limit" \
            else "随市场价"
        return ("地域 %s · 可用区 %s\n规格 %s\n镜像 %s\n系统盘 %s GB %s\n"
                "公网带宽上限 %s Mbps\n出价 竞价（%s）\n%s\n"
                "竞价实例会被回收。确定？"
                % (self.region, zone, itype, v.get("image"), v.get("disk"),
                   v.get("disk_type"), v.get("bandwidth"), price,
                   ("开机自动从 %s 拉模型到 %s（走内网，不计流量费）\n"
                    % (v.get("cos_src"), v.get("cos_dest") or "/root/models"))
                   if (v.get("cos_src") or "").strip() else ""))

    # ---- 开机自动从 COS 拉模型 ------------------------------------------
    #
    # 跟阿里云那半截同一个意思，工具链不同：COS + coscli + CAM 角色。
    # 同样**绝不把长期密钥写进 UserData**——临时凭证从实例元数据现拿。
    #
    # 跟阿里云的两处真差别：
    #   · coscli 不会自己去读元数据（ossutil 有 EcsRamRole 模式），所以脚本
    #     得自己拿凭证、自己写 ~/.cos.yaml；
    #   · 临时凭证最短 30 分钟就过期。几十 GB 走内网几分钟就完，够用；真超时
    #     了 coscli 会报鉴权失败，重跑一次脚本就行（脚本里写了怎么重跑）。

    BOOT_SCRIPT = """#!/bin/bash
# changji 开机拉模型（这个界面生成的，走内网不计流量费）
set -u
LOG=/var/log/changji-models.log
exec >>"$LOG" 2>&1
echo "=== $(date -Is) 开始 ==="
DEST=__DEST__
mkdir -p "$DEST"
COSCLI=/usr/local/bin/coscli
if [ ! -x "$COSCLI" ]; then
  echo "装 coscli……"
  curl -fsSL -o "$COSCLI" https://cosbrowser.cloud.tencent.com/software/coscli/coscli-linux-amd64 \\
    && chmod +x "$COSCLI" \\
    || { echo "装不上 coscli：这台机器出不了公网？带宽是不是填了 0"; exit 1; }
fi
# 临时凭证从实例元数据现拿（CAM 角色 __ROLE__），机器上不存长期密钥
python3 - > /root/.cos.yaml <<'CREDEOF' || { echo "拿不到临时凭证：这台机器绑 CAM 角色了吗"; exit 1; }
import json, urllib.request
u = "http://metadata.tencentyun.com/latest/meta-data/cam/security-credentials/__ROLE__"
d = json.load(urllib.request.urlopen(u, timeout=10))
print("cos:")
print("  base:")
print("    secretid: " + d["TmpSecretId"])
print("    secretkey: " + d["TmpSecretKey"])
print("    sessiontoken: " + d["Token"])
print("    protocol: https")
print("  buckets:")
print("  - name: __BUCKET__")
print("    alias: models")
print("    region: __REGION__")
print("    endpoint: __ENDPOINT__")
print("    ofs: false")
CREDEOF
chmod 600 /root/.cos.yaml   # 里面是临时凭证，别给别人看
"$COSCLI" sync cos://models/__PREFIX__ "$DEST" -r -c /root/.cos.yaml \\
  || { echo "同步失败。手工看一眼：$COSCLI ls cos://models/ -c /root/.cos.yaml"; exit 1; }
du -sh "$DEST"
# 拉完了留个记号，ssh 上去 ls 一下就知道能不能开工
touch /root/.changji-models-ready
echo "=== $(date -Is) 拉完了 ==="
"""

    @staticmethod
    def split_cos(src):
        """cos://桶名-APPID/前缀 → (桶名, 前缀)。"""
        rest = src[len("cos://"):].strip("/")
        bucket, _, prefix = rest.partition("/")
        return bucket, (prefix + "/" if prefix else "")

    def boot_script(self, v):
        src = (v.get("cos_src") or "").strip()
        if not src:
            return ""
        # 地域直接从表单取，不读 self.region：预览那条路不经过 set_context
        region = (v.get("region") or self.region or "ap-guangzhou")
        bucket, prefix = self.split_cos(src)
        out = self.BOOT_SCRIPT
        for k, val in (("__DEST__", (v.get("cos_dest") or "/root/models").strip()),
                       ("__ROLE__", (v.get("cam_role") or "").strip()),
                       ("__BUCKET__", bucket),
                       ("__PREFIX__", prefix),
                       ("__REGION__", region),
                       # 内网域名：cos-internal.<地域>.myqcloud.com。
                       # 用公网那个的话几十 GB 全按公网流量收费，而且慢得多。
                       ("__ENDPOINT__", "cos-internal.%s.myqcloud.com" % region)):
            out = out.replace(k, val)
        return out

    def list_roles(self):
        """账号里的 CAM 角色。自己建的排前面（同阿里云那条的理由）。"""
        rows = (self._call("DescribeRoleList", {"Page": 1, "Rp": 200},
                           service="cam").get("List") or [])
        # 腾讯云的服务角色**不是统一前缀**，是名字里带 QcsRole / QCSLinkedRole
        # （真账号实测：CSIP_QCSLinkedRoleInGlobalScene、TIONE_QcsRole，而自己
        # 建的叫 test）。按前缀排会把两种混在一起，所以按这个子串认。
        def service_role(r):
            n = str(r.get("RoleName") or "").lower()
            return "qcsrole" in n or "qcslinkedrole" in n
        rows.sort(key=lambda r: (service_role(r), str(r.get("RoleName") or "")))
        return [("%s（%s）" % (r.get("RoleName"), (r.get("Description") or "")[:24]),
                 r.get("RoleName")) for r in rows]

    # ---- 列表 / 状态 / 详情 --------------------------------------------

    @staticmethod
    def _local_time(iso):
        """CreatedTime 形如 2020-09-22T00:00:00Z / +00:00，两种都认。"""
        if not iso:
            return ""
        s = iso.replace("+00:00", "Z")
        try:
            t = time.strptime(s, "%Y-%m-%dT%H:%M:%SZ")
        except ValueError:
            return iso
        return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(calendar.timegm(t)))

    def _raw(self, iid=None):
        p = {"Limit": 100}
        if iid:
            p["InstanceIds"] = [iid]
        return self._call("DescribeInstances", p).get("InstanceSet") or []

    def instances(self):
        out = []
        for it in self._raw():
            charge = it.get("InstanceChargeType") or ""
            out.append({
                "id": it.get("InstanceId", ""),
                "name": it.get("InstanceName", ""),
                "status": it.get("InstanceState", ""),
                "spec": it.get("InstanceType", ""),
                "gpu": (it.get("GPUInfo") or {}).get("GPUCount", "") or "",
                "billing": "竞价" if charge == "SPOTPAID" else charge,
                "region": (it.get("Placement") or {}).get("Zone", ""),
                "created": self._local_time(it.get("CreatedTime")),
                "traffic": None,
            })
        return out

    def status(self, iid):
        rows = self._raw(iid)
        return rows[0].get("InstanceState", "") if rows else ""

    def detail(self, iid):
        rows = self._raw(iid)
        if not rows:
            return ["这台在 %s 里找不到（换个地域看看？）" % self.region], ""
        it = rows[0]
        ips = it.get("PublicIpAddresses") or []
        ip = ips[0] if ips else ""
        lines = []
        if ip:
            ssh = "ssh root@%s" % ip
            lines.append(ssh)
            lines.append("密码是你建机时填的那个（这个界面不存密码）")
        else:
            ssh = ""
            lines.append("没有公网 IP——建机时带宽填成 0 了，或者还没分配好")
        g = it.get("GPUInfo") or {}
        if g.get("GPUType"):
            lines.append("显卡 %s ×%s" % (g["GPUType"], g.get("GPUCount")))
        lines.append("%s · %s核 %sG · %s"
                     % (it.get("InstanceType"), it.get("CPU"), it.get("Memory"),
                        it.get("InstanceState")))
        return lines, ssh

    # ---- 开关机 / 释放 --------------------------------------------------

    def power_on(self, iid, command=""):
        self._call("StartInstances", {"InstanceIds": [iid]})

    def power_off(self, iid):
        self._call("StopInstances", {"InstanceIds": [iid]})

    def release(self, iid):
        self._call("TerminateInstances", {"InstanceIds": [iid]})

    def confirm_release_text(self, iid, name):
        return ("要销毁 %s（%s）吗？\n\n"
                "销毁 = 实例和系统盘一起没，这一步不可逆。\n"
                "开着也能销毁；销毁不掉的话会先替你关机再来一次。"
                % (iid, name or "无名"))

    # 腾讯云没要求销毁前先关机，所以先直接销毁——常见情况一步就成。
    # 销毁不动才退回「关机 → 等停稳 → 再销毁」。
    def release_safely(self, iid, log, wait_s=180):
        try:
            self.release(iid)
        except CloudError as e:
            log("直接销毁没成：%s" % e)
            log("那就先关机再销毁……")
            self.power_off(iid)
            st = self._wait_off(iid, log, wait_s)
            if not is_off(st):
                raise CloudError("等了 %d 秒还停在「%s」，没敢再销毁。去控制台看一眼"
                                 % (wait_s, st))
            self.release(iid)
        log("已销毁：%s" % iid)

    # ---- 比价 ----------------------------------------------------------
    #
    # 比阿里云省事：一个地域一次调用就把「有哪些带卡规格、在哪个可用区、
    # 什么价、有没有货」全给了。按量价另问一次，用来算省了多少。

    def price_table(self, log, regions=None):
        if not regions:
            regions = [r.get("Region") for r in
                       (self._call("DescribeRegions").get("RegionSet") or [])
                       if r.get("Region") and r.get("RegionState") in (None, "AVAILABLE")]
        log("扫 %d 个地域（含海外）：%s" % (len(regions), "、".join(regions)))

        def one(region):
            spot = self._gpu_quota(region, "SPOTPAID")
            pay = {}
            try:
                for q in self._gpu_quota(region, "POSTPAID_BY_HOUR"):
                    pay[(q.get("Zone"), q.get("InstanceType"))] = q
            except CloudError:
                pass          # 按量价拿不到不影响竞价价，那一栏空着就是
            return region, spot, pay

        rows, broken = [], []
        with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
            futs = {pool.submit(one, r): r for r in regions}
            for fut in concurrent.futures.as_completed(futs):
                try:
                    region, spot, pay = fut.result()
                except CloudError as e:
                    broken.append("%s（%s）" % (futs[fut], e))
                    continue
                if spot:
                    log("%s：%d 种带卡的" % (region, len(spot)))
                for q in spot:
                    card, vram, count = self._gpu_of(q)
                    # ⚠️ **竞价的真实价是 UnitPriceDiscount，不是 UnitPrice。**
                    # 实测同一个规格：竞价 UnitPrice=11.88（跟按量原价一模一样）、
                    # UnitPriceDiscount=2.376（正好两折）。拿 UnitPrice 当竞价价
                    # 会把每一条都报高五倍，而且高得很像真的——按量价也是那个数。
                    unit = self._real_price(q)
                    if unit is None:
                        continue
                    ref = pay.get((q.get("Zone"), q.get("InstanceType"))) or {}
                    ref_price = self._real_price(ref)
                    rows.append({
                        "region": region, "zone": q.get("Zone"),
                        "spec": q.get("InstanceType"),
                        "gpu": card, "n": count if count is not None else "",
                        "vram": vram,
                        "cpu": q.get("Cpu") or "",
                        "mem": q.get("Memory") or "",
                        "spot": float(unit),
                        "origin": float(ref_price) if ref_price is not None else None,
                        # 竞价的货是一阵一阵的：上海 31 种带卡规格里当时只有
                        # 1 种有货。全丢掉的话这张表几乎是空的，看不出行情；
                        # 所以都列出来、标清楚，默认只看有货。
                        "stock": q.get("Status") == "SELL",
                        "note": ("有货" if q.get("Status") == "SELL"
                                 else "无货（%s）" % (q.get("StatusCategory") or "SOLD_OUT")),
                        "pick": {"region": region,
                                 "spec": "%s|%s" % (q.get("Zone"), q.get("InstanceType"))},
                    })
        if broken:
            log("这几个地域没问出来，跳过了：%s" % "；".join(broken))
        if not rows:
            raise CloudError("这些地域现在都没有带卡的竞价规格有货")
        self._fill_latency(rows, log)
        rows.sort(key=lambda r: r["spot"])
        log("拿到 %d 条报价，最便宜 %.3f 元/时（%s %s）"
            % (len(rows), rows[0]["spot"], rows[0]["region"], rows[0]["spec"]))
        return rows


PLATFORMS = [AutoDLPlatform, PPIOPlatform, AliyunPlatform, TencentPlatform]


# ---- 钥匙存盘 ----------------------------------------------------------

def as_creds(v):
    """配置里一个平台存的可能是一段字符串（老格式），也可能是一个字典。"""
    if isinstance(v, dict):
        return v
    if isinstance(v, str) and v:
        return {"token": v}
    return {}


def load_config():
    try:
        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
            cfg = json.load(f)
    except Exception:
        cfg = {}
    # 只有 AutoDL 那会儿存的是 {"token": ...}，搬过来，别让人重填一遍。
    if not cfg:
        try:
            with open(LEGACY_CONFIG, "r", encoding="utf-8") as f:
                old = json.load(f).get("token")
            if old:
                cfg = {"autodl": old}
        except Exception:
            pass
    return cfg if isinstance(cfg, dict) else {}


def save_config(cfg):
    d = os.path.dirname(CONFIG_PATH)
    if not os.path.isdir(d):
        os.makedirs(d, 0o700)
    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
        json.dump(cfg, f)
    # 钥匙就是账号的全部权限，别留成 644。
    os.chmod(CONFIG_PATH, stat.S_IRUSR | stat.S_IWUSR)


# 表单上那十几栏，关掉窗口就全丢——第二次开机又得从头填一遍。存起来。
# **密码不存**：那是实例的登录密码，存进配置文件等于明文落盘，而它每次
# 现填只有一栏的代价。
RECIPE_SKIP = ("password",)


def load_recipe(name):
    r = (load_config().get("recipes") or {}).get(name)
    return r if isinstance(r, dict) else {}


def save_recipe(name, values):
    cfg = load_config()
    recipes = cfg.get("recipes")
    if not isinstance(recipes, dict):
        recipes = {}
    recipes[name] = {k: v for k, v in (values or {}).items()
                     if k not in RECIPE_SKIP and not isinstance(v, (dict, list))}
    cfg["recipes"] = recipes
    save_config(cfg)


def load_prefs():
    p = load_config().get("prefs")
    return p if isinstance(p, dict) else {}


def save_prefs(**kw):
    cfg = load_config()
    prefs = cfg.get("prefs")
    if not isinstance(prefs, dict):
        prefs = {}
    prefs.update(kw)     # 合并不是覆盖：这儿存着好几样不同时候写的门槛
    cfg["prefs"] = prefs
    save_config(cfg)


def creds_for(cls, cfg):
    return as_creds(cfg.get(cls.CONFIG_KEY))


# ---- 后台活：网络一律不在界面线程上跑，否则点一下窗口就转菊花 ------------

class Signals(QtCore.QObject):
    done = QtCore.Signal(object)
    failed = QtCore.Signal(str)
    logged = QtCore.Signal(str)
    finished = QtCore.Signal()


class Job(QtCore.QRunnable):
    """fn(log) 在线程池里跑，结果用信号发回界面线程。

    Qt 的跨线程信号是排队投递的，所以槽函数天然跑在界面线程上——不用自己
    再搭一套队列。
    """

    def __init__(self, fn):
        super().__init__()
        self.fn = fn
        self.sig = Signals()
        # **别让 Qt 自动删。** QRunnable 默认 autoDelete：run() 一返回 C++ 那头
        # 就把对象删了，而 Python 这边还要在 finally 里发 finished 信号，于是
        # 报 "Signal source has been deleted"，那一次的结果整个丢掉（面板空着，
        # 而且不报错）。生命周期交给 Window.jobs 那个列表管。
        self.setAutoDelete(False)

    def run(self):
        try:
            r = self.fn(self.sig.logged.emit)
        except CloudError as e:
            self.sig.failed.emit(str(e))
        except Exception as e:                      # 网络库偶尔抛别的
            self.sig.failed.emit("意外错误：%r" % (e,))
        else:
            self.sig.done.emit(r)
        finally:
            self.sig.finished.emit()


# ---- 表单：按平台声明的 Field 列表搭一张表 ------------------------------

def _show_head(combo):
    """可编辑下拉里把光标挪回开头。

    不挪的话 Qt 把长文本滚到末尾，显示出来的是
    「· 16C128G · ecs.gn8is.4xlarge」——**开头那段可用区和卡型号被切掉了，
    而那才是要看的**。
    """
    le = combo.lineEdit()
    if le is not None:
        le.setCursorPosition(0)
    combo.setToolTip(combo.currentText())


class Form(QtCore.QObject):
    def __init__(self, parent, fields):
        super().__init__(parent)
        self.fields = {f.key: f for f in fields}
        self.order = [f.key for f in fields]
        self.widgets = {}
        lay = QtWidgets.QFormLayout(parent)
        lay.setLabelAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        lay.setFieldGrowthPolicy(QtWidgets.QFormLayout.AllNonFixedFieldsGrow)
        # 不让长内容把标签挤到上一行——下拉已经限了宽，这里就不用换行策略了
        lay.setRowWrapPolicy(QtWidgets.QFormLayout.DontWrapRows)
        for f in fields:
            w = self._make(f)
            self.widgets[f.key] = w
            lay.addRow(f.label, w)

    def _make(self, f):
        if f.kind == "checks":
            box = QtWidgets.QWidget()
            h = QtWidgets.QHBoxLayout(box)
            h.setContentsMargins(0, 0, 0, 0)
            box.boxes = []
            for text, val in f.choices:
                cb = QtWidgets.QCheckBox(text)
                cb.value = val
                h.addWidget(cb)
                box.boxes.append(cb)
            h.addStretch(1)
            return box
        if f.kind in ("combo", "pick"):
            cb = QtWidgets.QComboBox()
            cb.setEditable(f.kind == "pick")     # pick 允许直接粘 ID / docker 地址
            if f.kind == "pick":
                cb.setInsertPolicy(QtWidgets.QComboBox.NoInsert)
            # 选项文字很长（「cn-wulanchabu-a · NVIDIA L20 ×1 · 16C128G · ecs…」），
            # 不限住的话它会把整个左栏撑宽，标签被挤到上一行去。
            cb.setSizeAdjustPolicy(
                QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon)
            cb.setMinimumContentsLength(12)
            for text, val in f.choices:
                cb.addItem(text, val)
                cb.setItemData(cb.count() - 1, text, QtCore.Qt.ToolTipRole)
            if f.default:
                k = cb.findText(f.default)
                if k >= 0:
                    cb.setCurrentIndex(k)
            _show_head(cb)
            return cb
        if f.kind == "secret":
            le = QtWidgets.QLineEdit()
            le.setEchoMode(QtWidgets.QLineEdit.Password)
            le.setPlaceholderText(f.hint)
            return le
        if f.kind == "spin":
            sp = QtWidgets.QSpinBox()
            sp.setRange(f.lo, f.hi)
            sp.setSingleStep(f.step or 1)
            sp.setValue(int(f.default or f.lo))
            return sp
        le = QtWidgets.QLineEdit(f.default)
        le.setPlaceholderText(f.hint)
        return le

    def set_choices(self, key, choices):
        """拉回来的规格 / 镜像填进下拉。原来选中的还在就留着。"""
        f = self.fields.get(key)
        w = self.widgets.get(key)
        if f is None or not isinstance(w, QtWidgets.QComboBox):
            return
        keep = self.value(key)
        f.choices = choices
        # 重填要挡住信号：不挡的话这一下会触发 currentIndexChanged，而「地域」
        # 那一栏的 change 正连着「重拉」，一拉又一填，转成死循环。
        w.blockSignals(True)
        try:
            w.clear()
            for text, val in choices:
                w.addItem(text, val)
            k = w.findData(keep)
            if k >= 0:
                w.setCurrentIndex(k)
        finally:
            w.blockSignals(False)
        _show_head(w)

    def value(self, key):
        f = self.fields[key]
        w = self.widgets[key]
        if f.kind == "checks":
            return [cb.value for cb in w.boxes if cb.isChecked()]
        if f.kind == "spin":
            return w.value()
        if f.kind in ("combo", "pick"):
            if f.kind == "combo":
                return w.currentData()
            # 可编辑下拉：挑了一项就用它背后的 ID；手打的就原样交出去。
            k = w.findText(w.currentText())
            return w.itemData(k) if k >= 0 else w.currentText().strip()
        return w.text().strip()

    def set_value(self, key, value):
        """把比价窗口里挑中的那一条填回表单。

        下拉里还没有这一项时要**先插进去**：比价扫的是所有地域，而规格下拉
        里装的只是当前地域那一批，不插的话填不进去，看着像没反应。
        """
        f = self.fields.get(key)
        w = self.widgets.get(key)
        if f is None or w is None:
            return
        if isinstance(w, QtWidgets.QComboBox):
            k = w.findData(value)
            if k < 0:
                w.addItem(str(value), value)
                k = w.count() - 1
            w.setCurrentIndex(k)
            _show_head(w)
        elif isinstance(w, QtWidgets.QSpinBox):
            w.setValue(int(value))
        elif isinstance(w, QtWidgets.QLineEdit):
            w.setText(str(value))

    def on_change(self, keys, slot):
        """这些字段一改就回调（阿里云换地域 → 规格/镜像/安全组/交换机全得重拉）。"""
        for k in keys:
            w = self.widgets.get(k)
            if isinstance(w, QtWidgets.QComboBox):
                w.currentIndexChanged.connect(slot)
            elif isinstance(w, QtWidgets.QLineEdit):
                w.editingFinished.connect(slot)

    def values(self):
        return {k: self.value(k) for k in self.order}


# ---- 窗口 --------------------------------------------------------------

# 国内地域怎么认：阿里云是 cn-xxx，腾讯云是 ap-guangzhou / ap-shanghai 这种
# （而 ap-singapore、ap-tokyo 是海外）。所以按地域名认，不按前缀。
DOMESTIC = ("cn-", "ap-guangzhou", "ap-shanghai", "ap-beijing", "ap-chengdu",
            "ap-chongqing", "ap-nanjing", "ap-shenzhen", "ap-hongkong",
            "ap-jinan", "ap-hangzhou", "ap-fuzhou", "ap-wuhan", "ap-changsha",
            "ap-taipei", "ap-shijiazhuang", "ap-qingyuan", "ap-xian", "ap-shenyang")


def is_domestic(region):
    r = str(region or "")
    return any(r.startswith(x) for x in DOMESTIC)


def as_num(v):
    """能当数看就返回数，不能就返回 None。

    别用 str(v).isdigit()：那个对小数返回 False，于是 vGPU 折算出来的
    「4.0 G 显存」会掉进字符串那条路，按字面排。
    """
    if isinstance(v, bool) or v is None or v == "":
        return None
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


class NumItem(QtWidgets.QTableWidgetItem):
    """按数值排、按格式显示的单元格。

    只用 setData(EditRole, 数值) 的话排序是对的，但**显示也会跟着用那个数值**，
    于是一列里 2.751 / 3.12 / 9.9 长短不一，扫一眼比不出来；只用 setText 的话
    显示齐了，排序却变成按字面比——"10.500" 会排在 "9.900" 前面。所以两件事
    分开：文字归文字，比大小走 __lt__。
    """

    def __init__(self, value, text):
        super().__init__(text)
        self.value = value

    def __lt__(self, other):
        if isinstance(other, NumItem):
            return self.value < other.value
        return super().__lt__(other)


# 「显存」这一栏是挑机器时最该先看的那个数（出首帧要 ≥30 G，见
# setup_gpu_box.md），而且不列它的话最便宜的几条永远是 2G 的 vGPU 切片，
# 看着诱人其实一点用没有。「内存」是系统内存，两回事。
PRICE_COLS = [("plat", "平台"), ("region", "地域"), ("zone", "可用区"), ("spec", "规格"),
              ("gpu", "显卡"), ("vram", "显存G"), ("n", "卡"),
              ("cpu", "vCPU"), ("mem", "内存G"), ("lat", "延迟ms"),
              ("spot", "抢占价"), ("origin", "按量价"), ("save", "省"),
              ("note", "备注")]


class PriceDialog(QtWidgets.QDialog):
    """可用规格和价格，默认按抢占价从便宜到贵排。

    **表头点一下就能换着排**，所以数字列不能塞字符串——塞字符串的话 "10.5"
    会排在 "9.9" 前面（按字典序比的），那张表就白列了。数字走 EditRole。
    """

    def __init__(self, parent, rows, title):
        super().__init__(parent)
        self.setWindowTitle(title)
        self.resize(1080, 620)
        self.picked = None
        lay = QtWidgets.QVBoxLayout(self)

        text = ("按抢占价从便宜到贵排。点表头换别的排法；"
                "双击一行就把它填回左边的建机表单。")
        why = next((r.get("lat_why") for r in rows if r.get("lat_why")), None)
        if why and all(r.get("lat") is None for r in rows):
            # 延迟量不出来就说清为什么，别留一栏破折号让人猜
            text += "\n延迟这一栏是空的：%s。" % why
        tip = QtWidgets.QLabel(text)
        tip.setStyleSheet("color: gray;")
        tip.setWordWrap(True)
        lay.addWidget(tip)

        # 筛选放在这儿而不是拉数据之前：拉一次要几十秒，而调门槛应该是即时的。
        bar = QtWidgets.QHBoxLayout()
        bar.addWidget(QtWidgets.QLabel("显存 ≥"))
        self.min_vram = QtWidgets.QSpinBox()
        self.min_vram.setRange(0, 200)
        self.min_vram.setSuffix(" G")
        self.min_vram.setSingleStep(8)
        bar.addWidget(self.min_vram)
        bar.addSpacing(14)
        bar.addWidget(QtWidgets.QLabel("卡数 ≥"))
        self.min_cards = QtWidgets.QSpinBox()
        self.min_cards.setRange(0, 16)
        bar.addWidget(self.min_cards)
        bar.addSpacing(14)
        # 「国内」各家写法不同：阿里云是 cn-，腾讯云是 ap-guangzhou 这种，
        # 所以按地域名里的关键字认，不按前缀。
        self.only_cn = QtWidgets.QCheckBox("只看国内")
        bar.addWidget(self.only_cn)
        # 竞价的货一阵一阵的，没货的也列着好看行情，但默认只看买得到的
        self.only_stock = QtWidgets.QCheckBox("只看有货")
        self.only_stock.setChecked(True)
        # 用一个显式的标记，**不要拿 isVisible() 当判据**：窗口还没 show 的
        # 时候所有子控件都报不可见，筛选就会整个失效。
        self.has_stock = any("stock" in r for r in rows)
        self.only_stock.setVisible(self.has_stock)
        bar.addWidget(self.only_stock)
        bar.addStretch(1)
        self.count_label = QtWidgets.QLabel("")
        bar.addWidget(self.count_label)
        lay.addLayout(bar)

        self.table = QtWidgets.QTableWidget(len(rows), len(PRICE_COLS))
        self.table.setHorizontalHeaderLabels([c[1] for c in PRICE_COLS])
        self.table.verticalHeader().setVisible(False)
        self.table.setSelectionBehavior(QtWidgets.QTableWidget.SelectRows)
        self.table.setSelectionMode(QtWidgets.QTableWidget.SingleSelection)
        self.table.setEditTriggers(QtWidgets.QTableWidget.NoEditTriggers)
        self.rows = rows
        for r, row in enumerate(rows):
            spot, origin = row.get("spot"), row.get("origin")
            save, dear = "", False
            if spot is not None and origin:
                pct = round((1 - spot / origin) * 100)
                # 抢占价**可能比按量价还贵**（需求一上来市场价就顶上去了）。
                # 写成「-1%」太容易滑过去，直接说「贵 1%」并且标红。
                save = ("省 %d%%" % pct) if pct >= 0 else ("贵 %d%%" % -pct)
                dear = pct < 0
            for c, (key, _label) in enumerate(PRICE_COLS):
                if key == "save":
                    v = save
                elif key in ("spot", "origin"):
                    v = row.get(key)
                else:
                    v = row.get(key, "")
                num = as_num(v) if key in ("n", "cpu", "mem", "vram", "lat") else None
                if key in ("spot", "origin") and v is not None:
                    item = NumItem(float(v), "%.3f" % float(v))
                elif num is not None:
                    # 整数就不显示小数点（24 而不是 24.0），小数保留一位
                    item = NumItem(num, ("%g" % num))
                elif key == "lat" and v is None:
                    item = QtWidgets.QTableWidgetItem("—")   # 量不了就说量不了
                else:
                    item = QtWidgets.QTableWidgetItem("" if v is None else str(v))
                if key == "spot":
                    item.setForeground(QtGui.QBrush(QtGui.QColor("#047857")))
                if key == "save" and dear:
                    item.setForeground(QtGui.QBrush(QtGui.QColor("#b91c1c")))
                item.setData(QtCore.Qt.UserRole, r)   # 排序之后还找得回原始那行
                self.table.setItem(r, c, item)
        self.table.horizontalHeader().setSectionResizeMode(
            QtWidgets.QHeaderView.ResizeToContents)
        self.table.setSortingEnabled(True)
        self.table.sortItems([c[0] for c in PRICE_COLS].index("spot"),
                             QtCore.Qt.AscendingOrder)
        self.table.selectRow(0)          # 开窗就选中最便宜那条，回车即可用
        self.table.doubleClicked.connect(self._pick)
        lay.addWidget(self.table)
        for w in (self.min_vram, self.min_cards):
            w.valueChanged.connect(self._filter)
        self.only_cn.toggled.connect(self._filter)
        self.only_stock.toggled.connect(self._filter)
        self._filter()

        btns = QtWidgets.QHBoxLayout()
        btns.addStretch(1)
        b = QtWidgets.QPushButton("填回表单")
        b.clicked.connect(self._pick)
        btns.addWidget(b)
        b = QtWidgets.QPushButton("关闭")
        b.clicked.connect(self.reject)
        btns.addWidget(b)
        lay.addLayout(btns)

    def _filter(self):
        """按门槛显示/隐藏行。**隐藏不是删除**——门槛调回去还要看得见，
        而且排序和「填回表单」都靠原始行号，删了就对不上了。"""
        vram = self.min_vram.value()
        cards = self.min_cards.value()
        cn_only = self.only_cn.isChecked()
        stock_only = self.has_stock and self.only_stock.isChecked()
        shown = 0
        for r in range(self.table.rowCount()):
            row = self.rows[self.table.item(r, 0).data(QtCore.Qt.UserRole)]
            ok = ((row.get("vram") or 0) >= vram
                  and (row.get("n") or 0) >= cards
                  # 不报库存的平台（没有 stock 这一项）当成有货，别整张表筛没
                  and (not stock_only or row.get("stock", True))
                  and (not cn_only or is_domestic(row.get("region", ""))))
            self.table.setRowHidden(r, not ok)
            shown += ok
        self.count_label.setText("%d / %d 条" % (shown, self.table.rowCount()))
        # 选中的那行被筛掉了就换一个还看得见的，免得「填回表单」填的是看不见的行
        cur = self.table.currentRow()
        if cur < 0 or self.table.isRowHidden(cur):
            for r in range(self.table.rowCount()):
                if not self.table.isRowHidden(r):
                    self.table.selectRow(r)
                    break

    def _pick(self):
        r = self.table.currentRow()
        if r < 0:
            return
        # 排过序之后行号跟原始顺序对不上了，拿当初存在 UserRole 里的下标找回来
        idx = self.table.item(r, 0).data(QtCore.Qt.UserRole)
        self.picked = self.rows[idx]
        self.accept()


class MetricsDialog(QtWidgets.QDialog):
    """一台机器的实时情况。开着就自动刷，关掉就停。

    **GPU 这一栏四家接口都不给。** AutoDL 的 snapshot 只有 CPU/内存/磁盘；
    阿里云和腾讯云的 GPU 指标要机器上装云监控插件，普通镜像没装。与其留一栏
    空着让人以为是坏了，不如把**能真跑通的那条路**摆出来：这个窗口已经知道
    ssh 怎么连，就把 `ssh … nvidia-smi` 拼好放这儿，一点就复制。
    """

    def __init__(self, parent, plat, iid, name, region=""):
        super().__init__(parent)
        self.setWindowTitle("%s · %s" % (name or iid, plat.NAME))
        self.resize(520, 480)
        self.plat, self.iid, self.region = plat, iid, region
        self.ssh = ""
        lay = QtWidgets.QVBoxLayout(self)

        head = QtWidgets.QLabel("%s　%s" % (iid, plat.NAME))
        head.setStyleSheet("font-weight: 600;")
        lay.addWidget(head)

        self.grid = QtWidgets.QFormLayout()
        self.grid.setLabelAlignment(QtCore.Qt.AlignRight | QtCore.Qt.AlignVCenter)
        box = QtWidgets.QWidget()
        box.setLayout(self.grid)
        lay.addWidget(box)

        self.notes = QtWidgets.QLabel("")
        self.notes.setWordWrap(True)
        self.notes.setStyleSheet("color: gray; font-size: 12px;")
        lay.addWidget(self.notes)
        lay.addStretch(1)

        self.gpu_line = QtWidgets.QLineEdit()
        self.gpu_line.setReadOnly(True)
        self.gpu_line.setPlaceholderText("拿到 ssh 之后这儿会给一条看 GPU 的命令")
        gl = QtWidgets.QHBoxLayout()
        gl.addWidget(QtWidgets.QLabel("看 GPU"))
        gl.addWidget(self.gpu_line, 1)
        self.gpu_copy = QtWidgets.QPushButton("复制")
        self.gpu_copy.clicked.connect(
            lambda: QtGui.QGuiApplication.clipboard().setText(self.gpu_line.text()))
        gl.addWidget(self.gpu_copy)
        lay.addLayout(gl)

        bot = QtWidgets.QHBoxLayout()
        self.stamp = QtWidgets.QLabel("")
        self.stamp.setStyleSheet("color: gray;")
        bot.addWidget(self.stamp)
        bot.addStretch(1)
        self.auto = QtWidgets.QCheckBox("每 5 秒刷新")
        self.auto.setChecked(True)
        bot.addWidget(self.auto)
        b = QtWidgets.QPushButton("刷新")
        b.clicked.connect(self.pull)
        bot.addWidget(b)
        b = QtWidgets.QPushButton("关闭")
        b.clicked.connect(self.reject)
        bot.addWidget(b)
        lay.addLayout(bot)

        self.bars = {}
        # 判「窗口还在不在」要用自己的旗子，**不能用 isVisible()**：
        # 构造函数里就发了第一次请求，那会儿窗口还没 show，isVisible() 是
        # False，回包一来就被当成「已经关了」丢掉——面板永远是空的。
        self._alive = True
        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(5000)
        self.timer.timeout.connect(self.pull)
        self.auto.toggled.connect(
            lambda on: self.timer.start() if on else self.timer.stop())
        self.timer.start()
        self.pull()

    # 刷新交给主窗口的后台机制跑：网络不能在界面线程上做，
    # 而这个窗口每 5 秒就要来一次。
    def pull(self):
        w = self.parent()
        if w is None:
            return
        plat, iid = self.plat, self.iid

        def done(res):
            if not self._alive:
                return          # 窗口关了就别往上画了
            self.render(res)
        region = self.region
        w.run_bg(lambda _log: self._gather(plat, iid, region), done, guard=False)

    @staticmethod
    def _gather(plat, iid, region):
        # **在后台线程里再设一次地域。** run_bg 发车前会把表单里选的地域推给
        # 平台，而这台机器未必在那个地域；这一句在它之后跑，说了算的是这台
        # 机器自己的地域。
        if region:
            plat.set_context({"region": region})
        out = plat.metrics(iid)
        try:
            lines, ssh = plat.detail(iid)
            out["ssh"] = ssh
        except CloudError:
            out["ssh"] = ""     # 详情拿不到不该让整个面板空着
        return out

    def render(self, res):
        rows = res.get("rows") or []
        # 行数和名字在同一台机器上基本不变，所以第一次建好就只更新值，
        # 免得每 5 秒把整块拆了重搭（会闪，而且滚动位置会跳）
        if set(self.bars) != set(n for n, _, _ in rows):
            while self.grid.rowCount():
                self.grid.removeRow(0)
            self.bars = {}
            for name, _v, pct in rows:
                holder = QtWidgets.QWidget()
                h = QtWidgets.QHBoxLayout(holder)
                h.setContentsMargins(0, 0, 0, 0)
                val = QtWidgets.QLabel("")
                h.addWidget(val)
                bar = QtWidgets.QProgressBar()
                bar.setRange(0, 100)
                bar.setTextVisible(False)
                bar.setFixedHeight(10)
                bar.setVisible(pct is not None)
                h.addWidget(bar, 1)
                self.grid.addRow(name, holder)
                self.bars[name] = (val, bar)
        for name, v, pct in rows:
            val, bar = self.bars[name]
            val.setText(str(v))
            bar.setVisible(pct is not None)
            if pct is not None:
                bar.setValue(int(max(0, min(100, pct))))
        # 这些说明是各平台运行时拼的字符串，静态扫不到，而 QLabel 不认
        # markdown——**写了星号就真的显示两个星号**。这个错前后犯了四次，
        # 所以在这儿再剥一层，不指望每个平台都记得。
        self.notes.setText("\n".join("· " + n.replace("**", "")
                                      for n in (res.get("notes") or [])))
        ssh = res.get("ssh") or ""
        self.ssh = ssh
        # ssh 那条本来就带 -p 和用户名，后面挂个命令就是一条能直接粘的
        self.gpu_line.setText(
            (ssh + " nvidia-smi") if ssh else "")
        self.gpu_copy.setEnabled(bool(ssh))
        self.stamp.setText("更新于 " + time.strftime("%H:%M:%S"))

    def closeEvent(self, e):
        self._alive = False
        self.timer.stop()
        super().closeEvent(e)

    def reject(self):
        self._alive = False
        self.timer.stop()
        super().reject()


class ImageDialog(QtWidgets.QDialog):
    """自定义镜像。

    **这个窗口是整套东西的省钱关键。** 模型权重四五十 GB，公网 100 Mbps 下
    要一个多小时才下得完，而抢占式随时会被回收——每次开机现下载，等于每次
    先烧一小时的钱和运气。正路是：起一台机 → 下好模型 → 在这儿存成镜像 →
    以后每台新机直接从这个镜像开，零下载。

    抢占式最便宜的地域会漂（今天乌兰察布、明天深圳），所以还得能把镜像
    复制到别的地域，不然只能被钉在一个地方。
    """

    def __init__(self, parent, rows, plat, selected_instance):
        super().__init__(parent)
        self.setWindowTitle("%s · 自定义镜像" % plat.NAME)
        self.resize(820, 420)
        self.action = None            # ("save", 名字) / ("copy", 镜像ID, [地域])
        self.rows = rows
        lay = QtWidgets.QVBoxLayout(self)

        tip = QtWidgets.QLabel(
            "把下好模型的机器存成镜像，以后新机直接从镜像开，不用再下载。\n"
            "镜像是「按地域」的：抢占式最便宜的地域会漂，常用的几个地域各复制一份。")
        tip.setStyleSheet("color: gray;")
        tip.setWordWrap(True)
        lay.addWidget(tip)

        cols = [("id", "镜像ID"), ("name", "名称"), ("size", "大小GB"),
                ("status", "状态"), ("region", "地域")]
        self.table = QtWidgets.QTableWidget(len(rows), len(cols))
        self.table.setHorizontalHeaderLabels([c[1] for c in cols])
        self.table.verticalHeader().setVisible(False)
        self.table.setSelectionBehavior(QtWidgets.QTableWidget.SelectRows)
        self.table.setSelectionMode(QtWidgets.QTableWidget.SingleSelection)
        self.table.setEditTriggers(QtWidgets.QTableWidget.NoEditTriggers)
        for r, row in enumerate(rows):
            for c, (key, _label) in enumerate(cols):
                self.table.setItem(r, c, QtWidgets.QTableWidgetItem(
                    str(row.get(key, "") if row.get(key) is not None else "")))
        self.table.horizontalHeader().setSectionResizeMode(
            QtWidgets.QHeaderView.ResizeToContents)
        if rows:
            self.table.selectRow(0)
        lay.addWidget(self.table)
        if not rows:
            empty = QtWidgets.QLabel("还没有自定义镜像。先起一台机、把模型下好，"
                                     "再回来点「从选中的机器做镜像」。")
            empty.setStyleSheet("color: gray;")
            lay.addWidget(empty)

        btns = QtWidgets.QHBoxLayout()
        self.save_btn = QtWidgets.QPushButton("从选中的机器做镜像")
        self.save_btn.clicked.connect(self._save)
        # 没选机器就做不了——说清楚为什么按不动，别让人对着灰按钮猜
        self.save_btn.setEnabled(bool(selected_instance))
        self.save_btn.setToolTip("" if selected_instance
                                 else "先在主窗口的列表里点一台机器")
        btns.addWidget(self.save_btn)
        self.copy_btn = QtWidgets.QPushButton("复制到其他地域")
        self.copy_btn.clicked.connect(self._copy)
        self.copy_btn.setEnabled(bool(rows) and plat.CAN_COPY_IMAGE)
        btns.addWidget(self.copy_btn)
        btns.addStretch(1)
        close = QtWidgets.QPushButton("关闭")
        close.clicked.connect(self.reject)
        btns.addWidget(close)
        lay.addLayout(btns)

    def _save(self):
        name, ok = QtWidgets.QInputDialog.getText(
            self, "做镜像", "镜像名（建议带上日期和里面装了什么）：",
            text="changji-models-" + time.strftime("%Y%m%d"))
        if not ok or not name.strip():
            return
        self.action = ("save", name.strip())
        self.accept()

    def _copy(self):
        r = self.table.currentRow()
        if r < 0:
            return
        image_id = self.rows[r].get("id")
        text, ok = QtWidgets.QInputDialog.getText(
            self, "复制镜像", "复制到哪些地域？逗号分隔：\n"
            "（照着比价表里便宜的那几个填，比如 cn-wulanchabu,cn-shenzhen）")
        regions = [x.strip() for x in (text or "").replace("，", ",").split(",") if x.strip()]
        if not ok or not regions:
            return
        self.action = ("copy", image_id, regions)
        self.accept()


COLUMNS = [("id", "实例ID"), ("name", "名称"), ("status", "状态"), ("spec", "规格"),
           ("gpu", "卡"), ("billing", "计费"), ("traffic", "出网24h"),
           ("region", "地区"), ("created", "创建时间")]


def fmt_traffic(gb):
    """按流量计费花钱的是出网。小的用 MB 说，大的用 GB 说，没有就说没有。"""
    if gb is None:
        return "—"
    if gb < 1:
        return "%d MB" % round(gb * 1000)
    return "%.2f GB" % gb


class Window(QtWidgets.QWidget):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("租卡 · AutoDL / PPIO")
        # 默认尺寸按**放得进 1280×800 那一档笔记本**定：算上菜单栏和程序坞，
        # 能用的高度常常只有七百出头。再大是用户自己拉的事。
        self.resize(1240, 780)

        cfg = load_config()
        self.platforms = [cls(creds_for(cls, cfg)) for cls in PLATFORMS]
        self.plat = self.platforms[0]
        self.form = None
        self.page_of = {}
        self.cred_page_of = {}
        self.switching = False
        self.rows = {}
        self.busy = 0
        # 每换一次平台 +1。切过去的请求还在路上、用户又切回来时，回包带着旧号，
        # 整条作废。只比平台名不够：A→B→A 的时候名字又相等了，旧回包会混进来。
        self.seq = 0
        self.pool = QtCore.QThreadPool(self)
        self.jobs = []          # QRunnable 交给池子之后自己不留引用，信号对象会被回收

        self._build()
        self.switch_platform(self.platforms[0].NAME)

    # --- 搭界面 -----------------------------------------------------

    def _build(self):
        root = QtWidgets.QVBoxLayout(self)

        bar = QtWidgets.QHBoxLayout()
        bar.addWidget(QtWidgets.QLabel("平台"))
        self.tabs = {}
        group = QtWidgets.QButtonGroup(self)
        for p in self.platforms:
            b = QtWidgets.QRadioButton(p.NAME)
            b.toggled.connect(
                lambda on, n=p.NAME: on and self.switch_platform(n))
            group.addButton(b)
            bar.addWidget(b)
            self.tabs[p.NAME] = b
        bar.addSpacing(16)
        # 钥匙几段由平台自己说了算：AutoDL / PPIO 一段，阿里云是
        # AccessKey ID + Secret 两段。跟表单一样一个平台一页，不拆不重搭。
        self.cred_stack = QtWidgets.QStackedWidget()
        self.cred_edits = {}
        for i, p in enumerate(self.platforms):
            page = QtWidgets.QWidget()
            h = QtWidgets.QHBoxLayout(page)
            h.setContentsMargins(0, 0, 0, 0)
            edits = {}
            for key, label, _envs in p.CREDS:
                h.addWidget(QtWidgets.QLabel(label))
                e = QtWidgets.QLineEdit()
                e.setEchoMode(QtWidgets.QLineEdit.Password)
                e.setPlaceholderText("粘进来再点保存")
                e.setMinimumWidth(260 if len(p.CREDS) == 1 else 190)
                h.addWidget(e)
                edits[key] = e
            self.cred_edits[p.NAME] = edits
            self.cred_stack.addWidget(page)
            self.cred_page_of[p.NAME] = i
        bar.addWidget(self.cred_stack)
        save = QtWidgets.QPushButton("保存")
        save.clicked.connect(self.on_save_token)
        bar.addWidget(save)
        bar.addStretch(1)
        self.busy_label = QtWidgets.QLabel("")
        bar.addWidget(self.busy_label)
        root.addLayout(bar)

        split = QtWidgets.QHBoxLayout()

        self.create_box = QtWidgets.QGroupBox("新建实例")
        left = QtWidgets.QVBoxLayout(self.create_box)
        # 一个平台一页，建一次就不动了。换平台 = 换页，不拆不重搭：
        # 拆了重搭时旧表单在真正被删掉之前还占着位置，几层标签会叠在一起。
        # 顺带一个好处：两边各自填了一半的东西都留着。
        self.stack = QtWidgets.QStackedWidget()
        self.forms = {}
        for i, p in enumerate(self.platforms):
            page = QtWidgets.QWidget()
            self.forms[p.NAME] = Form(page, p.fields())
            # 上次填的套回去。下拉里还没有的项（比如规格）由 set_value 先插进去，
            # 等「拉规格 / 镜像」回来时 set_choices 会认出同一个值并保住它。
            for key, val in load_recipe(p.NAME).items():
                if key in self.forms[p.NAME].fields:
                    self.forms[p.NAME].set_value(key, val)
            self.forms[p.NAME].on_change(p.REFRESH_ON, self.on_fetch)
            self.stack.addWidget(page)
            self.page_of[p.NAME] = i
        left.addWidget(self.stack)
        # 提示语默认收起来。**它有六七行**，常驻的话光这一块就一百三十多像素，
        # 而它是「看一次就记住」的东西，不是每次都要读的。
        self.hint_btn = QtWidgets.QToolButton()
        self.hint_btn.setText("这家的脾气（点开看）")
        self.hint_btn.setCheckable(True)
        self.hint_btn.setStyleSheet("border: none; color: gray;")
        self.hint_btn.setToolButtonStyle(QtCore.Qt.ToolButtonTextBesideIcon)
        self.hint_btn.setArrowType(QtCore.Qt.RightArrow)
        left.addWidget(self.hint_btn)
        self.hint = QtWidgets.QLabel("")
        self.hint.setWordWrap(True)
        self.hint.setStyleSheet("color: gray; font-size: 12px;")
        self.hint.setVisible(False)
        self.hint_btn.toggled.connect(self.hint.setVisible)
        self.hint_btn.toggled.connect(
            lambda on: self.hint_btn.setArrowType(
                QtCore.Qt.DownArrow if on else QtCore.Qt.RightArrow))
        left.addWidget(self.hint)
        left.addStretch(1)

        # **左栏放进滚动区。** 阿里云和腾讯云各有 15 个字段，加上四个按钮，
        # 光这一栏的最小高度就 810——Qt 会无视 resize() 把整个窗口撑到那么高，
        # 小屏幕上直接看不见下半截（实测最小高度 1103）。进了滚动区之后它
        # 不再顶着窗口，窗口矮了就在栏内滚。
        left_scroll = QtWidgets.QScrollArea()
        left_scroll.setWidget(self.create_box)
        left_scroll.setWidgetResizable(True)
        left_scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        left_scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)

        # **按钮不进滚动区。** 进去的话窗口一矮，「一键最便宜」这种最常按的
        # 反而要先滚一下才够得着——最常用的东西不该藏在滚动条后面。
        # 表单滚，按钮钉在底下。
        left_col = QtWidgets.QWidget()
        left_col.setFixedWidth(392)         # 370 的内容 + 滚动条的地方
        lcv = QtWidgets.QVBoxLayout(left_col)
        lcv.setContentsMargins(0, 0, 0, 0)
        lcv.addWidget(left_scroll, 1)

        btns = QtWidgets.QHBoxLayout()
        b = QtWidgets.QPushButton("创建")
        b.clicked.connect(self.on_create)
        btns.addWidget(b)
        b = QtWidgets.QPushButton("拉规格 / 镜像")
        b.clicked.connect(self.on_fetch)
        btns.addWidget(b)
        lcv.addLayout(btns)
        self.cheap_btn = QtWidgets.QPushButton("一键最便宜（扫所有平台）")
        self.cheap_btn.clicked.connect(self.on_cheapest)
        lcv.addWidget(self.cheap_btn)
        row2 = QtWidgets.QHBoxLayout()
        self.price_btn = QtWidgets.QPushButton("比价")
        self.price_btn.clicked.connect(self.on_prices)
        row2.addWidget(self.price_btn)
        self.image_btn = QtWidgets.QPushButton("自定义镜像…")
        self.image_btn.clicked.connect(self.on_images)
        row2.addWidget(self.image_btn)
        self.boot_btn = QtWidgets.QPushButton("开机脚本")
        self.boot_btn.clicked.connect(self.on_boot_script)
        row2.addWidget(self.boot_btn)
        lcv.addLayout(row2)
        split.addWidget(left_col)

        right = QtWidgets.QVBoxLayout()
        top = QtWidgets.QHBoxLayout()
        for text, slot in (("刷新", self.refresh), ("开机", self.on_power_on),
                           ("关机", self.on_power_off), ("详情 / SSH", self.on_detail)):
            b = QtWidgets.QPushButton(text)
            b.clicked.connect(slot)
            top.addWidget(b)
        self.metrics_btn = QtWidgets.QPushButton("实时")
        self.metrics_btn.clicked.connect(self.on_metrics)
        top.addWidget(self.metrics_btn)
        b = QtWidgets.QPushButton("释放")
        b.setStyleSheet("color: #b91c1c;")
        b.clicked.connect(self.on_release)
        top.addWidget(b)
        top.addStretch(1)
        self.auto = QtWidgets.QCheckBox("自动刷新 15s")
        self.auto.toggled.connect(self.on_auto)
        top.addWidget(self.auto)
        right.addLayout(top)

        self.table = QtWidgets.QTableWidget(0, len(COLUMNS))
        self.table.setHorizontalHeaderLabels([c[1] for c in COLUMNS])
        self.table.verticalHeader().setVisible(False)
        self.table.setSelectionBehavior(QtWidgets.QTableWidget.SelectRows)
        self.table.setSelectionMode(QtWidgets.QTableWidget.SingleSelection)
        self.table.setEditTriggers(QtWidgets.QTableWidget.NoEditTriggers)
        # 双击 = 看这台机器现在怎么样。用户要的就是「点一下快速看一眼」。
        self.table.doubleClicked.connect(self.on_metrics)
        head = self.table.horizontalHeader()
        # 列宽写死的话，八栏加起来比窗口宽，最后那栏「创建时间」会被横向滚动
        # 条藏掉——而那一栏正好是判断「这台开了多久」用的。让它按内容排。
        head.setSectionResizeMode(QtWidgets.QHeaderView.ResizeToContents)
        head.setSectionResizeMode(1, QtWidgets.QHeaderView.Stretch)   # 名称吃掉富余
        right.addWidget(self.table)
        split.addLayout(right)

        # 上半截（表单 + 实例列表）和下半截（日志）用 splitter 分，**能拖**。
        # 原来两个都直接塞进主布局、日志还写死 190 像素——想多看几行日志没
        # 办法，想让窗口矮一点也没办法。
        top = QtWidgets.QWidget()
        top.setLayout(split)
        self.vsplit = QtWidgets.QSplitter(QtCore.Qt.Vertical)
        self.vsplit.addWidget(top)
        self.vsplit.setChildrenCollapsible(False)
        root.addWidget(self.vsplit, 1)

        box = QtWidgets.QGroupBox("日志")
        lv = QtWidgets.QVBoxLayout(box)
        self.logbox = QtWidgets.QPlainTextEdit()
        self.logbox.setReadOnly(True)
        self.logbox.setMaximumBlockCount(500)   # 自动刷新开一天能攒出几万行
        self.logbox.setFont(QtGui.QFontDatabase.systemFont(
            QtGui.QFontDatabase.FixedFont))
        # **不写死高度。** 原来是 setFixedHeight(190)，那 190 像素不管窗口
        # 多矮都要占着，是窗口下不去的原因之一。给个小的下限，剩下交给
        # splitter——想多看几行日志就往上拖。
        self.logbox.setMinimumHeight(64)
        lv.addWidget(self.logbox)
        self.vsplit.addWidget(box)
        # 富余全给上半截，日志保持它那一点高度
        self.vsplit.setStretchFactor(0, 1)
        self.vsplit.setStretchFactor(1, 0)
        self.vsplit.setSizes([600, 170])

        self.timer = QtCore.QTimer(self)
        self.timer.setInterval(15000)
        self.timer.timeout.connect(self.refresh)

    def log(self, msg):
        self.logbox.appendPlainText("%s  %s" % (time.strftime("%H:%M:%S"), msg))

    # --- 后台 -------------------------------------------------------

    def set_busy(self, delta):
        self.busy = max(0, self.busy + delta)
        self.busy_label.setText("忙…" if self.busy else "")
        if self.busy:
            QtWidgets.QApplication.setOverrideCursor(QtCore.Qt.WaitCursor)
        else:
            QtWidgets.QApplication.restoreOverrideCursor()

    def run_bg(self, fn, done=None, guard=True):
        """guard=True 的活，回包带的号跟当前对不上就丢掉（用户换平台了）。"""
        seq = self.seq
        name = self.plat.NAME
        if self.form is not None:
            # 阿里云的一切都按地域，而地域在表单上。别的家不理这个调用。
            self.plat.set_context(self.form.values())
        self.set_busy(+1)
        job = Job(fn)
        self.jobs.append(job)
        job.sig.logged.connect(lambda m: self.log("  " + m))
        job.sig.failed.connect(lambda m: self.log("✗ [%s] %s" % (name, m)))

        def on_done(r):
            if guard and seq != self.seq:
                return
            if done:
                done(r)

        def on_finished():
            self.set_busy(-1)
            if job in self.jobs:
                self.jobs.remove(job)

        job.sig.done.connect(on_done)
        job.sig.finished.connect(on_finished)
        self.pool.start(job)

    # --- 换平台 -----------------------------------------------------

    def switch_platform(self, name):
        # setChecked 自己会再触发一次 toggled，不挡住的话整套动作跑两遍，
        # 网络请求也发两份（日志里每条都重一遍就是这么来的）。
        if self.switching:
            return
        self.switching = True
        try:
            self._switch(name)
        finally:
            self.switching = False

    def _switch(self, name):
        if self.form is not None:
            # 离开前把输入框里那几段钥匙记回去，免得填了没保存就换走丢掉。
            self._harvest_creds()
        self.seq += 1
        self.plat = next(p for p in self.platforms if p.NAME == name)
        self.tabs[name].setChecked(True)

        self.cred_stack.setCurrentIndex(self.cred_page_of[name])
        for key, e in self.cred_edits[name].items():
            e.setText(self.plat.creds.get(key, ""))
        self.create_box.setTitle("新建实例 · %s" % name)
        self.hint.setText(self.plat.BILLING_HINT)
        self.table.setColumnHidden([c[0] for c in COLUMNS].index("traffic"),
                                   not self.plat.TRAFFIC)
        self.metrics_btn.setEnabled(self.plat.METRICS)
        self.metrics_btn.setToolTip("" if self.plat.METRICS
                                    else "%s 的接口不报实时数据" % name)
        self.image_btn.setEnabled(self.plat.CAN_IMAGE)
        self.image_btn.setToolTip("" if self.plat.CAN_IMAGE
                                  else "%s 不支持自定义镜像" % name)
        # 这两个是**跨平台**的，不随当前平台变灰——扫的是所有填了钥匙的家
        can = bool(self.scannable())
        for b in (self.price_btn, self.cheap_btn):
            b.setEnabled(can)
            b.setToolTip("" if can else "先给至少一家能比价的平台填上钥匙")

        self.stack.setCurrentIndex(self.page_of[name])
        self.form = self.forms[name]

        self.table.setRowCount(0)
        self.rows.clear()
        self.log("── %s ──" % name)
        if not self.plat.has_creds():
            self.log("先填钥匙：%s" % self.plat.TOKEN_HINT)
            return
        self.refresh()
        # PPIO 的规格必须现拉才有得选，换过去就顺手拉一次，别让用户先撞一次空。
        # AutoDL 的镜像有写死的公共表，不空，所以这儿不会白跑一趟。
        if [k for k in self.plat.DYNAMIC if not self.form.fields[k].choices]:
            self.on_fetch()

    def _harvest_creds(self):
        for key, e in self.cred_edits[self.plat.NAME].items():
            self.plat.creds[key] = e.text().strip()

    def on_save_token(self):
        self._harvest_creds()
        cfg = load_config()
        cfg[self.plat.CONFIG_KEY] = dict(self.plat.creds)
        try:
            save_config(cfg)
        except Exception as e:
            self.log("存钥匙失败：%s" % e)
            return
        self.log("%s 的钥匙已存到 %s" % (self.plat.NAME, CONFIG_PATH))
        self.switch_platform(self.plat.NAME)

    # --- 动作 -------------------------------------------------------

    def refresh(self):
        plat = self.plat

        def done(rows):
            self.table.setRowCount(len(rows))
            self.rows.clear()
            for r, it in enumerate(rows):
                self.rows[it["id"]] = it
                for c, col in enumerate(COLUMNS):
                    if col[0] == "traffic":
                        item = QtWidgets.QTableWidgetItem(fmt_traffic(it.get("traffic")))
                    else:
                        item = QtWidgets.QTableWidgetItem(str(it.get(col[0], "")))
                    if col[0] == "status" and it.get("status") in OFF_STATES:
                        item.setForeground(QtGui.QBrush(QtGui.QColor("#9aa3ad")))
                    self.table.setItem(r, c, item)
            self.log("%s：%d 台" % (plat.NAME, len(rows)))
        self.run_bg(lambda _log: plat.instances(), done)

    def on_auto(self, on):
        self.timer.start() if on else self.timer.stop()

    def selected(self):
        r = self.table.currentRow()
        if r < 0 or not self.table.item(r, 0):
            self.log("先在列表里点一台")
            return None
        return self.table.item(r, 0).text()

    def on_fetch(self):
        plat = self.plat
        if not plat.DYNAMIC:
            return

        def done(choices):
            for key, rows in (choices or {}).items():
                self.form.set_choices(key, rows)
                self.log("%s：%s %d 项" % (plat.NAME, key, len(rows)))
        self.run_bg(lambda _log: plat.load_choices(), done)

    def scannable(self):
        """能比价、而且钥匙填齐了的平台。"""
        return [p for p in self.platforms if p.PRICES and p.has_creds()]

    def scan_prices(self, log):
        """**所有平台一起扫。** 只扫当前这家等于闭着一只眼挑最便宜的——
        L20 在阿里云 2.379，同档在腾讯云可能翻倍，不一起看就选不出来。
        一家挂了不影响别家。"""
        rows, broken = [], []
        for plat in self.scannable():
            log("—— %s ——" % plat.NAME)
            try:
                got = plat.price_table(log)
            except CloudError as e:
                broken.append("%s（%s）" % (plat.NAME, e))
                continue
            for r in got:
                r["plat"] = plat.NAME
            rows.extend(got)
        for b in broken:
            log("这家没问出来，跳过：%s" % b)
        rows.sort(key=lambda r: r["spot"])
        return rows

    def apply_pick(self, row):
        """把挑中的那条填回表单。跨平台的话先切过去。"""
        name = row.get("plat")
        if name and name != self.plat.NAME:
            self.switch_platform(name)
        for key, val in (row.get("pick") or {}).items():
            self.form.set_value(key, val)
        self.log("挑了 %s %s %s · %.3f 元/时，已填回表单"
                 % (row.get("plat") or "", row.get("region"), row.get("spec"),
                    row.get("spot")))

    def on_prices(self):
        if not self.scannable():
            self.log("没有一家既能比价又填了钥匙")
            return
        self.log("比价中……（要按地域一个个问，慢一点）")

        def done(rows):
            if not rows:
                self.log("一条报价都没拿到")
                return
            d = PriceDialog(self, rows, "可用规格和价格（%s）"
                            % "、".join(p.NAME for p in self.scannable()))
            if d.exec() == QtWidgets.QDialog.Accepted and d.picked:
                self.apply_pick(d.picked)
        self.run_bg(self.scan_prices, done, guard=False)

    # ---- 一键最便宜 ----------------------------------------------------
    #
    # 这一个按钮把原来十几步收成两步：扫所有平台 → 按门槛挑最便宜有货的 →
    # 其余十几栏从上次的配方里来 → 只剩一个确认框。

    def on_cheapest(self):
        if not self.scannable():
            self.log("没有一家既能比价又填了钥匙")
            return
        prefs = load_prefs()
        vram, okd = QtWidgets.QInputDialog.getInt(
            self, "一键最便宜", "显存至少多少 G？（出首帧那档要 ≥30）",
            int(prefs.get("min_vram") or 32), 0, 200, 8)
        if not okd:
            return
        cards, okd = QtWidgets.QInputDialog.getInt(
            self, "一键最便宜", "至少几张卡？", int(prefs.get("min_cards") or 1), 1, 16)
        if not okd:
            return
        save_prefs(min_vram=vram, min_cards=cards)
        self.log("找显存 ≥%dG、≥%d 卡、现在买得到的最便宜那台……" % (vram, cards))

        def done(rows):
            fit = [r for r in rows
                   if (r.get("vram") or 0) >= vram
                   and (r.get("n") or 0) >= cards
                   and r.get("stock", True)]
            if not fit:
                self.log("这个门槛下现在一台都没有。把显存或卡数往下调调，"
                         "或者点「比价」自己看看行情")
                return
            best = fit[0]           # scan_prices 已经按价排过
            self.apply_pick(best)
            self.do_create(self.plat, prefix=(
                "这是扫出来最便宜的一台（显存 ≥%dG、≥%d 卡、有货）：\n"
                "%s %s %s · %.3f 元/时\n\n其余几栏来自上次的配方。\n\n"
                % (vram, cards, best.get("plat") or "", best.get("region"),
                   best.get("spec"), best.get("spot"))))
        self.run_bg(self.scan_prices, done, guard=False)

    def on_boot_script(self):
        """把要塞进 UserData 的脚本原样摆出来。

        这段东西会在你的机器上**以 root 跑**，不该是个黑盒——尤其它还要去
        公网装 ossutil。摆出来能看、能复制，自己 ssh 上去手动跑一遍也行。
        """
        try:
            values = self.form.values()
            self.plat.set_context(values)      # 保险：让平台也知道当前地域
            script = self.plat.boot_script(values)
        except Exception as e:
            self.log("生成不了开机脚本：%r" % (e,))
            return
        if not script:
            self.log("这次不会装开机脚本"
                     + ("（「开机拉模型」那栏是空的）" if self.plat.CAN_IMAGE else ""))
            return
        d = QtWidgets.QDialog(self)
        d.setWindowTitle("开机脚本（会塞进 UserData，以 root 执行）")
        d.resize(820, 560)
        lay = QtWidgets.QVBoxLayout(d)
        tip = QtWidgets.QLabel(
            "建机时这段会被 base64 塞进 UserData，实例第一次开机以 root 执行。\n"
            "日志在机器的 /var/log/changji-models.log；拉完会留下 "
            "/root/.changji-models-ready。")
        tip.setStyleSheet("color: gray;")
        tip.setWordWrap(True)
        lay.addWidget(tip)
        box = QtWidgets.QPlainTextEdit(script)
        box.setReadOnly(True)
        box.setFont(QtGui.QFontDatabase.systemFont(QtGui.QFontDatabase.FixedFont))
        lay.addWidget(box)
        row = QtWidgets.QHBoxLayout()
        row.addStretch(1)
        b = QtWidgets.QPushButton("复制")
        b.clicked.connect(lambda: QtGui.QGuiApplication.clipboard().setText(script))
        row.addWidget(b)
        c = QtWidgets.QPushButton("关闭")
        c.clicked.connect(d.reject)
        row.addWidget(c)
        lay.addLayout(row)
        d.exec()

    def on_images(self):
        plat = self.plat
        if not plat.CAN_IMAGE:
            self.log("%s 不支持自定义镜像" % plat.NAME)
            return
        iid = self.table.item(self.table.currentRow(), 0).text() \
            if self.table.currentRow() >= 0 else ""

        def done(rows):
            d = ImageDialog(self, rows, plat, iid)
            if d.exec() != QtWidgets.QDialog.Accepted or not d.action:
                return
            if d.action[0] == "save":
                name = d.action[1]
                if QtWidgets.QMessageBox.question(
                        self, "做镜像",
                        "把 %s 存成镜像「%s」？\n\n"
                        "这一步是异步的，几十 GB 的盘要做一阵子。\n"
                        "做的过程中机器会被关机（不关的话缓存没落盘，"
                        "镜像里的模型可能是半截的）。" % (iid, name)) \
                        != QtWidgets.QMessageBox.Yes:
                    return
                self.log("做镜像 %s ← %s ……" % (name, iid))
                self.run_bg(lambda log: plat.save_image(iid, name),
                            lambda img: (self.log("✓ 镜像 %s 已发起，做完之后点"
                                                  "「拉规格 / 镜像」就能在镜像下拉里选到" % img),
                                         self.refresh()))
            else:
                _, image_id, regions = d.action
                self.log("复制镜像 %s → %s ……" % (image_id, "、".join(regions)))
                self.run_bg(
                    lambda log: plat.copy_image(image_id, regions, log),
                    lambda _r: self.log("✓ 已发起复制（异步，做完才能在那些地域用）"))
        self.run_bg(lambda log: plat.list_images(), done)

    def on_create(self):
        self.do_create(self.plat, prefix="")

    def do_create(self, plat, prefix=""):
        """校验 → 存配方 → 确认 → 建。「一键最便宜」也走这一条，不另开一套。"""
        try:
            v = self.form.values()
            plat.validate(v)            # 确认框弹出来之前就该拦住
            summary = plat.create_summary(v)
        except CloudError as e:
            self.log("✗ %s" % e)
            return
        # **校验过了才存**：存一份填错的配方，下次开窗口就是错的
        try:
            save_recipe(plat.NAME, v)
        except Exception as e:
            self.log("配方没存下来：%s" % e)
        if QtWidgets.QMessageBox.question(
                self, "确认创建（%s）" % plat.NAME, prefix + summary) != \
                QtWidgets.QMessageBox.Yes:
            return

        def done(iid):
            self.log("✓ 创建成功：%s" % iid)
            self.refresh()
        self.run_bg(lambda _log: plat.create(v), done)

    def on_power_on(self):
        iid = self.selected()
        if not iid:
            return
        plat = self.plat
        cmd = self.form.value("command") if "command" in self.form.fields else ""
        self.log("开机 %s ……" % iid)
        self.run_bg(lambda _log: plat.power_on(iid, cmd),
                    lambda _r: (self.log("✓ 已发开机"), self.refresh()))

    def on_power_off(self):
        iid = self.selected()
        if not iid:
            return
        plat = self.plat
        self.log("关机 %s ……" % iid)
        self.run_bg(lambda _log: plat.power_off(iid),
                    lambda _r: (self.log("✓ 已发关机"), self.refresh()))

    def on_metrics(self):
        iid = self.selected()
        if not iid:
            return
        if not self.plat.METRICS:
            self.log("%s 的接口不报实时数据" % self.plat.NAME)
            return
        it = self.rows.get(iid, {})
        d = MetricsDialog(self, self.plat, iid, it.get("name"),
                          self.plat.region_of(it))
        d.exec()

    def on_detail(self):
        iid = self.selected()
        if not iid:
            return
        plat = self.plat

        def done(result):
            lines, ssh = result
            self.log("—— %s ——" % iid)
            for ln in lines:
                self.log("  %s" % ln)
            if ssh:
                # SSH 那行进剪贴板，下一步就是 install_workers.sh。
                QtGui.QGuiApplication.clipboard().setText(ssh)
                self.log("  （SSH 命令已复制）")
        self.run_bg(lambda _log: plat.detail(iid), done)

    def on_release(self):
        iid = self.selected()
        if not iid:
            return
        plat = self.plat
        it = self.rows.get(iid, {})
        if QtWidgets.QMessageBox.warning(
                self, "释放实例（%s）" % plat.NAME,
                plat.confirm_release_text(iid, it.get("name")),
                QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No,
                QtWidgets.QMessageBox.No) != QtWidgets.QMessageBox.Yes:
            return
        self.log("释放 %s ……" % iid)
        # 关机要等，整串都在后台线程上跑，过程日志靠信号一行一行发回来。
        self.run_bg(lambda log: plat.release_safely(iid, log),
                    lambda _r: self.refresh())


def selftest():
    """不开窗口跑一遍，确认这个包是活的。

    **打包出来的东西必须能自检。** 光看 CI 上「编出来了」证明不了任何事——
    PySide6 冻进单文件之后少一个 Qt 插件、少一个动态库，都是双击才发现，
    而那时候人已经下下去了。这一遍在 offscreen 下把四家的表单真搭一次。
    """
    os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
    import tempfile
    tmp = tempfile.mkdtemp()
    global CONFIG_PATH, LEGACY_CONFIG
    # 自检不许碰使用者真正的配置：这台机器上可能存着真钥匙
    CONFIG_PATH = os.path.join(tmp, "cloud.json")
    LEGACY_CONFIG = os.path.join(tmp, "legacy.json")
    app = QtWidgets.QApplication([])
    w = Window()
    bad = []
    for plat in w.platforms:
        w.switch_platform(plat.NAME)
        QtWidgets.QApplication.processEvents()
        if not w.form.order:
            bad.append("%s 的表单是空的" % plat.NAME)
        for f in plat.fields():
            if "**" in (f.label + f.hint):
                bad.append("%s 的 %s 上有裸星号" % (plat.NAME, f.key))
        if "**" in (plat.BILLING_HINT + plat.TOKEN_HINT):
            bad.append("%s 的提示语里有裸星号" % plat.NAME)
    names = [p.NAME for p in w.platforms]
    if len(names) != len(set(names)):
        bad.append("平台重名：%s" % names)
    w.close()
    del app
    if bad:
        sys.stderr.write("自检没过：\n  " + "\n  ".join(bad) + "\n")
        return 1
    print("自检通过 · %s · 平台：%s" % (VERSION, "、".join(names)))
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if "--version" in argv:
        print(VERSION)
        return 0
    if "--selftest" in argv:
        return selftest()
    if argv:
        sys.stderr.write("只认 --version 和 --selftest，别的都不带参数。\n")
        return 2
    app = QtWidgets.QApplication(sys.argv)
    app.setApplicationName("租卡")
    w = Window()
    w.show()
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
