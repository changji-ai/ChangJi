# -*- coding: utf-8 -*-
import json, os, sys, tempfile, time as _t
os.environ["QT_QPA_PLATFORM"] = "offscreen"
# tests/ 的上一层就是 tools/，被测的模块在那儿
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import autodl_gui as G
from PySide6 import QtWidgets
ok=[]
def check(n): ok.append(n); print("  ✓", n)
tmp = tempfile.mkdtemp()
G.CONFIG_PATH = os.path.join(tmp, "cloud.json"); G.LEGACY_CONFIG = os.path.join(tmp, "l.json")
for k in ("AUTODL_TOKEN","PPIO_API_KEY","PPIO_TOKEN","ALIBABA_CLOUD_ACCESS_KEY_ID",
          "ALIBABA_CLOUD_ACCESS_KEY_SECRET","TENCENTCLOUD_SECRET_ID","TENCENTCLOUD_SECRET_KEY"):
    os.environ.pop(k, None)

G.save_config({"aliyun": {"key_id":"AK","key_secret":"SK"}})
G.save_recipe("阿里云", {"region":"cn-wulanchabu", "image":"m-1", "disk":300,
                       "password":"Changji#2026", "oss_src":"oss://b/models/"})
r = G.load_recipe("阿里云")
assert r["region"] == "cn-wulanchabu" and r["disk"] == 300
assert "password" not in r
check("配方存得下、密码不存（实例登录密码明文落盘不值当，每次现填就一栏）")

assert G.load_config()["aliyun"]["key_id"] == "AK"
check("存配方不会把钥匙冲掉")
assert oct(os.stat(G.CONFIG_PATH).st_mode)[-3:] == "600"
check("配置文件权限还是 600")

G.save_prefs(min_vram=32, min_cards=1)
assert G.load_prefs() == {"min_vram":32, "min_cards":1}
G.save_prefs(cn_only=True)
assert G.load_prefs()["min_vram"] == 32 and G.load_prefs()["cn_only"] is True
check("门槛（显存/卡数/只看国内）也存得下，而且是合并不是覆盖")

assert G.load_recipe("没存过的") == {}
check("没存过的平台返回空字典，不炸")

# ---- 窗口开起来要把配方套回表单 ----
G.AliyunPlatform.instances = lambda self: []
G.AliyunPlatform.load_choices = lambda self: {
    "spec": [("cn-wulanchabu-a · L20", "cn-wulanchabu-a|ecs.gn8is.4xlarge")],
    "image": [("[自定义] changji（260GB）", "m-1")]}
G.save_recipe("阿里云", {"region":"cn-wulanchabu", "image":"m-1", "disk":300,
                       "oss_src":"oss://changji-models/wan22/",
                       "oss_dest":"/root/models", "ram_role":"changji-oss-read",
                       "spec":"cn-wulanchabu-a|ecs.gn8is.4xlarge"})
app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
w = G.Window()
def pump(n=60):
    for _ in range(n): QtWidgets.QApplication.processEvents(); _t.sleep(0.02)
pump()
f = w.forms["阿里云"]
assert f.value("region") == "cn-wulanchabu"
assert f.value("disk") == 300
assert f.value("oss_src") == "oss://changji-models/wan22/"
assert f.value("ram_role") == "changji-oss-read"
check("开窗口就把上次填的套回去了（十几栏不用再敲一遍）")

# 下拉里原本没有的项也要能套上，等拉回来时还保得住
assert f.value("spec") == "cn-wulanchabu-a|ecs.gn8is.4xlarge"
w.switch_platform("阿里云"); pump(80)
assert f.value("spec") == "cn-wulanchabu-a|ecs.gn8is.4xlarge"
assert f.value("image") == "m-1"
check("配方里的规格/镜像在「拉规格」回来之后还在（set_choices 认得出同一个值）")

# ---- 扫所有平台 ----
G.save_config({"aliyun":{"key_id":"AK","key_secret":"SK"},
               "tencent":{"secret_id":"SID","secret_key":"SKEY"},
               "ppio":{"token":"tok"}})
w2 = G.Window(); pump()
names = [p_.NAME for p_ in w2.scannable()]
assert names == ["PPIO", "阿里云", "腾讯云"], names      # AutoDL 没价目接口
check("扫的是「能比价 + 钥匙填齐」的那几家（AutoDL 没价目接口，不算）")

G.PPIOPlatform.price_table = lambda self, log, regions=None: [
    {"region":"cn-south-1","zone":"","spec":"RTX4090","gpu":"RTX4090","vram":24,
     "n":1,"cpu":16,"mem":128,"spot":8.80,"origin":19.8,"note":"","pick":{"product":"p-1"}}]
G.AliyunPlatform.price_table = lambda self, log, regions=None: [
    {"region":"cn-wulanchabu","zone":"cn-wulanchabu-a","spec":"ecs.gn8is.4xlarge",
     "gpu":"NVIDIA L20","vram":48,"n":1,"cpu":16,"mem":128,"spot":2.379,"origin":15.857,
     "note":"","pick":{"region":"cn-wulanchabu","spec":"cn-wulanchabu-a|ecs.gn8is.4xlarge"}}]
def tc_boom(self, log, regions=None):
    raise G.CloudError("余额不足")
G.TencentPlatform.price_table = tc_boom
msgs = []
rows = w2.scan_prices(msgs.append)
assert [r["plat"] for r in rows] == ["阿里云", "PPIO"], rows   # 按价排
assert rows[0]["spot"] == 2.379
assert any("腾讯云" in m and "跳过" in m for m in msgs), msgs
check("四家合成一张表按价排；一家挂了说清楚并跳过，别家照常出")

# ---- 跨平台挑一条：自动切平台再填 ----
w2.switch_platform("PPIO"); pump(40)
w2.apply_pick(rows[0])
pump(40)
assert w2.plat.NAME == "阿里云"
assert w2.form.value("spec") == "cn-wulanchabu-a|ecs.gn8is.4xlarge"
assert w2.form.value("region") == "cn-wulanchabu"
check("挑中别家的一条：自动切到那个平台再填回去（不切的话填进了错的表单）")

# ---- 按钮是跨平台的，不随当前平台变灰 ----
for name in ("AutoDL", "PPIO", "阿里云", "腾讯云"):
    w2.switch_platform(name); pump(20)
    assert w2.cheap_btn.isEnabled(), name
    assert w2.price_btn.isEnabled(), name
check("「一键最便宜」和「比价」在哪个平台上都是亮的（扫的是所有家）")

G.save_config({})      # 钥匙全清掉
w3 = G.Window(); pump()
assert not w3.cheap_btn.isEnabled() and "填上钥匙" in w3.cheap_btn.toolTip()
check("一家钥匙都没填：按钮灰着并说清为什么")

print("\n配方 %d 项全过" % len(ok))
