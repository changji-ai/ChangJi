# -*- coding: utf-8 -*-
import os, sys, tempfile, time
os.environ["QT_QPA_PLATFORM"] = "offscreen"
# tests/ 的上一层就是 tools/，被测的模块在那儿
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import autodl_gui as G
from PySide6 import QtWidgets, QtCore
ok=[]
def check(n): ok.append(n); print("  ✓", n)
A = G.AliyunPlatform
ORIG_PROBE = G.Platform.__dict__["_probe_latency"]   # 已经挪到基类了；先存原件

# ========== 阿里云：按地域 × 规格取价 ==========
CALLS = []
def fake(method, url, headers, body=None, timeout=30, form=None):
    act = form["Action"]
    CALLS.append((act, form.get("RegionId"), form.get("InstanceType")))
    if act == "DescribeRegions":
        return {"Regions": {"Region": [
            {"RegionId": "cn-hangzhou"}, {"RegionId": "cn-beijing"},
            {"RegionId": "ap-southeast-1"}]}}     # 海外的默认不扫
    if act == "DescribeInstanceTypes":
        return {"InstanceTypes": {"InstanceType": [
            {"InstanceTypeId": "ecs.gn7i-c8g1.2xlarge", "GPUAmount": 1,
             "GPUSpec": "NVIDIA A10", "GPUMemorySize": 24,
             "CpuCoreCount": 8, "MemorySize": 30},
            {"InstanceTypeId": "ecs.gn6i-c4g1.xlarge", "GPUAmount": 1,
             "GPUSpec": "NVIDIA T4", "GPUMemorySize": 16,
             "CpuCoreCount": 4, "MemorySize": 15},
            {"InstanceTypeId": "ecs.g6.large", "GPUAmount": 0}]}}   # 没卡的不要
    if act == "DescribeAvailableResource":
        avail = {"cn-hangzhou": ["ecs.gn7i-c8g1.2xlarge", "ecs.gn6i-c4g1.xlarge", "ecs.g6.large"],
                 "cn-beijing": ["ecs.gn7i-c8g1.2xlarge"],
                 "ap-southeast-1": ["ecs.gn7i-c8g1.2xlarge"]}.get(form["RegionId"], [])
        return {"AvailableZones": {"AvailableZone": [{"ZoneId": form["RegionId"]+"-k",
            "AvailableResources": {"AvailableResource": [{"SupportedResources":
            {"SupportedResource": [{"Value": v, "Status": "Available"} for v in avail]}}]}}]}}
    if act == "DescribeSpotPriceHistory":
        price = {("cn-hangzhou","ecs.gn7i-c8g1.2xlarge"): 10.5,
                 ("cn-hangzhou","ecs.gn6i-c4g1.xlarge"): 9.9,
                 ("cn-beijing","ecs.gn7i-c8g1.2xlarge"): 2.75,
                 # 海外这条最便宜：只扫 cn- 的话它根本不会出现在表里
                 ("ap-southeast-1","ecs.gn7i-c8g1.2xlarge"): 1.20}[
                     (form["RegionId"], form["InstanceType"])]
        z = form["RegionId"] + "-k"
        return {"Currency": "CNY", "SpotPrices": {"SpotPriceType": [
            # 同一个可用区回好几笔采样，要取时间最新的那一笔
            {"ZoneId": z, "SpotPrice": price + 3, "OriginPrice": 20.0,
             "InstanceType": form["InstanceType"], "Timestamp": "2026-09-19T01:00:00Z"},
            {"ZoneId": z, "SpotPrice": price, "OriginPrice": 20.0,
             "InstanceType": form["InstanceType"], "Timestamp": "2026-09-19T03:00:00Z"},
            {"ZoneId": z, "SpotPrice": price + 1, "OriginPrice": 20.0,
             "InstanceType": form["InstanceType"], "Timestamp": "2026-09-19T02:00:00Z"}]}}
    return {}
G.request_json = fake

# ★ 这条钉的是 2026-09-19 真撞上的那个 bug：DescribeAvailableResource 拿
#   cn-hangzhou 的接入点去问 cn-zhangjiakou，阿里云回 HTTP 500 UnknownError
#   （16 个国内地域里 8 个这样）；cn-wuhan-lr 这种更坑——不报错，直接回 0 条。
ENDPOINTS = []
_plain = fake
def fake_ep(method, url, headers, body=None, timeout=30, form=None):
    ENDPOINTS.append((form["Action"], url.split("//")[1].split(".")[1], form.get("RegionId")))
    return _plain(method, url, headers, body, timeout, form)
G.request_json = fake_ep
A({"key_id":"AK","key_secret":"SK"}).price_table(lambda m: None)
bad = [e for e in ENDPOINTS
       if e[0] in ("DescribeAvailableResource", "DescribeSpotPriceHistory")
       and e[1] != e[2]]
assert not bad, "这些请求发错了接入点：%r" % bad
check("按地域问的每一笔都发到那个地域自己的接入点（发错阿里云回 500，或者静悄悄回 0 条）")
G.request_json = fake

logs = []
p = A({"key_id":"AK","key_secret":"SK"})
rows = p.price_table(logs.append)
assert [r["spot"] for r in rows] == [1.20, 2.75, 9.9, 10.5], [r["spot"] for r in rows]
check("比价结果按抢占价从便宜到贵排：%s" % [r["spot"] for r in rows])
assert rows[0]["region"] == "ap-southeast-1" and rows[0]["gpu"] == "NVIDIA A10"
check("同一块 A10：新加坡 1.20、北京 2.75、杭州 10.5 —— 差价就是这个功能的意义")
assert all(r["spec"] != "ecs.g6.large" for r in rows)
check("没卡的规格不进这张表")
assert rows[0]["vram"] == 24 and rows[0]["mem"] == 30   # 显存 24G，系统内存 30G
check("显存和系统内存分两栏（最便宜的常是 2G vGPU 切片，不列显存就看不见）")

assert G.as_num(24) == 24.0 and G.as_num("24") == 24.0 and G.as_num(4.0) == 4.0
assert G.as_num("") is None and G.as_num(None) is None and G.as_num("说不清") is None
check("as_num：整数、字符串数字、小数都认，空和非数字返回 None")
assert rows[0]["pick"] == {"region":"ap-southeast-1",
                          "spec":"ap-southeast-1-k|ecs.gn7i-c8g1.2xlarge"}
check("每行都带着「填回表单」要用的地域和规格")
assert any(c[1] == "ap-southeast-1" for c in CALLS)
check("海外地域也要扫（海外常常比国内便宜一截，只扫 cn- 等于把便宜的藏起来）")
# 同一可用区三笔采样里取时间最新的那笔，不是第一笔也不是最便宜那笔
assert rows[3]["spot"] == 10.5   # 采样里还有 13.5 和 11.5
check("同一可用区回多笔采样时取时间最新的，不是随手拿第一笔")

# 一个地域挂了，别的照样要出来
def one_region_down(method, url, headers, body=None, timeout=30, form=None):
    if form["Action"] == "DescribeAvailableResource" and form["RegionId"] == "cn-beijing":
        raise G.CloudError("DescribeAvailableResource@cn-beijing：HTTP 500：炸了")
    return fake(method, url, headers, body, timeout, form)
G.request_json = one_region_down
G.time.sleep = lambda s: None
msgs = []
rows3 = A({"key_id":"AK","key_secret":"SK"}).price_table(msgs.append)
assert sorted(r["region"] for r in rows3) == ["ap-southeast-1", "cn-hangzhou", "cn-hangzhou"], rows3
assert any("没问出来" in m and "cn-beijing" in m for m in msgs), msgs
check("一个地域 500 了：跳过它、说清楚是哪个，杭州的报价照样出来")
G.request_json = fake

# 限流了要重试，不能整张表少几行
tries = {"n": 0}
def throttled(method, url, headers, body=None, timeout=30, form=None):
    if form["Action"] == "DescribeSpotPriceHistory":
        tries["n"] += 1
        if tries["n"] == 1:
            raise G.CloudError("HTTP 400：Request was denied due to request throttling.（Throttling）")
    return fake(method, url, headers, body, timeout, form)
G.request_json = throttled
G.time.sleep = lambda s: None
rows2 = A({"key_id":"AK","key_secret":"SK"}).price_table(lambda m: None)
assert len(rows2) == 4 and tries["n"] > 1
check("撞上限流会重试，不会静悄悄少几行（少行比慢几秒难查得多）")
G.request_json = fake

# ========== PPIO：价目跟产品表是同一份 ==========
def ppio_fake(method, url, headers, body=None, timeout=30, form=None):
    return {"data": [
        {"id":"prod-4090","name":"RTX4090-24G","region_ids":["cn-south-1"],
         "resource_spec":{"gpu":{"name":"RTX4090","max":8,"available":6},"cpu":16,"memory_gb":128},
         "pricing":{"precision":3,"postpaid":{"final_price":1980},"spot":{"final_price":880}}},
        {"id":"prod-a100","name":"A100-80G","region_ids":[],
         "resource_spec":{"gpu":{"name":"A100","max":8},"cpu":32,"memory_gb":256},
         "pricing":{"precision":3,"postpaid":{"final_price":12600},"spot":None}},
        {"id":"prod-none","name":"没价的","resource_spec":{},"pricing":{}}]}
G.request_json = ppio_fake
rows = G.PPIOPlatform("k").price_table(lambda m: None)
assert [r["spot"] for r in rows] == [0.88, 12.6], [r["spot"] for r in rows]
check("PPIO：有抢占价用抢占价，没有就用按量价，两档都没有的不列")
assert rows[0]["note"] == "可用 6 卡" and rows[0]["pick"] == {"product":"prod-4090"}
assert rows[1]["region"] == "自动调度"
check("PPIO 每行带库存和 product_id，没绑地域的写「自动调度」")

# ========== 窗口：数字列必须按数值排 ==========
app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
rows = [{"region":"cn-hangzhou","zone":"z1","spec":"A","gpu":"A10","vram":24,"n":1,"cpu":8,
         "mem":30,"spot":10.5,"origin":20.0,"note":"","pick":{"region":"cn-hangzhou","spec":"z1|A"}},
        {"region":"cn-beijing","zone":"z2","spec":"B","gpu":"A10","vram":24,"n":1,"cpu":8,
         "mem":30,"spot":9.9,"origin":20.0,"note":"","pick":{"region":"cn-beijing","spec":"z2|B"}},
        {"region":"cn-shanghai","zone":"z3","spec":"C","gpu":"T4","vram":16,"n":1,"cpu":4,
         "mem":15,"spot":2.75,"origin":20.0,"note":"","pick":{"region":"cn-shanghai","spec":"z3|C"}}]
d = G.PriceDialog(None, rows, "t")
col = [c[0] for c in G.PRICE_COLS].index("spot")
shown = [float(d.table.item(r, col).text()) for r in range(d.table.rowCount())]
assert shown == [2.75, 9.9, 10.5], shown
check("开窗就按抢占价升序：%s（字符串排的话 10.5 会排在 9.9 前面）" % shown)

d.table.sortItems(col, QtCore.Qt.DescendingOrder)
shown = [float(d.table.item(r, col).text()) for r in range(d.table.rowCount())]
assert shown == [10.5, 9.9, 2.75], shown
check("点表头能倒过来排，还是按数值")

save_col = [c[0] for c in G.PRICE_COLS].index("save")
assert d.table.item(0, save_col).text() == "省 48%"    # 10.5 / 20.0
check("「省」那栏按抢占价和按量价算出来（10.5 vs 20 → 省 48%）")

# 价格列要对齐显示，不能一列里 2.751 / 3.12 / 9.9 长短不一
d.table.sortItems(col, QtCore.Qt.AscendingOrder)
texts = [d.table.item(r, col).text() for r in range(d.table.rowCount())]
assert texts == ["2.750", "9.900", "10.500"], texts
check("价格按三位小数对齐显示，同时还是按数值排：%s" % texts)

# 抢占价比按量价还贵的时候要说「贵」，不是写个负号让人自己看
dear = G.PriceDialog(None, [{"region":"r","zone":"z","spec":"s","gpu":"g","vram":24,"n":1,
    "cpu":8,"mem":30,"spot":10.5,"origin":10.42,"note":"","pick":{}}], "t")
txt = dear.table.item(0, save_col).text()
assert txt == "贵 1%", txt
assert dear.table.item(0, save_col).foreground().color().name() == "#b91c1c"
check("抢占价反而更贵时写「贵 1%」并且标红（写成 -1% 太容易滑过去）")

# 排过序之后挑第一行，拿到的得是那一行，不是原始顺序的第一行
d.table.sortItems(col, QtCore.Qt.AscendingOrder)
d.table.setCurrentCell(0, 0)
d._pick()
assert d.picked["spec"] == "C" and d.picked["spot"] == 2.75, d.picked
check("排过序再挑：拿到的是屏幕上那一行（行号和原始顺序早就对不上了）")

# 小数显存（vGPU 折算出来的）也要按数值参与显示和筛选
FRAC = [{"region":"r","zone":"z","spec":"a","gpu":"T4","vram":4.0,"n":0.25,"cpu":4,
         "mem":16,"spot":1.0,"origin":2.0,"lat":None,"note":"","pick":{}},
        {"region":"r","zone":"z","spec":"b","gpu":"T4","vram":16,"n":1,"cpu":16,
         "mem":64,"spot":2.0,"origin":4.0,"lat":None,"note":"","pick":{}}]
dv = G.PriceDialog(None, FRAC, "t")
vcol = [c[0] for c in G.PRICE_COLS].index("vram")
assert [dv.table.item(r, vcol).text() for r in range(2)] == ["4", "16"]
dv.min_vram.setValue(8)
vis = [dv.rows[dv.table.item(r,0).data(QtCore.Qt.UserRole)]["spec"]
       for r in range(2) if not dv.table.isRowHidden(r)]
assert vis == ["b"], vis
check("小数显存（vGPU 折算的 4.0G）：显示成 4 不是 4.0，筛选也按数值")

# ========== 延迟：量不了的时候不能编 ==========
A._probe_latency = classmethod(lambda cls, r, tries=2, timeout=3.0:
    (None, "本机走着代理/TUN（oss-%s.aliyuncs.com 解析成假 IP 198.18.4.250），"
           "量到的是代理不是机房" % r))
G.request_json = fake
msgs = []
rows_nolat = A({"key_id":"AK","key_secret":"SK"}).price_table(msgs.append)
assert all(r["lat"] is None for r in rows_nolat)
assert any("量不了" in m and "代理" in m for m in msgs), msgs
check("延迟量不了时：每行是 None，日志说清为什么（不编一个看着像真的数）")

d2 = G.PriceDialog(None, rows_nolat, "t")
lat_col = [c[0] for c in G.PRICE_COLS].index("lat")
assert d2.table.item(0, lat_col).text() == "—"
tip = d2.findChild(QtWidgets.QLabel).text()
assert "延迟这一栏是空的" in tip and "代理" in tip, tip
check("窗口上直接写明延迟为什么是空的，不留一栏破折号让人猜")

A._probe_latency = classmethod(lambda cls, r, tries=2, timeout=3.0:
    ({"cn-hangzhou": 8, "cn-beijing": 25, "ap-southeast-1": 70}.get(r, 140), None))
rows_lat = A({"key_id":"AK","key_secret":"SK"}).price_table(lambda m: None)
by_region = {r["region"]: r["lat"] for r in rows_lat}
assert by_region["ap-southeast-1"] == 70 and by_region["cn-hangzhou"] == 8
check("量到了就按地域填进每一行（同地域的行共用一次测量，不是每行量一遍）")

# 代理不一定用 198.18 段。全都快得不像话时也要认出来——跨洲不可能 1ms
A._probe_latency = classmethod(lambda cls, r, tries=2, timeout=3.0: (1, None))
msgs = []
rows_fast = A({"key_id":"AK","key_secret":"SK"}).price_table(msgs.append)
assert all(r["lat"] is None for r in rows_fast), [r["lat"] for r in rows_fast]
assert any("跨洲不可能这么快" in m for m in msgs), msgs
check("量出来全在 3ms 以内：判成被代理截了，不报那一列假数")

# 连不上的那一路（本机有 TUN 连黑洞都能连上，只能靠打桩验这条分支）
import socket as _sock
_real = _sock.create_connection
def _boom(*a, **k): raise OSError("Connection refused")
_sock.create_connection = _boom
A._probe_latency = ORIG_PROBE
# 用一个不在假 IP 段里的地址，好让它真的走到 connect 那一步
_real_gai = _sock.getaddrinfo
_sock.getaddrinfo = lambda *a, **k: [(2, 1, 6, "", ("203.0.113.9", 443))]
ms, why = A._probe_latency("cn-hangzhou", tries=1, timeout=0.5)
_sock.getaddrinfo = _real_gai
_sock.create_connection = _real
assert ms is None and ("连不上" in why or "解析" in why or "代理" in why), (ms, why)
check("连不上时如实说，不把失败当成 0 毫秒")

A._probe_latency = classmethod(lambda cls, r, tries=2, timeout=3.0:
    ({"cn-hangzhou": 8, "cn-beijing": 25, "ap-southeast-1": 70}.get(r, 140), None))
rows_lat = A({"key_id":"AK","key_secret":"SK"}).price_table(lambda m: None)
by_region = {r["region"]: r["lat"] for r in rows_lat}
assert by_region["ap-southeast-1"] == 70 and by_region["cn-hangzhou"] == 8

d3 = G.PriceDialog(None, rows_lat, "t")
d3.table.sortItems(lat_col, QtCore.Qt.AscendingOrder)
lats = [int(d3.table.item(r, lat_col).text()) for r in range(d3.table.rowCount())]
assert lats == sorted(lats) and lats[0] == 8, lats
check("延迟列能按数值排：%s" % lats)

# ========== 筛选 ==========
FROWS = [
 {"region":"cn-hangzhou","zone":"z","spec":"vgpu","gpu":"vGPU8-2G","vram":2,"n":1,
  "cpu":4,"mem":16,"spot":0.165,"origin":1.1,"lat":8,"note":"","pick":{}},
 {"region":"us-west-1","zone":"z","spec":"L20","gpu":"NVIDIA L20","vram":48,"n":1,
  "cpu":16,"mem":128,"spot":1.937,"origin":19.4,"lat":150,"note":"","pick":{}},
 {"region":"cn-beijing","zone":"z","spec":"A10x4","gpu":"NVIDIA A10","vram":24,"n":4,
  "cpu":64,"mem":256,"spot":9.0,"origin":40.0,"lat":25,"note":"","pick":{}}]
d = G.PriceDialog(None, FROWS, "t")
def visible():
    return [d.rows[d.table.item(r,0).data(QtCore.Qt.UserRole)]["spec"]
            for r in range(d.table.rowCount()) if not d.table.isRowHidden(r)]
assert sorted(visible()) == ["A10x4","L20","vgpu"] and d.count_label.text() == "3 / 3 条"
check("不设门槛时全都看得见（3 / 3 条）")

d.min_vram.setValue(24)
assert sorted(visible()) == ["A10x4","L20"], visible()
assert d.count_label.text() == "2 / 3 条"
check("显存 ≥24G：2G 的 vGPU 切片被筛掉（这正是它最该挡的东西）")

d.min_cards.setValue(2)
assert visible() == ["A10x4"], visible()
check("再加卡数 ≥2：只剩四卡那台")

d.only_cn.setChecked(True)
assert visible() == ["A10x4"]
d.min_cards.setValue(0)
assert sorted(visible()) == ["A10x4"], visible()   # L20 在美西，被「只看国内」挡住
check("「只看国内」挡掉海外那条（延迟受不了的时候一键切回来）")

# 「国内」得按地域名认：腾讯云的国内地域是 ap-guangzhou，海外也是 ap- 开头
assert G.is_domestic("cn-hangzhou") and G.is_domestic("ap-guangzhou")
assert G.is_domestic("ap-shanghai") and G.is_domestic("ap-hongkong")
assert not G.is_domestic("ap-singapore") and not G.is_domestic("ap-tokyo")
assert not G.is_domestic("us-west-1") and not G.is_domestic("na-ashburn")
check("国内判据按地域名认：ap-guangzhou 算国内、ap-singapore 不算（按前缀会全算错）")

# 「只看有货」：默认勾上，取消能看到全部行情
STOCK = [{"region":"ap-shanghai","zone":"z","spec":"有货的","gpu":"T4","vram":16,"n":1,
          "cpu":10,"mem":32,"spot":1.7,"origin":8.7,"lat":None,"stock":True,
          "note":"有货","pick":{}},
         {"region":"ap-beijing","zone":"z","spec":"没货的","gpu":"A100","vram":"","n":8,
          "cpu":96,"mem":1024,"spot":45.2,"origin":226.1,"lat":None,"stock":False,
          "note":"无货（WithoutStock）","pick":{}}]
ds = G.PriceDialog(None, STOCK, "t")
def vis_s():
    return [ds.rows[ds.table.item(r,0).data(QtCore.Qt.UserRole)]["spec"]
            for r in range(2) if not ds.table.isRowHidden(r)]
assert ds.only_stock.isChecked() and vis_s() == ["有货的"], vis_s()
ds.only_stock.setChecked(False)
assert set(vis_s()) == {"没货的", "有货的"}, vis_s()   # 别用 sorted 比中文，码点顺序反直觉
check("「只看有货」默认勾上；取消之后没货的也看得见（看行情用）")

# 不报库存的平台（阿里云/PPIO）不能被这个筛没
dn = G.PriceDialog(None, FROWS, "t")
assert dn.has_stock is False          # 阿里云那批没有 stock 这一项
assert len([r for r in range(dn.table.rowCount()) if not dn.table.isRowHidden(r)]) == 3
check("不报库存的平台：这个筛选整个不显示，也不会把它们的行筛没")

# 这条筛选不能靠 isVisible() 判断（窗口没 show 时子控件全报不可见）
assert not ds.only_stock.isVisible() and ds.has_stock is True
ds.only_stock.setChecked(True)
assert vis_s() == ["有货的"], vis_s()
check("窗口还没显示时筛选照样生效（用显式标记，不用 isVisible）")

d.only_cn.setChecked(False); d.min_vram.setValue(0)
assert len(visible()) == 3
check("门槛调回去，被筛掉的还在（隐藏不是删除）")

# 选中的行被筛掉之后，不能还停在那一行上
d.table.selectRow([r for r in range(3)
                   if d.rows[d.table.item(r,0).data(QtCore.Qt.UserRole)]["spec"]=="vgpu"][0])
d.min_vram.setValue(24)
d._pick()
assert d.picked["spec"] != "vgpu", d.picked
check("选中的行被筛掉时自动跳到还看得见的一行，不会填回一条看不见的")

print("\n比价 %d 项全过" % len(ok))
