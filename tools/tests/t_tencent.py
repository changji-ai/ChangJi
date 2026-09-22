# -*- coding: utf-8 -*-
import hashlib, json, subprocess, sys, time
import os
# tests/ 的上一层就是 tools/，被测的模块在那儿
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import autodl_gui as G
T = G.TencentPlatform
ok=[]
def check(n): ok.append(n); print("  ✓", n)

# ========== 1. 签名：对着官方例子里那两个中间哈希 ==========
BODY = '{"Limit": 1, "Filters": [{"Values": ["\\u672a\\u547d\\u540d"], "Name": "instance-name"}]}'
HOST = "cvm.tencentcloudapi.com"
TS = 1551113065
WANT_BODY_HASH = "35e9c5b0e3ae67532d3c9f17ead6c90222632e5b1ff7f6e89887f1398934f064"
WANT_CR_HASH = "7019a55be8395899b900fb5564e4200d984910f34794a27cb3fb7d10ff6a1e84"

# json.dumps 的默认输出必须跟文档那串一模一样（非 ASCII 转义、冒号逗号后有空格）
mine = json.dumps({"Limit": 1, "Filters": [{"Values": ["未命名"], "Name": "instance-name"}]})
assert mine == BODY, "\n实得 " + mine + "\n应为 " + BODY
check("json.dumps 的默认输出跟文档例子逐字节一致（所以能签自己发出去的那串）")

assert hashlib.sha256(BODY.encode()).hexdigest() == WANT_BODY_HASH
check("请求体哈希 = 官方那串 35e9c5b0…")

cr = T._canonical_request("DescribeInstances", HOST, BODY)
assert cr.split("\n")[:3] == ["POST", "/", ""], cr.split("\n")[:5]
assert "x-tc-action:describeinstances" in cr      # 动作名要小写
assert hashlib.sha256(cr.encode()).hexdigest() == WANT_CR_HASH, cr
check("规范请求串哈希 = 官方那串 7019a55b…（六行、请求头后面那个空行都对上了）")

sts = T._string_to_sign("DescribeInstances", HOST, BODY, "cvm", TS)
assert sts == "\n".join(["TC3-HMAC-SHA256", "1551113065",
                         "2019-02-25/cvm/tc3_request", WANT_CR_HASH]), sts
check("待签串四行跟文档一致（含 2019-02-25/cvm/tc3_request 这个凭证范围）")

# 派生链跟 openssl 对（官方例子里的密钥是打码的，最终签名没法对，但这条链能对）
p = T({"secret_id": "AKIDEXAMPLE", "secret_key": "SECRETEXAMPLE"})
auth = p._authorization("DescribeInstances", HOST, BODY, "cvm", TS)
sig = auth.split("Signature=")[1]
def sh(key_hex_or_str, msg, is_hex=False):
    # ⚠️ hexkey 必须配 -mac HMAC，少了它 openssl 会**静默忽略 -macopt**、
    # 算成普通摘要，对出来的结果全是错的（这里踩过一次）
    k = (["-mac", "HMAC", "-macopt", "hexkey:" + key_hex_or_str] if is_hex
         else ["-hmac", key_hex_or_str])
    out = subprocess.run(["openssl", "dgst", "-sha256"] + k, input=msg.encode(),
                         capture_output=True).stdout.decode().strip()
    return out.rsplit(" ", 1)[-1]
k1 = sh("TC3SECRETEXAMPLE", "2019-02-25")
k2 = sh(k1, "cvm", True)
k3 = sh(k2, "tc3_request", True)
ref = sh(k3, sts, True)
assert sig == ref, (sig, ref)
check("四层派生（TC3+密钥→日期→cvm→tc3_request）跟 openssl 逐字节一致")

assert auth.startswith("TC3-HMAC-SHA256 Credential=AKIDEXAMPLE/2019-02-25/cvm/tc3_request, ")
assert "SignedHeaders=content-type;host;x-tc-action" in auth
check("Authorization 头的格式跟文档一致")

# 三种写错的方式，哈希都不一样 —— 守卫真会红
import copy
bad = []
bad.append(hashlib.sha256("\n".join(["POST","/","",
    "content-type:%s\nhost:%s\nx-tc-action:describeinstances" % (T.CT, HOST),
    "content-type;host;x-tc-action", WANT_BODY_HASH]).encode()).hexdigest())  # 少那个空行
bad.append(hashlib.sha256(T._canonical_request("DescribeInstances", HOST,
    json.dumps({"Limit":1,"Filters":[{"Values":["未命名"],"Name":"instance-name"}]},
               separators=(",",":"))).encode()).hexdigest())                   # 重新序列化
bad.append(hashlib.sha256(T._canonical_request("describeinstances", "cvm.x.com",
    BODY).encode()).hexdigest())                                              # host 错
assert WANT_CR_HASH not in bad and len(set(bad)) == 3
check("少一个空行 / 重新序列化请求体 / host 写错 —— 三种都签不出正确的串")

# ========== 2. 错误是 HTTP 200 回的 ==========
try:
    T._unwrap({"Response": {"Error": {"Code": "AuthFailure.SignatureFailure",
                                      "Message": "签名不对"}, "RequestId": "x"}})
except G.CloudError as e:
    assert "签名不对" in str(e) and "AuthFailure" in str(e), e
    check("Response.Error 被挑出来当错误（HTTP 200 也是错，不挑就当成功了）")
else:
    raise AssertionError("HTTP 200 里的错误没被挑出来")
assert T._unwrap({"Response": {"InstanceSet": [1], "RequestId": "x"}}) == {
    "InstanceSet": [1], "RequestId": "x"}
check("正常回包剥掉 Response 这层")

# ========== 3. 请求形状 ==========
SENT = []
def fake(method, url, headers, body=None, timeout=30, form=None, raw=None):
    SENT.append({"url": url, "action": headers["X-TC-Action"],
                 "region": headers["X-TC-Region"], "version": headers["X-TC-Version"],
                 "body": json.loads(raw), "raw": raw, "auth": headers["Authorization"]})
    if headers["X-TC-Action"] == "RunInstances":
        return {"Response": {"InstanceIdSet": ["ins-rn79mzt1"], "RequestId": "r"}}
    return {"Response": {"RequestId": "r"}}
G.request_json = fake
q = T({"secret_id": "SID", "secret_key": "SKEY"})
q.set_context({"region": "ap-shanghai"})
SENT[:] = []
iid = q.create({"spec": "ap-shanghai-2|GN10Xp.2XLARGE40", "image": "img-1",
                "sg": "sg-1", "subnet": "subnet-1", "disk": 100,
                "disk_type": "CLOUD_PREMIUM", "strategy": "market", "price": "",
                "bandwidth": 100, "name": "changji", "password": "Changji#2026"})
r = SENT[0]; b = r["body"]
assert iid == "ins-rn79mzt1"
assert r["url"] == "https://cvm.tencentcloudapi.com/" and r["version"] == "2017-03-12"
assert r["region"] == "ap-shanghai"            # 地域走请求头，不换域名
assert b["Placement"]["Zone"] == "ap-shanghai-2"
assert b["InstanceChargeType"] == "SPOTPAID"
assert b["InstanceMarketOptions"]["MarketType"] == "spot"
assert "MaxPrice" not in b["InstanceMarketOptions"]["SpotOptions"]   # 随市场价不发上限
assert b["InternetAccessible"] == {"InternetChargeType": "TRAFFIC_POSTPAID_BY_HOUR",
                                   "InternetMaxBandwidthOut": 100, "PublicIpAssigned": True}
assert b["SystemDisk"] == {"DiskType": "CLOUD_PREMIUM", "DiskSize": 100}
check("RunInstances：竞价参数、可用区、公网按流量、带宽拉满都对")

assert "Changji" not in r["url"] and "Password" not in r["url"]
assert b["LoginSettings"]["Password"] == "Changji#2026"
check("实例密码只在请求体里，一个字没进 URL")

SENT[:] = []
q.create({"spec": "ap-shanghai-2|GN7.2XLARGE32", "image": "i", "sg": "s", "subnet": "n", "disk": 50,
          "disk_type": "CLOUD_SSD", "strategy": "limit", "price": "3.5",
          "bandwidth": 0, "name": "", "password": "Changji#2026"})
b = SENT[0]["body"]
assert b["InstanceMarketOptions"]["SpotOptions"]["MaxPrice"] == "3.5"
assert b["InternetAccessible"]["PublicIpAssigned"] is False    # 带宽 0 就不要公网 IP
check("设了价格上限才发 MaxPrice；带宽 0 时不分配公网 IP")

SENT[:] = []
q.power_on("ins-1"); q.power_off("ins-1"); q.release("ins-1")
assert [x["action"] for x in SENT] == ["StartInstances", "StopInstances", "TerminateInstances"]
assert all(x["body"]["InstanceIds"] == ["ins-1"] for x in SENT)
check("开机/关机/销毁三个 Action 对，实例 ID 是数组")

# 安全组和子网在 vpc 那套里，不在 cvm
SENT[:] = []
try:
    q.load_choices()
except Exception:
    pass
hosts = {x["action"]: x["url"] for x in SENT}
assert hosts.get("DescribeSecurityGroups", "").startswith("https://vpc.")
assert hosts.get("DescribeSubnets", "").startswith("https://vpc.")
assert hosts.get("DescribeImages", "").startswith("https://cvm.")
check("安全组和子网发到 vpc.tencentcloudapi.com，其余发到 cvm（发错域名会 404）")

# ========== 4. 大小写 + 释放 ==========
assert G.is_off("STOPPED") and not G.is_off("RUNNING")
check("腾讯云的状态是全大写的 STOPPED / RUNNING，判据认得出来")

calls = []
class Rec(T):
    def __init__(self, fail=0):
        T.__init__(self, {"secret_id":"a","secret_key":"b"})
        self.fail = fail
    def status(self, i):
        calls.append("status"); return "STOPPED" if len(calls) > 2 else "RUNNING"
    def _call(self, action, payload=None, service="cvm", region=None):
        calls.append(action)
        if action == "TerminateInstances" and self.fail:
            self.fail -= 1
            raise G.CloudError("实例状态不允许")
        return {}
G.time.sleep = lambda s: None
Rec().release_safely("ins-1", lambda m: None)
assert calls == ["TerminateInstances"], calls
check("销毁：常见情况一步就成，不让用户白等一轮关机")
calls[:] = []
Rec(fail=1).release_safely("ins-2", lambda m: None)
assert calls[0] == "TerminateInstances" and "StopInstances" in calls
assert calls[-1] == "TerminateInstances", calls
check("销毁不动：退回 关机 → 等停稳 → 再销毁")

# ========== 5. 密码：两家要求的类数不同 ==========
G.check_password("abcd1234", 2)              # 腾讯云两类就行
try:
    G.check_password("abcd1234", 3)          # 阿里云要三类
except G.CloudError as e:
    assert "3 类" in str(e), e
    check("密码校验按各家要求判：腾讯云两类放行，阿里云同一个密码要三类会拦")

# ========== 6. 显存：接口不报，靠机型族对 ==========
# 先钉住那个**已经写错过**的地方：Gpu 是卡数（整数），不是型号
card, vram, n = T._gpu_of({"InstanceFamily":"GN10Xp", "Gpu":8, "GpuCount":8,
                           "Cpu":80, "Memory":320})
assert card == "NVIDIA V100 NVLink" and vram == 32 and n == 8, (card, vram, n)
check("GN10Xp → V100 NVLink / 单卡 32G / 8 张（Gpu 那个 8 是卡数，不是型号）")

for fam, want_card, want_vram in [("GN7","NVIDIA T4",16), ("GT4","NVIDIA A100 NVLink",40),
                                  ("PNV4","NVIDIA A10",24), ("GN8","NVIDIA P40",24),
                                  ("GN6","NVIDIA P4",8)]:
    c, vm, _ = T._gpu_of({"InstanceFamily": fam, "GpuCount": 1})
    assert (c, vm) == (want_card, want_vram), (fam, c, vm)
check("五个常见机型族的型号和显存都对得上官方机型表")

# 接口哪天真回了 GpuType / GpuMemory，要优先用接口的
c, vm, _ = T._gpu_of({"InstanceFamily":"GN7", "GpuCount":1,
                      "GpuType":"NVIDIA L40S", "GpuMemory":48})
assert (c, vm) == ("NVIDIA L40S", 48), (c, vm)
check("接口真给了 GpuType/GpuMemory 就用接口的，表只兜底")

# 不认识的族留空，不猜
c, vm, _ = T._gpu_of({"InstanceFamily":"GX99", "GpuCount":1})
assert vm == "" and c == "GX99", (c, vm)
check("没见过的机型族：显存留空不猜（错的显存会让筛选放行跑不动的机器）")

# vGPU 切片要按比例折算
c, vm, n = T._gpu_of({"InstanceFamily":"GN7vi", "GpuCount":0.25})
assert vm == 4.0 and n == 0.25, (vm, n)
check("1/4 张 T4 折算成 4G，不报 16G（不折算的话显存筛选会把它当整卡放进来）")

# ========== 7. 真跑一次之后逮到的那几个 ==========
# 价格：竞价的真实价是 UnitPriceDiscount，不是 UnitPrice
assert T._real_price({"Price": {"UnitPrice": 11.88, "UnitPriceDiscount": 2.376}}) == 2.376
assert T._real_price({"Price": {"UnitPrice": 8.68}}) == 8.68      # 没折扣字段就用原价
assert T._real_price({}) is None
check("竞价价取 UnitPriceDiscount（UnitPrice 等于按量原价，拿它当竞价价会报高五倍）")

# Remark 里写着卡型号，比按机型族猜靠谱
for remark, want_card, want_vram in [
        ("4 颗 NVIDIA T4", "NVIDIA T4", 16),
        ("8 * NVIDIA V100", "NVIDIA V100", 32),
        ("8 * NVIDIA A100", "NVIDIA A100", "")]:   # A100 有 40/80 两种，不猜
    c, vm, _ = T._gpu_of({"InstanceFamily": "HCCxx", "GpuCount": 8, "Remark": remark})
    assert (c, vm) == (want_card, want_vram), (remark, c, vm)
check("从 Remark 认卡：BMG5t/HCCG5v 这些不在机型表里的族也认得出来")
assert T._gpu_of({"InstanceFamily":"HCCPNV4h","GpuCount":8,
                  "Remark":"8 * NVIDIA A100"})[1] == ""
check("A100 有 40G 和 80G 两种，光看名字分不出来 → 显存留空，不猜")

# 延迟探针不能拿阿里云的域名去探腾讯云
assert G.AliyunPlatform.LATENCY_HOST == "oss-%s.aliyuncs.com"
assert T.LATENCY_HOST == "cos.%s.myqcloud.com"
assert G.AutoDLPlatform.LATENCY_HOST == ""
_, why = G.AutoDLPlatform._probe_latency("whatever")
assert "没有可用来探延迟的地域接入点" in why
check("延迟探针用各家自己的接入点（原来拿 oss-ap-beijing.aliyuncs.com 探腾讯云）")

# 没货的也列出来，但标清楚
SENT2 = []
def q(z, t, status, cat="WithoutStock"):
    return {"Zone": z, "InstanceType": t, "InstanceFamily": "GN7", "Cpu": 10,
            "Memory": 32, "GpuCount": 1, "Status": status, "StatusCategory": cat,
            "Price": {"UnitPrice": 8.68, "UnitPriceDiscount": 1.736}}
def fake2(method, url, headers, body=None, timeout=30, form=None, raw=None):
    act = headers["X-TC-Action"]
    if act == "DescribeRegions":
        return {"Response": {"RegionSet": [{"Region": "ap-shanghai", "RegionState": "AVAILABLE"}]}}
    if act == "DescribeZoneInstanceConfigInfos":
        return {"Response": {"InstanceTypeQuotaSet": [
            q("ap-shanghai-2", "GN7.HAS", "SELL", "UnderStock"),
            q("ap-shanghai-5", "GN7.NONE", "SOLD_OUT")]}}
    return {"Response": {}}
G.request_json = fake2
T._probe_latency = classmethod(lambda cls, r, tries=2, timeout=3.0: (None, "代理"))
rows = T({"secret_id":"a","secret_key":"b"}).price_table(lambda m: None)
assert len(rows) == 2, rows
assert [r["stock"] for r in rows] == [True, False] or [r["stock"] for r in rows] == [False, True]
by = {r["spec"]: r for r in rows}
assert by["GN7.HAS"]["stock"] is True and by["GN7.HAS"]["note"] == "有货"
assert by["GN7.NONE"]["stock"] is False and "无货" in by["GN7.NONE"]["note"]
assert by["GN7.HAS"]["spot"] == 1.736
check("没货的也列出来并标「无货」（上海 31 种带卡的当时只有 1 种有货，全丢掉表就空了）")

# 地域和可用区对不上要拦住（跟阿里云同一条判据）
q2 = T({"secret_id":"a","secret_key":"b"}); q2.set_context({"region":"ap-beijing"})
try:
    q2.validate({"spec":"ap-shanghai-2|GN7.2XLARGE32","image":"i","sg":"s",
                 "subnet":"n","disk":100,"disk_type":"CLOUD_PREMIUM",
                 "strategy":"market","price":"","bandwidth":100,"name":"x",
                 "password":"Changji#2026"})
except G.CloudError as e:
    assert "不在地域" in str(e), e
    check("腾讯云同样：可用区不在选中的地域里就拦住")
else:
    raise AssertionError("没拦住")

print("\n腾讯云 %d 项全过" % len(ok))
