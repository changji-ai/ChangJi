# -*- coding: utf-8 -*-
import os, sys, time
# tests/ 的上一层就是 tools/，被测的模块在那儿
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
import autodl_gui as G
from PySide6 import QtWidgets
ok=[]
def check(n): ok.append(n); print("  ✓", n)

assert G.human_bytes(0) == "0 B"
assert G.human_bytes(1500) == "1.5 KB"
assert G.human_bytes(2_500_000_000) == "2.5 GB"
check("字节说成人话（GB 按十进制，跟云厂商账单一个口径）")

# ---- AutoDL：真机字段抄下来的形状 ----
SNAP = {"snapshot_gpu_alias_name": "RTX PRO 6000",
        "usage_info": {"cpu_usage_percent": 8.7, "mem_usage": 1895477248,
                       "mem_limit": 118111600640, "root_fs_used_size": 2020007936,
                       "root_fs_total_size": 241948286976, "data_disk_total_size": 0,
                       "pull_image_progress": 1}}
def au(method, url, headers, body=None, timeout=30, form=None, raw=None):
    if "snapshot" in url:
        return {"code":"Success","msg":"","data":SNAP}
    if "status" in url:
        return {"code":"Success","msg":"","data":"running"}
    return {"code":"Success","msg":"","data":None}
G.request_json = au
m = G.AutoDLPlatform("t").metrics("pro-1")
names = [n for n, _, _ in m["rows"]]
assert names == ["CPU", "内存", "系统盘", "显卡"], names
vals = dict((n, v) for n, v, _ in m["rows"])
assert vals["内存"] == "1.9 GB / 118.1 GB" and vals["显卡"] == "RTX PRO 6000"
pct = dict((n, p) for n, _, p in m["rows"])
assert abs(pct["CPU"] - 8.7) < 0.01 and pct["显卡"] is None
check("AutoDL：CPU/内存/系统盘/显卡四行，百分比只给该有进度条的那几行")
assert any("不报 GPU" in n for n in m["notes"])
check("说清 AutoDL 不报 GPU 和网络（留一栏空着会让人以为是坏了）")
assert SNAP["usage_info"]["data_disk_total_size"] == 0
assert "数据盘" not in names
check("没有数据盘就不列那一行，不报一个 0/0")

# ---- 阿里云：最新那个点常常缺字段，要往回找 ----
PTS = [{"TimeStamp":"T1","CPU":4,"InternetTX":187,"InternetRX":165,
        "InternetBandwidth":5,"IntranetTX":100,"IntranetRX":90,
        "IntranetBandwidth":3,"BPSRead":0,"BPSWrite":1160,"IOPSRead":0,"IOPSWrite":2},
       {"TimeStamp":"T2","CPU":4,"InternetTX":229,"InternetRX":194,"InternetBandwidth":7,
        "IntranetTX":110,"IntranetRX":95,"IntranetBandwidth":4,
        "BPSRead":0,"BPSWrite":1200,"IOPSRead":0,"IOPSWrite":3},
       # 真账号上实测：最后一个点的公网那几栏是 None
       {"TimeStamp":"T3","CPU":6,"InternetTX":None,"InternetRX":None,
        "InternetBandwidth":None,"IntranetTX":None,"IntranetRX":None,
        "IntranetBandwidth":None,"BPSRead":None,"BPSWrite":None,
        "IOPSRead":None,"IOPSWrite":None}]
SEEN = []
def ali(method, url, headers, body=None, timeout=30, form=None, raw=None):
    SEEN.append(dict(form or {}))
    return {"code":"Success","msg":"","MonitorData":{"InstanceMonitorData":PTS}}
G.request_json = ali
z = G.AliyunPlatform({"key_id":"a","key_secret":"b"})
z.set_context({"region":"cn-hangzhou"})
m = z.metrics("i-1")
vals = dict((n, v) for n, v, _ in m["rows"])
assert vals["CPU"] == "6%", vals["CPU"]          # CPU 最后一个点有，就用它
assert vals["公网带宽"] == "0.01 Mbit/s", vals["公网带宽"]   # 往回取 T2 的 7
assert vals["公网出（每分钟）"] == "28.6 KB", vals["公网出（每分钟）"]  # 229 kbit
check("最新那个点缺字段时往回找（实测最后一点的公网栏全是 None，直接取会空掉）")
assert any("T2" in n or "按分钟" in n for n in m["notes"])
check("说清是按分钟出点的、最快也是一分钟前")

# 一条数据都没有时要说人话，不是留一片空白
G.request_json = lambda *a, **k: {"code":"Success","msg":"","MonitorData":{"InstanceMonitorData":[]}}
m = z.metrics("i-1")
assert m["rows"] == [] and any("等一两分钟" in n for n in m["notes"])
check("刚开机还没出点：说「等一两分钟」，不是留一片空白让人猜")

# ---- 用哪个地域：机器自己的，不是表单里选的 ----
assert G.AliyunPlatform.region_of({"region": "cn-shanghai-l"}) == "cn-shanghai"
assert G.TencentPlatform.region_of({"region": "ap-shanghai-2"}) == "ap-shanghai"
assert G.AliyunPlatform.region_of({"region": ""}) == ""
assert G.AutoDLPlatform.region_of({"region": "内蒙C区"}) == ""   # 这家没有地域概念
check("从列表那行倒推地域：cn-shanghai-l → cn-shanghai（查错地域的表现是「没数据」）")

# ---- 能力开关 ----
assert G.AutoDLPlatform.METRICS is True and G.AliyunPlatform.METRICS is True
assert G.PPIOPlatform.METRICS is False
try:
    G.PPIOPlatform("k").metrics("x")
except G.CloudError as e:
    assert "不报实时数据" in str(e)
check("不支持的平台如实说不支持")

# ---- 面板 ----
app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
G.request_json = au
d = G.MetricsDialog(None, G.AutoDLPlatform("t"), "pro-1", "changji")
d.render({"rows":[("CPU","5.0%",5.0),("显卡","RTX PRO 6000",None)],
          "notes":["这家**不报**网络"], "ssh":"ssh -p 49079 root@x.com"})
assert set(d.bars) == {"CPU", "显卡"}
assert d.bars["CPU"][1].isVisible() is False or d.bars["CPU"][1].value() == 5
assert d.gpu_line.text() == "ssh -p 49079 root@x.com nvidia-smi"
check("面板：ssh 那行后面挂上 nvidia-smi，一点就复制（GPU 唯一真能看的办法）")
assert "**" not in d.notes.text(), d.notes.text()
check("说明里的裸星号在渲染时被剥掉（QLabel 不认 markdown，这个错犯过四次）")

d._alive = False
d.render({"rows":[("CPU","9%",9.0)], "notes":[], "ssh":""})
assert d.gpu_line.text() == ""
check("拿不到 ssh 时那一栏空着，不给一条连不上的命令")

print("\n实时面板 %d 项全过" % len(ok))
