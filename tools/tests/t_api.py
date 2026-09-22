# -*- coding: utf-8 -*-
import io, json, sys, os, time, urllib.error
# tests/ 的上一层就是 tools/，被测的模块在那儿
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import autodl_gui as G

ok = []
def check(name): ok.append(name); print("  ✓", name)

# ========== A. 错误里那句人话，各家藏的栏不一样 ==========
assert G._error_text('{"code":"Fail","msg":"未实名"}') == "未实名"
assert G._error_text('{"error":{"message":"product sold out"}}') == "product sold out"
assert G._error_text('{"message":"insufficient balance"}') == "insufficient balance"
assert G._error_text('{"error":"UNAUTHORIZED"}') == "UNAUTHORIZED"
assert G._error_text("<html>502</html>").startswith("<html>")
check("错误原话抽取：4 种形状 + 非 JSON")

# ========== B. GET 带 body（AutoDL 的非常规写法）==========
seen = {}
def spy_urlopen(req, timeout=0):
    seen["method"] = req.get_method(); seen["url"] = req.full_url
    seen["body"] = req.data.decode() if req.data else None
    seen["hdr"] = dict(req.headers)
    class R:
        def read(s): return b'{"code":"Success","data":"running","msg":""}'
        def close(s): pass
    return R()
G.urllib.request.urlopen = spy_urlopen
a = G.AutoDLPlatform("tok-123")
assert a.status("pro-abc") == "running"
assert seen["method"] == "GET", seen          # urllib 见到 data 会想改成 POST
# ⚠️ **这条原来钉的是「GET 带 body」——那是照文档写的，而文档是错的。**
# 2026-09-20 拿真账号试出来：GET 带 body 回 RequestParameterIsWrong，
# 换 POST 是 404，只有 GET + query string 才通。所以这个工具的
# status / detail 一直是坏的，而 release_safely 第一步就查状态，
# **释放整条也跟着坏**——用例却一直绿着，因为它照着文档钉。
assert "instance_uuid=pro-abc" in seen["url"], seen["url"]
assert seen["body"] is None, "GET 不该带 body：AutoDL 收到 body 会说参数错误"
assert seen["hdr"]["Authorization"] == "tok-123"      # AutoDL 是裸 token
check("AutoDL 的 GET 走 query string 不走 body（文档写反了，真账号试出来的）")

# ========== C. 两家的请求形状 ==========
calls = []
def fake(method, url, headers, body=None, timeout=30):
    calls.append((method, url, body, headers.get("Authorization")))
    if url.endswith("/gpus/v2/instances") and method == "POST":
        return {"id": "ca338f12f3"}
    if "/api/v1/dev/instance/pro/create" in url:
        return {"code": "Success", "data": "pro-76419909953e", "msg": ""}
    return {"code": "Success", "data": None, "msg": ""}
G.request_json = fake

# AutoDL：选填项留空时**不发空值**
a = G.AutoDLPlatform("tok")
calls[:] = []
iid = a.create({"spec": "v-48g", "gpu_num": 2, "disk": 0, "image": "base-image-x",
                "cuda": 118, "regions": [], "name": "", "command": ""})
body = calls[0][2]
assert iid == "pro-76419909953e"
assert body == {"req_gpu_amount": 2, "expand_system_disk_by_gb": 0,
                "gpu_spec_uuid": "v-48g", "image_uuid": "base-image-x",
                "cuda_v_from": 118}, body
calls[:] = []
a.create({"spec": "v-48g", "gpu_num": 1, "disk": 100, "image": "i", "cuda": 124,
          "regions": ["westDC3"], "name": "changji", "command": "sleep 1"})
body = calls[0][2]
assert body["data_center_list"] == ["westDC3"] and body["instance_name"] == "changji"
assert body["start_command"] == "sleep 1"
check("AutoDL 建机 body：空的选填项不发，填了的都在")

# PPIO：嵌套 body + REST 动词
p = G.PPIOPlatform("sk-ppio")
calls[:] = []
iid = p.create({"product": "prod-4090", "gpu_num": 2, "disk": 80,
                "image": "registry.x/ns/img:tag", "billing": "spot",
                "name": "changji", "command": "sleep 1"})
m, url, body, auth = calls[0]
assert iid == "ca338f12f3"
assert (m, url) == ("POST", "https://api.ppio.com/gpus/v2/instances")
assert auth == "Bearer sk-ppio"
assert body == {"name": "changji", "product_id": "prod-4090",
                "image": "registry.x/ns/img:tag", "type": "gpu",
                "billing": {"mode": "spot"},
                "resource": {"rootfs_size_gb": 80, "gpu_num": 2},
                "command": "sleep 1"}, body
check("PPIO 建机 body：billing/resource 嵌套对，Bearer 头对")

calls[:] = []
p.power_on("ca338f12f3"); p.power_off("ca338f12f3"); p.release("ca338f12f3")
assert [(c[0], c[1].split("/gpus")[1]) for c in calls] == [
    ("PUT", "/v2/instances/ca338f12f3/start"),
    ("PUT", "/v2/instances/ca338f12f3/stop"),
    ("DELETE", "/v2/instances/ca338f12f3")], calls
check("PPIO 启停删：PUT/PUT/DELETE 三个 URL 都对")

# ========== D. 列表归一化：两家字段名天差地别，画到同一张表上 ==========
G.request_json = lambda m, u, h, body=None, timeout=30: {"data": [{
    "id": "ca338f12f3", "name": "changji", "product_id": "prod-4090",
    "billing": {"mode": "spot"}, "resource_specs": {"gpu_num": 2},
    "region": "cn-south-1", "status": {"status": "running", "message": ""},
    "created_at": 1714464000}]}
row = G.PPIOPlatform("k").instances()[0]
assert row["status"] == "running" and row["gpu"] == 2 and row["billing"] == "spot"
assert row["created"] == time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(1714464000))
assert row["id"] == "ca338f12f3"
check("PPIO 列表归一化：unix 时间戳转成人看的，状态从嵌套里挖出来")

G.request_json = lambda m, u, h, body=None, timeout=30: {"code": "Success", "msg": "", "data": {"list": [{
    "uuid": "pro-765", "name": "zq", "status": "running", "gpu_spec_uuid": "pro6000-p",
    "req_gpu_amount": 1, "region_name": "内蒙C区", "charge_type": "payg",
    "created_at": "2025-12-15T17:30:54+08:00"}]}}
row = G.AutoDLPlatform("k").instances()[0]
assert row["id"] == "pro-765" and row["billing"] == "按量"
assert row["created"] == "2025-12-15 17:30:54"
check("AutoDL 列表归一化：同样八栏")

# ========== E. 价格：整数 + 小数位数，得自己还原 ==========
G.request_json = lambda m, u, h, body=None, timeout=30: (
    {"data": [{"id": "prod-a100", "name": "A100-80G",
               "resource_spec": {"gpu": {"memory_gb": 80, "available": 6}},
               "pricing": {"precision": 3, "postpaid": {"final_price": 12600},
                           "spot": {"final_price": 6000}}}]}
    if "products" in u else {"data": []})
label, pid = G.PPIOPlatform("k").load_choices()["product"][0]
assert pid == "prod-a100"
assert label == "A100-80G 80G 可用6卡 按量¥12.60/时 抢占¥6.00/时", label
check("PPIO 规格标签：12600 + precision 3 → ¥12.60，可用卡数在里面")

# 不支持抢占的产品不能凭空编一个价
G.request_json = lambda m, u, h, body=None, timeout=30: (
    {"data": [{"id": "p", "name": "L20", "resource_spec": {"gpu": {}},
               "pricing": {"precision": 2, "postpaid": {"final_price": 300}, "spot": None}}]}
    if "products" in u else {"data": []})
label, _ = G.PPIOPlatform("k").load_choices()["product"][0]
assert label == "L20 按量¥3.00/时", label
check("没有抢占档的产品不瞎编价")

print("\nAPI 层 %d 项全过" % len(ok))
