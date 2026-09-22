# -*- coding: utf-8 -*-
import os, sys, tempfile, time
os.environ["QT_QPA_PLATFORM"] = "offscreen"
# tests/ 的上一层就是 tools/，被测的模块在那儿
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
tmp = tempfile.mkdtemp()
import autodl_gui as G
G.CONFIG_PATH = os.path.join(tmp, "cloud.json")
G.LEGACY_CONFIG = os.path.join(tmp, "autodl.json")
for k in ("AUTODL_TOKEN","PPIO_API_KEY","PPIO_TOKEN"): os.environ.pop(k, None)
from PySide6 import QtWidgets, QtCore, QtGui
from PySide6.QtTest import QTest


def wait(ms):
    """QTest.qWait 不放 GIL，主线程在里面空转时后台线程根本跑不动（实测 worker
    的第一次 emit 要等 qWait 结束之后才回来）。自己转：processEvents 收信号，
    time.sleep 让出 GIL。"""
    end = time.time() + ms / 1000.0
    while time.time() < end:
        QtWidgets.QApplication.processEvents(QtCore.QEventLoop.AllEvents, 10)
        time.sleep(0.01)

ok=[]
def check(n): ok.append(n); print("  ✓", n)

A, P = G.AutoDLPlatform, G.PPIOPlatform
A.instances = lambda self: [
 {"id":"pro-76576c61fdf1","name":"changji 出片","status":"running","spec":"pro6000-p",
  "gpu":1,"billing":"按量","region":"内蒙C区","created":"2026-09-18 17:30:54"},
 {"id":"pro-759127a8714f","name":"changji 首帧","status":"shutdown","spec":"v-48g",
  "gpu":2,"billing":"按量","region":"西北B区","created":"2026-09-17 09:12:03"}]
P.instances = lambda self: [
 {"id":"ca338f12f3","name":"changji spot","status":"running","spec":"prod-4090-24g",
  "gpu":4,"billing":"spot","region":"cn-south-1","created":"2026-09-18 21:04:11"}]
A.load_choices = lambda self: {"image":[("[私有] changji-wan-5b","image-3d0217ce85")] + A.IMAGES}
P.load_choices = lambda self: {
 "product":[("RTX4090-24G 24G 可用6卡 按量¥1.98/时 抢占¥0.88/时","prod-4090-24g"),
            ("A100-80G 80G 可用2卡 按量¥12.60/时 抢占¥6.00/时","prod-a100-80g")],
 "image":[("pytorch:2.3.0-cuda12.1","registry.ppio.cn/base/pytorch:2.3.0-cuda12.1")]}
A.detail = lambda self,i: (["ssh -p 34222 root@connect.westb.seetacloud.com",
    "密码 jbeOXgTWUxq+"], "ssh -p 34222 root@connect.westb.seetacloud.com")
created = {}
A.create = lambda self,v: created.setdefault("v", v) and None or "pro-新建的"
def rel(self, iid, log, wait_s=180):
    log("当前状态：running"); log("AutoDL 释放前必须先关机，替你关了……")
    log("  …… shutdown"); log("已关机。"); log("已释放：" + iid)
A.release_safely = rel
G.save_config({"autodl":"demo-token","ppio":"demo-key"})

app = QtWidgets.QApplication([])
w = G.Window(); w.show(); wait(600)

def logs(): return w.logbox.toPlainText().splitlines()
def ids(): return [w.table.item(r,0).text() for r in range(w.table.rowCount())]

# ---- 开局 AutoDL ----
assert w.plat.NAME == "AutoDL"
assert w.form.order == ["spec","gpu_num","disk","image","cuda","regions","name","command"]
assert w.form.value("spec") == "pro6000-p"       # 下拉显示人话，交出去的是 ID
assert w.form.value("cuda") == 118
assert w.form.value("gpu_num") == 1
assert ids() == ["pro-76576c61fdf1","pro-759127a8714f"], ids()
assert w.table.item(0,5).text() == "按量" and w.table.item(1,2).text() == "shutdown"
check("开局 AutoDL：八个字段、下拉翻 ID、表里两台机器八栏都对")

# 关机的那台状态是灰的，开着的不是
assert w.table.item(1,2).foreground().color().name() == "#9aa3ad"
assert w.table.item(0,2).foreground().color() != w.table.item(1,2).foreground().color()
check("关机的那台状态涂灰，扫一眼就知道哪台在烧钱")

# ---- 切 PPIO ----
w.switch_platform("PPIO"); wait(600)
assert w.form.order == ["product","gpu_num","disk","image","billing","name","command"]
assert w.form.value("billing") == "postpaid"
assert w.form.value("disk") == 50                 # PPIO 系统盘不能填 0
assert w.form.value("product") == "prod-4090-24g" # 自动拉了规格并选上第一个
assert ids() == ["ca338f12f3"]
assert w.create_box.title() == "新建实例 · PPIO"
assert "抢占" in w.hint.text()
check("切 PPIO：表单整块换成七个字段，规格自动拉回来，表和提示语都跟着换")

# ---- 可编辑下拉：粘 ID 也认 ----
w.form.widgets["image"].setEditable(True)
w.form.widgets["image"].setCurrentText("registry.自己粘的/img:tag")
assert w.form.value("image") == "registry.自己粘的/img:tag"
check("镜像下拉可编辑：粘 docker 地址也认")

# ---- 慢回包：拉 PPIO 的时候切回 AutoDL，PPIO 的机器不许画上去 ----
def slow(self):
    time.sleep(1.2)
    return [{"id":"晚到的-PPIO","name":"x","status":"running","spec":"s","gpu":1,
             "billing":"spot","region":"r","created":"c"}]
P.instances = slow
w.switch_platform("PPIO")          # 这一次的 instances 会慢 1.2 秒
wait(150)
w.switch_platform("AutoDL")        # 用户切走了
wait(2500)
assert "晚到的-PPIO" not in ids(), ids()
assert w.plat.NAME == "AutoDL" and w.form.order[0] == "spec"
assert w.create_box.title() == "新建实例 · AutoDL"
check("PPIO 的慢回包到得晚：用户已切回 AutoDL，不画上去，表单标题也没串")

# ---- 详情：日志 + 剪贴板 ----
w.table.selectRow(0)
w.on_detail(); wait(500)
assert any("ssh -p 34222" in l for l in logs())
assert any("SSH 命令已复制" in l for l in logs())
assert "ssh -p 34222" in QtGui.QGuiApplication.clipboard().text()
check("详情：SSH 和密码进日志，命令进剪贴板")

# ---- 释放：过程日志一行一行发回来 ----
w.table.selectRow(0)
before = len(logs())
w.run_bg(lambda log: rel(w.plat, "pro-76576c61fdf1", log), lambda _r: None)
wait(600)
tail = logs()[before:]
assert any("释放前必须先关机" in l for l in tail), tail
assert any("已释放：pro-76576c61fdf1" in l for l in tail), tail
check("释放：关机、等、释放的过程一行不落地回到日志")

# ---- 没选机器就点动作 ----
w.table.clearSelection(); w.table.setCurrentCell(-1, -1)
before = len(logs()); w.on_power_on(); wait(100)
assert "先在列表里点一台" in logs()[-1]
check("没选机器就点开机：提示先选一台，不是静悄悄什么都不发生")

# ---- 空的必填项：确认框弹出来之前就拦住 ----
w.switch_platform("PPIO"); wait(600)
w.form.widgets["product"].clear()
before = len(logs()); w.on_create(); wait(200)
assert "算力规格没填" in logs()[-1], logs()[-1]
check("规格没填就点创建：当场拦住，不会先弹「确定要创建吗」再报错")

# ---- 换一次平台只干一套活，不是两套 ----
# 上面那条测慢回包时把 P.instances 换成了睡 1.2 秒的版本，换回来，
# 不然这条量的是「等够了没」而不是「跑了几遍」。
P.instances = lambda self: [
 {"id":"ca338f12f3","name":"changji spot","status":"running","spec":"prod-4090-24g",
  "gpu":4,"billing":"spot","region":"cn-south-1","created":"2026-09-18 21:04:11"}]
w.switch_platform("AutoDL"); wait(600)
n0 = len(logs())
w.switch_platform("PPIO"); wait(900)
new = logs()[n0:]
assert new.count("── PPIO ──") == 1 or len([l for l in new if "── PPIO ──" in l]) == 1, new
assert len([l for l in new if "PPIO：1 台" in l]) == 1, new
check("换一次平台只拉一次列表（setChecked 会再触发一次 toggled，挡住了）")

# ---- 表单一个平台一页，不会几层叠在一起 ----
assert w.stack.count() == len(G.PLATFORMS)   # 别写死条数，加一家就过期一次
assert w.stack.currentIndex() == w.page_of["PPIO"]
vis = [c for c in w.stack.currentWidget().findChildren(QtWidgets.QComboBox)]
assert len(vis) == 3, [c.objectName() for c in vis]   # product / image / billing
w.switch_platform("AutoDL"); wait(600)
assert w.stack.currentIndex() == w.page_of["AutoDL"]
assert len(w.stack.currentWidget().findChildren(QtWidgets.QComboBox)) == 3  # spec/image/cuda
check("一个平台一页：当前页上只有这个平台的控件，旧表单不会叠着没走")

# ---- 两边各填一半，切走再切回来还在 ----
w.form.widgets["name"].setText("我填的 AutoDL 名字")
w.switch_platform("PPIO"); wait(700)
w.form.widgets["name"].setText("我填的 PPIO 名字")
w.switch_platform("AutoDL"); wait(700)
assert w.form.value("name") == "我填的 AutoDL 名字"
w.switch_platform("PPIO"); wait(700)
assert w.form.value("name") == "我填的 PPIO 名字"
check("两边各填各的：切走再切回来，各自填的还在")

# ---- 阿里云：钥匙两段、地域一改就重拉 ----
assert [b.text() for b in w.tabs.values()] == [p.NAME for p in w.platforms]
assert len(w.cred_edits["AutoDL"]) == 1 and len(w.cred_edits["阿里云"]) == 2
assert set(w.cred_edits["阿里云"]) == {"key_id", "key_secret"}
for edits in w.cred_edits.values():
    for e in edits.values():
        assert e.echoMode() == QtWidgets.QLineEdit.Password
check("阿里云的钥匙是两个框（ID + Secret），而且全都是密码框不明文显示")

pulls = []
G.AliyunPlatform.instances = lambda self: []
# 桩里**必须也返回 region**：会自己触发自己的正是「重填地域下拉」那一下，
# 不返回的话这条用例根本没碰到要测的东西（拆掉守卫也照样绿，验过）。
G.AliyunPlatform.load_choices = lambda self: (
    pulls.append(self.region) or {
        "region": [("华东1（杭州）", "cn-hangzhou"), ("华北2（北京）", "cn-beijing")],
        "spec": [("%s-k · A10 ×1" % self.region, "%s-k|ecs.gn7i" % self.region)]})
w.platforms[2].creds = {"key_id": "AK", "key_secret": "SK"}
w.switch_platform("阿里云"); wait(800)
assert pulls == ["cn-hangzhou"], pulls          # 换过去自动拉了一次
assert w.form.value("spec") == "cn-hangzhou-k|ecs.gn7i"
check("换到阿里云：按默认地域自动拉了一次规格")

combo = w.form.widgets["region"]
combo.setCurrentIndex(combo.findData("cn-beijing"))
wait(900)
assert pulls == ["cn-hangzhou", "cn-beijing"], pulls
assert w.form.value("spec") == "cn-beijing-k|ecs.gn7i"   # 规格跟着换了地域
check("改地域 → 规格/镜像那几张表自动重拉（地域变了旧表就是错的）")

# set_choices 会重填下拉，重填又会触发 change —— 挡住了才不会转成死循环
n_before = len(pulls)
wait(1500)
assert len(pulls) == n_before, "重填下拉触发了新一轮重拉：%r" % pulls
check("重填下拉没把自己再触发一遍（不挡信号这里会无限拉下去）")

assert w.platforms[2].region == "cn-beijing"
check("表单上选的地域推给了平台（阿里云所有请求都要按地域发）")

w.switch_platform("AutoDL"); wait(600)

# ---- 「出网24h」这一栏只在报流量的平台上出现 ----
TCOL = [c[0] for c in G.COLUMNS].index("traffic")
G.AliyunPlatform.instances = lambda self: [
    {"id":"i-a","name":"跑着的","status":"Running","spec":"ecs.gn8is.4xlarge",
     "gpu":1,"billing":"抢占","region":"cn-wulanchabu-a",
     "created":"2026-09-19 09:00:00","traffic":1.2},
    {"id":"i-b","name":"没查到的","status":"Stopped","spec":"ecs.gn8is.4xlarge",
     "gpu":1,"billing":"抢占","region":"cn-wulanchabu-a",
     "created":"2026-09-18 09:00:00","traffic":None}]
w.switch_platform("阿里云"); wait(800)
assert not w.table.isColumnHidden(TCOL)
assert w.table.item(0, TCOL).text() == "1.20 GB", w.table.item(0, TCOL).text()
assert w.table.item(1, TCOL).text() == "—"
check("阿里云：出网24h 这一栏在，查到的写 GB，没查到写「—」")

w.switch_platform("AutoDL"); wait(600)
assert w.table.isColumnHidden(TCOL)
w.switch_platform("PPIO"); wait(600)
assert w.table.isColumnHidden(TCOL)
w.switch_platform("阿里云"); wait(700)
assert not w.table.isColumnHidden(TCOL)
check("AutoDL / PPIO 不报流量：整列藏起来，不留一列破折号")

# ---- 第四家：腾讯云 ----
G.TencentPlatform.instances = lambda self: [
    {"id":"ins-rn79mzt1","name":"changji 出片","status":"RUNNING",
     "spec":"GN10Xp.2XLARGE40","gpu":1,"billing":"竞价","region":"ap-shanghai-2",
     "created":"2026-09-19 10:00:00","traffic":None}]
G.TencentPlatform.load_choices = lambda self: {
    "spec":[("ap-shanghai-2 · V100 ×1 · 10C40G · GN10Xp.2XLARGE40",
             "ap-shanghai-2|GN10Xp.2XLARGE40")]}
w.platforms[3].creds = {"secret_id":"SID","secret_key":"SKEY"}
w.switch_platform("腾讯云"); wait(800)
assert w.plat.NAME == "腾讯云"
assert w.form.order == ["region","spec","image","sg","subnet","disk","disk_type",
                        "strategy","price","bandwidth","name","password",
                        "cos_src","cos_dest","cam_role"], w.form.order
assert set(w.cred_edits["腾讯云"]) == {"secret_id","secret_key"}
assert w.form.value("bandwidth") == 100 and w.form.value("disk") == 100
assert [w.table.item(0,c).text() for c in (0,2,5)] == ["ins-rn79mzt1","RUNNING","竞价"]
check("腾讯云那一页：十二个字段、SecretId+SecretKey 两段钥匙、带宽默认拉满")

TC = [c[0] for c in G.COLUMNS].index("traffic")
assert w.table.isColumnHidden(TC)        # 这家不报流量
assert w.price_btn.isEnabled()           # 但支持比价
check("腾讯云：流量那列藏起来（接口口径没核准），比价按钮是亮的")

# 密码格式要求各家不同，占位文字得跟着各家走
tc_hint = w.form.widgets["password"].placeholderText()
w.switch_platform("阿里云"); wait(700)
ali_hint = w.form.widgets["password"].placeholderText()
assert "两类" in tc_hint and "三类" in ali_hint, (tc_hint, ali_hint)
check("密码框的提示按各家要求写：腾讯云「至少两类」、阿里云「四类占三类」")
w.switch_platform("腾讯云"); wait(700)

assert [b.text() for b in w.tabs.values()] == ["AutoDL","PPIO","阿里云","腾讯云"]
check("四家都在顶栏上")

# ---- 长选项要显示开头，不是尾巴 ----
w.switch_platform("阿里云"); wait(700)
G.AliyunPlatform.load_choices = lambda self: {"spec": [
    ("cn-wulanchabu-a · NVIDIA L20 ×1 · 16C128G · ecs.gn8is.4xlarge",
     "cn-wulanchabu-a|ecs.gn8is.4xlarge")]}
w.on_fetch(); wait(700)
cb = w.form.widgets["spec"]
assert cb.lineEdit().cursorPosition() == 0, cb.lineEdit().cursorPosition()
assert cb.toolTip().startswith("cn-wulanchabu-a")     # 悬停能看全
check("长选项把光标留在开头：看得见可用区和卡型号，不是只剩一截规格名尾巴")

# 下拉不许把左栏撑宽
assert cb.sizeAdjustPolicy() == QtWidgets.QComboBox.AdjustToMinimumContentsLengthWithIcon
labels = [w.form.widgets[k] for k in w.form.order]
assert all(x.width() <= w.create_box.width() for x in labels)
check("长选项不把左栏撑宽（撑宽了标签会被挤到上一行去）")

w.switch_platform("AutoDL"); wait(600)

# ---- 界面上一个裸星号都不许有 ----
# Qt 的 QLabel 不认 markdown，写 ** 就真的显示两个星号。这个错犯过三次
# （PPIO 的提示语、阿里云的提示语、镜像窗口的说明），每次都是截图才看见，
# 所以扫一遍所有给人看的字。
bad = []
for cls in G.PLATFORMS:
    for attr in ("BILLING_HINT", "TOKEN_HINT"):
        if "**" in (getattr(cls, attr, "") or ""):
            bad.append("%s.%s" % (cls.NAME, attr))
for p_ in w.platforms:
    for f in p_.fields():
        if "**" in (f.label + f.hint):
            bad.append("%s 的字段 %s" % (p_.NAME, f.key))
for name in ("阿里云", "腾讯云", "AutoDL", "PPIO"):
    w.switch_platform(name); wait(400)
    for lb in w.findChildren(QtWidgets.QLabel):
        if "**" in lb.text():
            bad.append("%s 界面上的 %r" % (name, lb.text()[:40]))
# 两个弹窗要拿变量接住：不接的话 Python 这头一回收，C++ 那头的控件就没了，
# 再去读它的文字会炸 "Internal C++ object already deleted"
_img_dlg = G.ImageDialog(None, [], w.platforms[2], "")
_price_dlg = G.PriceDialog(None, [{"region":"r","zone":"z","spec":"s","gpu":"g","vram":8,
    "n":1,"cpu":1,"mem":1,"spot":1.0,"origin":2.0,"lat":None,"note":"","pick":{}}], "t")
dlg_lbls = (_img_dlg.findChildren(QtWidgets.QLabel)
            + _price_dlg.findChildren(QtWidgets.QLabel))
for lb in dlg_lbls:
    if "**" in lb.text():
        bad.append("弹窗上的 %r" % lb.text()[:40])
assert not bad, "这些地方有裸星号（QLabel 不认 markdown）：%s" % bad
check("界面上所有给人看的字里一个裸星号都没有（这个错犯过三次，钉住）")
w.switch_platform("AutoDL"); wait(400)

# ---- 窗口得压得下去 ----
#
# 2026-09-20 用户报「窗口太高了，下面看不到」。原因是左栏 15 个字段 + 写死
# 190 的日志框把**最小高度**顶到 1103——resize() 根本没生效，Qt 被内容撑着。
# 钉的是「最小高度」这个数，不是布局长什么样：布局以后还会改，而"能不能
# 在小屏幕上用"是不能退的。
for name in ("AutoDL", "PPIO", "阿里云", "腾讯云"):
    w.switch_platform(name); wait(300)
    mh = w.minimumSizeHint().height()
    assert mh <= 480, "%s 那一页把窗口最小高度顶到 %d 了" % (name, mh)
check("四家的最小窗口高度都压在 480 以内（原来阿里云那页是 1103）")

w.resize(1240, 560); wait(300)
assert w.height() == 560, "resize 没生效，被内容顶着：%d" % w.height()
check("能压到 560 并且真生效（resize 被内容顶掉的话这条就红）")

# 最常按的那几个不许藏在滚动条后面
for btn, why in ((w.cheap_btn, "一键最便宜"), (w.price_btn, "比价"),
                 (w.image_btn, "自定义镜像"), (w.boot_btn, "开机脚本")):
    top = btn.mapTo(w, QtCore.QPoint(0, 0)).y()
    assert 0 <= top and top + btn.height() <= w.height(), \
        "%s 掉到窗口外面去了（y=%d 高=%d 窗口高=%d）" % (why, top, btn.height(), w.height())
check("窗口压到 560 时，一键最便宜/比价/镜像/开机脚本四个按钮还都在窗口里")

# 提示语默认收着——它有六七行，常驻就把左栏顶高了
assert not w.hint.isVisible() or not w.hint_btn.isChecked()
w.hint_btn.setChecked(True); wait(200)
h_open = w.minimumSizeHint().height()
w.hint_btn.setChecked(False); wait(200)
assert h_open <= 480, "提示语展开把最小高度顶到 %d" % h_open
check("那段六七行的提示语默认收着，展开也不会把窗口顶高（它在滚动区里）")

w.resize(1240, 780); wait(200)

# ---- 截图留档 ----
w.switch_platform("阿里云"); wait(800)
w.resize(1380, 880); wait(200)
shot = "/tmp/qt_ui.png"
w.grab().save(shot)
print("\n截图：", shot)
print("Qt 层 %d 项全过" % len(ok))
