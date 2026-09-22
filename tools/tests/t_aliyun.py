# -*- coding: utf-8 -*-
import json, sys, time
import os
# tests/ 的上一层就是 tools/，被测的模块在那儿
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import autodl_gui as G
A = G.AliyunPlatform
ok=[]
def check(n): ok.append(n); print("  ✓", n)

# ========== 1. 签名：拿官方文档里那个带标准答案的例子对 ==========
GOLD = {"AccessKeyId":"testid", "Action":"DescribeRegions", "Format":"XML",
        "SignatureMethod":"HMAC-SHA1",
        "SignatureNonce":"3ee8c1b8-83d3-44af-a94f-4e0ad82fd6cf",
        "SignatureVersion":"1.0", "Timestamp":"2019-08-23T12:46:24Z",
        "Version":"2019-09-10"}
WANT_STS = ("GET&%2F&AccessKeyId%3Dtestid%26Action%3DDescribeRegions%26Format%3DXML"
            "%26SignatureMethod%3DHMAC-SHA1%26SignatureNonce%3D3ee8c1b8-83d3-44af-a94f"
            "-4e0ad82fd6cf%26SignatureVersion%3D1.0%26Timestamp%3D2019-08-23T12%253A46"
            "%253A24Z%26Version%3D2019-09-10")
sts = A._string_to_sign("GET", GOLD)
assert sts == WANT_STS, "\n实得 " + sts + "\n应为 " + WANT_STS
check("StringToSign 跟官方例子逐字节一致（冒号要二次编码成 %253A）")

# HMAC 那一步没法拿文档对——**文档里那个签名常量是过期的**（它印的
# OLeaidS1JvxuMvnyHOwuJ+uX5qY= 跟它自己印的 StringToSign 对不上，
# 例子参数改过、签名串没跟着改）。所以改成跟一个完全独立的实现对：openssl。
import subprocess
ref = subprocess.run(["openssl", "dgst", "-sha1", "-hmac", "testsecret&", "-binary"],
                     input=sts.encode(), capture_output=True).stdout
import base64, hmac, hashlib
ref_b64 = base64.b64encode(ref).decode()
mine = A._sign("GET", GOLD, "testsecret")
assert mine == ref_b64, (mine, ref_b64)
check("HMAC 那一步跟 openssl 逐字节一致（%s）" % mine)
assert mine != "OLeaidS1JvxuMvnyHOwuJ+uX5qY="   # 文档那串是过期的，别照它改代码
check("确认过：文档末尾那个签名常量跟它自己的 StringToSign 对不上，不能拿来当标准答案")

# 三种写错的方式，签出来都不一样 —— 这几条守卫真会红
base = A._sign("GET", GOLD, "testsecret")
no_amp = base64.b64encode(hmac.new(b"testsecret", sts.encode(), hashlib.sha1).digest()).decode()
post = A._sign("POST", GOLD, "testsecret")
single = base64.b64encode(hmac.new(b"testsecret&", sts.replace("%253A","%3A").encode(),
                                   hashlib.sha1).digest()).decode()
assert len({base, no_amp, post, single}) == 4
check("密钥少拼 &、方法写错、时间戳少编一层 —— 三种写法签出来全不一样")

# percentEncode 的四个特例
assert A._percent(" ") == "%20" and A._percent("*") == "%2A"
assert A._percent("~") == "~" and A._percent("/") == "%2F"
assert A._percent("a-b_c.d~e") == "a-b_c.d~e"
check("percentEncode：空格→%20、*→%2A、~ 和 -_. 原样不动")

# ========== 2. 请求形状 ==========
sent = []
def fake(method, url, headers, body=None, timeout=30, form=None):
    sent.append({"method":method, "url":url, "form":form})
    if "RunInstances" in (form or {}).get("Action",""):
        return {"InstanceIdSets":{"InstanceIdSet":["i-bp67acfmxazb4ph"]}}
    return {}
G.request_json = fake

p = A({"key_id":"AK", "key_secret":"SK"})
p.set_context({"region":"cn-beijing"})
sent[:] = []
iid = p.create({"spec":"cn-beijing-i|ecs.gn7i-c8g1.2xlarge", "image":"ubuntu_22",
                "sg":"sg-1", "vsw":"vsw-1", "disk":100, "disk_cat":"cloud_essd",
                "strategy":"SpotAsPriceGo", "price":"", "bandwidth":5,
                "name":"changji", "password":"Changji#2026"})
r = sent[0]
assert iid == "i-bp67acfmxazb4ph"
assert r["method"] == "POST", r["method"]
assert r["url"] == "https://ecs.cn-beijing.aliyuncs.com/", r["url"]
f = r["form"]
assert f["Action"] == "RunInstances" and f["Version"] == "2014-05-26"
assert f["ZoneId"] == "cn-beijing-i" and f["InstanceType"] == "ecs.gn7i-c8g1.2xlarge"
assert f["SpotStrategy"] == "SpotAsPriceGo" and f["InstanceChargeType"] == "PostPaid"
assert f["SystemDisk.Category"] == "cloud_essd" and f["SystemDisk.Size"] == 100
assert f["InternetMaxBandwidthOut"] == 5 and f["InternetChargeType"] == "PayByTraffic"
assert "SpotPriceLimit" not in f          # 随市场价就别发上限
assert f["Signature"] and f["SignatureNonce"]
check("RunInstances：地域进了域名、可用区和规格拆开了、抢占参数都对")

# 公网：按流量计费，带宽默认拉到上限
bw = [fd for fd in A().fields() if fd.key == "bandwidth"][0]
assert bw.default == "100" and bw.hi == 100, (bw.default, bw.hi)
check("公网带宽默认就是上限 100（按流量计费下它只是峰值上限，填满不额外花钱）")
assert f["InternetChargeType"] == "PayByTraffic"
check("公网计费方式是按流量，不是按带宽包月")

# 密码绝不能出现在 URL 里
assert "Changji" not in r["url"] and "Password" not in r["url"]
assert f["Password"] == "Changji#2026"    # 只在 POST 表单体里
check("实例密码只在 POST 表单体里，一个字都没进 URL")

sent[:] = []
p.create({"spec":"cn-beijing-i|ecs.gn7i", "image":"i", "sg":"s", "vsw":"v", "disk":40,
          "disk_cat":"cloud_ssd", "strategy":"SpotWithPriceLimit", "price":"3.5",
          "bandwidth":0, "name":"", "password":"Changji#2026"})
assert sent[0]["form"]["SpotPriceLimit"] == "3.5"
assert sent[0]["form"]["InstanceName"] == "changji"     # 空名字给个默认
check("选了「设价格上限」才发 SpotPriceLimit")

sent[:] = []
p.power_on("i-1"); p.power_off("i-1"); p.release("i-1")
assert [x["form"]["Action"] for x in sent] == ["StartInstance","StopInstance","DeleteInstance"]
assert "ForceStop" not in sent[1]["form"]     # 手点「关机」是优雅关机
check("开机/关机/删除三个 Action 对，手点关机不强制断电")

# ========== 3. 大小写：阿里云的状态是 Stopped 不是 stopped ==========
assert G.is_off("Stopped") and G.is_off("STOPPED") and G.is_off(" stopped ")
assert not G.is_off("Running") and not G.is_off("Stopping")
check("状态判据大小写无关：Stopped 认得出来（原来只比小写会一直等到超时）")

# ========== 4. 释放：关机(强制) → 等停稳 → 删 ==========
calls = []
class Rec(A):
    def __init__(self):
        A.__init__(self, {"key_id":"AK","key_secret":"SK"})
        self.seq = list(self._states)
    _states = ["Running", "Stopping", "Stopped"]
    def status(self, i):
        calls.append("status")
        return self.seq.pop(0) if len(self.seq) > 1 else self.seq[0]
    def _call(self, action, params=None, region=None):
        calls.append(action + (":force" if (params or {}).get("ForceStop") else ""))
        return {}
G.time.sleep = lambda s: None
r = Rec(); r.release_safely("i-1", lambda m: None)
assert calls == ["status","StopInstance:force","status","status","DeleteInstance"], calls
check("释放：先查 → 强制关机（盘马上要删，不等优雅关机）→ 等停稳 → 删")

calls[:] = []
class Off(Rec):
    _states = ["Stopped"]
o = Off(); o.release_safely("i-2", lambda m: None)
assert calls == ["status","DeleteInstance"], calls
check("已经是 Stopped：不多关一次，直接删")

calls[:] = []
class Stuck(Rec):
    _states = ["Running"]
try:
    Stuck().release_safely("i-3", lambda m: None, wait_s=0.01)
except G.CloudError as e:
    assert "DeleteInstance" not in calls, calls
    check("关不掉就不删，把真情况报出来：%s" % str(e)[:34])
else:
    raise AssertionError("关不掉却删了")

# ========== 5. 错误里那句人话（阿里云是大写字段） ==========
msg = G._error_text(json.dumps({"RequestId":"x", "HostId":"ecs.aliyuncs.com",
    "Code":"InvalidAccessKeyId.NotFound",
    "Message":"Specified access key is not found."}))
assert "not found" in msg and "InvalidAccessKeyId.NotFound" in msg, msg
check("错误原话：Message 和 Code 都带上（%s）" % msg[:46])

# ========== 6. 时间和密码 ==========
assert A._local_time("2026-09-18T10:30Z") == time.strftime(
    "%Y-%m-%d %H:%M:%S", time.localtime(__import__("calendar").timegm(
        time.strptime("2026-09-18T10:30Z", "%Y-%m-%dT%H:%MZ"))))
assert A._local_time("2026-09-18T10:30:45Z").endswith("45")   # 到秒的也认
assert A._local_time("说不清") == "说不清"                      # 认不出就原样
check("创建时间：UTC 转本地，到分和到秒两种格式都认")

for pw, why in [("Ab#1", "太短"), ("abcdefghij", "只有一类"),
                ("abcdefgh1", "只有两类"), ("A"*31 + "b1#", "太长")]:
    try:
        A._check_password(pw)
    except G.CloudError:
        pass
    else:
        raise AssertionError("这个密码该被拦下来：%s（%s）" % (pw, why))
A._check_password("Changji#2026")     # 四类占三类，长度够
A._check_password("changji2026#")     # 小写+数字+符号 也是三类
check("密码本地先判：长度和「四类占三类」不合就当场说，不用去猜接口那句 Malformed")

# ========== 已用流量 ==========
MON = []
def with_mon(method, url, headers, body=None, timeout=30, form=None):
    act = form["Action"]
    if act == "DescribeInstances":
        return {"Instances": {"Instance": [
            {"InstanceId":"i-a","InstanceName":"跑着的","Status":"Running",
             "InstanceType":"ecs.gn8is.4xlarge","SpotStrategy":"SpotAsPriceGo",
             "ZoneId":"cn-wulanchabu-a","CreationTime":"2026-09-19T01:00Z","GPUAmount":1},
            {"InstanceId":"i-b","InstanceName":"停着的","Status":"Stopped",
             "InstanceType":"ecs.gn8is.4xlarge","SpotStrategy":"SpotAsPriceGo",
             "ZoneId":"cn-wulanchabu-a","CreationTime":"2026-09-18T01:00Z","GPUAmount":1}]}}
    if act == "DescribeInstanceMonitorData":
        MON.append((form["InstanceId"], form["StartTime"], form["EndTime"], form["Period"]))
        if form["InstanceId"] == "i-a":
            # 24 段，每段 400000 kbits → 合计 9600000 kbits = 1.2 GB
            return {"MonitorData": {"InstanceMonitorData":
                [{"InternetTX": 400000, "InternetRX": 999999} for _ in range(24)]}}
        return {"MonitorData": {"InstanceMonitorData": [{"InternetTX": 4000}]}}
    return {}
G.request_json = with_mon
q = A({"key_id":"AK","key_secret":"SK"})
q.set_context({"region":"cn-wulanchabu"})
rows = q.instances()
assert abs(rows[0]["traffic"] - 1.2) < 1e-9, rows[0]["traffic"]
assert abs(rows[1]["traffic"] - 0.0005) < 1e-9, rows[1]["traffic"]
check("出网流量按段累加再换算：24×400000 kbits = 1.20 GB")
assert all(m[3] == 3600 for m in MON) and len(MON) == 2
assert MON[0][1].endswith(":00Z") and MON[0][2].endswith(":00Z")
check("每台查一次、Period=3600、时间戳对齐到整分（秒不是 00 阿里云会进位）")

# 只看出网：入网再大也不算进来（入网不花钱）
assert rows[0]["traffic"] < 2, "把入网也算进去了"
check("只算出网（InternetTX），入网 999999 没被算进来")

# 缓存：紧接着再刷一次不该再问监控接口
MON[:] = []
q.instances()
assert MON == [], MON
check("两分钟内再刷新不重复问监控接口（自动刷新 15 秒一次，不然全在问流量）")

# 查不到流量不能把整张表弄没
def mon_down(method, url, headers, body=None, timeout=30, form=None):
    if form["Action"] == "DescribeInstanceMonitorData":
        raise G.CloudError("Throttling")
    return with_mon(method, url, headers, body, timeout, form)
G.request_json = mon_down
q2 = A({"key_id":"AK","key_secret":"SK"}); q2.set_context({"region":"cn-wulanchabu"})
rows2 = q2.instances()
assert len(rows2) == 2 and all(r["traffic"] is None for r in rows2)
check("监控接口挂了：流量那栏空着，机器列表照样出来")

assert G.fmt_traffic(None) == "—"
assert G.fmt_traffic(0.0005) == "0 MB" or G.fmt_traffic(0.0005) == "1 MB"
assert G.fmt_traffic(0.25) == "250 MB" and G.fmt_traffic(1.2) == "1.20 GB"
check("显示：不到 1 GB 说 MB，够了说 GB，没有说「—」")

assert A.TRAFFIC is True
assert G.AutoDLPlatform.TRAFFIC is False and G.PPIOPlatform.TRAFFIC is False
check("AutoDL / PPIO 的接口不报流量，这一栏在它们那儿会被整列藏起来")

print("\n阿里云 %d 项全过" % len(ok))
