# -*- coding: utf-8 -*-
import json, os, sys, tempfile
os.environ["QT_QPA_PLATFORM"] = "offscreen"
# tests/ 的上一层就是 tools/，被测的模块在那儿
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import autodl_gui as G
from PySide6 import QtWidgets
ok=[]
def check(n): ok.append(n); print("  ✓", n)
A, T, D = G.AliyunPlatform, G.TencentPlatform, G.AutoDLPlatform

SENT = []
def ali(method, url, headers, body=None, timeout=30, form=None, raw=None):
    SENT.append(dict(form or {}))
    act = form["Action"]
    if act == "DescribeImages":
        if form.get("ImageOwnerAlias") == "self":
            return {"code":"Success","msg":"","data":None,
                    "Images":{"Image":[{"ImageId":"m-1","ImageName":"changji-models-20260919",
                                        "Size":200,"Status":"Available"}]}}
        return {"Images":{"Image":[{"ImageId":"ubuntu_22","ImageName":"Ubuntu 22.04",
                                    "OSName":"Ubuntu"}]}}
    if act == "CreateImage":
        return {"ImageId":"m-new"}
    if act == "CopyImage":
        return {"ImageId":"m-copy-" + form["DestinationRegionId"]}
    return {}
G.request_json = lambda *a, **k: ali(*a, **k)

p = A({"key_id":"a","key_secret":"b"}); p.set_context({"region":"cn-wulanchabu"})
imgs = p.list_images()
assert imgs == [{"id":"m-1","name":"changji-models-20260919","size":200,
                 "status":"Available","region":"cn-wulanchabu"}], imgs
check("阿里云：列得出自定义镜像（ImageOwnerAlias=self）")

SENT[:] = []
assert p.save_image("i-1", "changji-models") == "m-new"
assert SENT[0]["Action"] == "CreateImage" and SENT[0]["InstanceId"] == "i-1"
assert SENT[0]["ImageName"] == "changji-models" and SENT[0]["RegionId"] == "cn-wulanchabu"
check("阿里云：CreateImage 参数对（实例、名字、地域）")

SENT[:] = []
logs = []
out = p.copy_image("m-1", ["cn-shenzhen", "us-west-1"], logs.append)
assert [x["DestinationRegionId"] for x in SENT] == ["cn-shenzhen","us-west-1"]
assert out == [("cn-shenzhen","m-copy-cn-shenzhen"), ("us-west-1","m-copy-us-west-1")]
check("阿里云：一个目标地域一次 CopyImage（同地域同时最多 5 个，多了排队）")

# 一个地域复制失败不该把别的带走
def ali_partial(method, url, headers, body=None, timeout=30, form=None, raw=None):
    if form["Action"] == "CopyImage" and form["DestinationRegionId"] == "cn-shenzhen":
        raise G.CloudError("OperationDenied.ImageCopyConflict")
    return ali(method, url, headers, body, timeout, form, raw)
G.request_json = ali_partial
logs = []
out = p.copy_image("m-1", ["cn-shenzhen", "us-west-1"], logs.append)
assert out == [("us-west-1","m-copy-us-west-1")]
assert any("没发起来" in l and "cn-shenzhen" in l for l in logs), logs
check("一个地域复制失败：说清是哪个，别的照常发")
G.request_json = lambda *a, **k: ali(*a, **k)

# 自定义镜像要排在镜像下拉最前面 —— 不然烤好的镜像根本选不到
ch = p.load_choices()
assert ch["image"][0][1] == "m-1", ch["image"][:2]
assert ch["image"][0][0].startswith("[自定义]") and "200GB" in ch["image"][0][0]
check("自定义镜像排在镜像下拉第一个（这是最早那个缺口：做好了也选不到）")

# 系统盘小于镜像 → 建机必失败，本地先拦住并且把该填多少说出来
try:
    p.validate({"spec":"cn-wulanchabu-a|ecs.gn8is.4xlarge","image":"m-1","sg":"s","vsw":"v","disk":100,
                "disk_cat":"cloud_essd","strategy":"SpotAsPriceGo","price":"",
                "bandwidth":100,"name":"x","password":"Changji#2026"})
except G.CloudError as e:
    assert "装不下" in str(e) and "200" in str(e), e
    check("选了 200GB 的镜像却只给 100GB 系统盘：当场拦住并说该调到多少")
else:
    raise AssertionError("系统盘装不下镜像却放行了")
p.validate({"spec":"cn-wulanchabu-a|ecs.gn8is.4xlarge","image":"m-1","sg":"s","vsw":"v","disk":300,
            "disk_cat":"cloud_essd","strategy":"SpotAsPriceGo","price":"",
            "bandwidth":100,"name":"x","password":"Changji#2026"})
check("系统盘够大就放行")
# 公共镜像没记大小，不能因为不知道就拦住
p.validate({"spec":"cn-wulanchabu-a|ecs.gn8is.4xlarge","image":"ubuntu_22","sg":"s","vsw":"v","disk":40,
            "disk_cat":"cloud_essd","strategy":"SpotAsPriceGo","price":"",
            "bandwidth":100,"name":"x","password":"Changji#2026"})
check("不知道大小的镜像（公共镜像）不拦——不知道不等于不行")

# ---- 腾讯云 ----
TSENT = []
def tc(method, url, headers, body=None, timeout=30, form=None, raw=None):
    b = json.loads(raw); act = headers["X-TC-Action"]
    TSENT.append((act, b))
    if act == "DescribeImages":
        kind = b["Filters"][0]["Values"][0]
        if kind == "PRIVATE_IMAGE":
            return {"Response":{"ImageSet":[{"ImageId":"img-mine","ImageName":"changji",
                                             "ImageSize":200,"ImageState":"NORMAL"}]}}
        return {"Response":{"ImageSet":[{"ImageId":"img-pub","ImageName":"Ubuntu 22.04",
                                         "OsName":"Ubuntu"}]}}
    if act == "CreateImage":
        return {"Response":{"ImageId":"img-new"}}
    return {"Response":{}}
G.request_json = tc
q = T({"secret_id":"a","secret_key":"b"}); q.set_context({"region":"ap-shanghai"})
assert q.list_images()[0]["id"] == "img-mine"
check("腾讯云：列得出私有镜像（image-type=PRIVATE_IMAGE）")
TSENT[:] = []
assert q.save_image("ins-1", "changji") == "img-new"
act, b = TSENT[0]
assert act == "CreateImage" and b["InstanceId"] == "ins-1" and b["ImageName"] == "changji"
assert b["ForcePoweroff"] == "TRUE"
check("腾讯云：CreateImage 带 ForcePoweroff（不关机做出来的镜像里模型可能是半截的）")
TSENT[:] = []
logs = []
q.copy_image("img-mine", ["ap-guangzhou","ap-beijing"], logs.append)
act, b = TSENT[0]
assert act == "SyncImages" and b == {"ImageIds":["img-mine"],
                                     "DestinationRegions":["ap-guangzhou","ap-beijing"]}
check("腾讯云：SyncImages 一次带上全部目标地域")

# ---- AutoDL：接口早就有，之前只喂下拉没暴露出来 ----
def au(method, url, headers, body=None, timeout=30, form=None, raw=None):
    if "image/save" in url:
        return {"code":"Success","msg":"","data":{"image_uuid":"image-new"}}
    return {"code":"Success","msg":"","data":{"list":[
        {"image_uuid":"image-1","name":"changji","image_size":56125440,
         "status":"finished"}]}}
G.request_json = au
d = D("tok")
assert d.list_images()[0]["id"] == "image-1"
assert d.save_image("pro-1", "changji") == "image-new"
check("AutoDL：存镜像的接口早就有，这次接到界面上了")

# ---- 能力开关 ----
# 一律用 is True 比，不用真值——**非空列表也是真值**，当初就是这么让一个
# 名字冲突（AutoDL 的 IMAGES 本来是公共镜像表）一路绿着混过去的
for cls in (D, A, T):
    assert cls.CAN_IMAGE is True, cls.NAME
assert G.PPIOPlatform.CAN_IMAGE is False
assert A.CAN_COPY_IMAGE is True and T.CAN_COPY_IMAGE is True
assert D.CAN_COPY_IMAGE is False
assert isinstance(D.IMAGES, list)      # AutoDL 的 IMAGES 还是那张公共镜像表
try:
    G.PPIOPlatform("k").save_image("i", "n")
except G.CloudError as e:
    assert "不支持" in str(e)
check("不支持的平台如实说不支持（PPIO 没接，AutoDL 不能跨地域复制）")

# ---- 窗口 ----
app = QtWidgets.QApplication.instance() or QtWidgets.QApplication([])
rows = [{"id":"m-1","name":"changji-models-20260919","size":200,
         "status":"Available","region":"cn-wulanchabu"}]
dlg = G.ImageDialog(None, rows, A({"key_id":"a","key_secret":"b"}), "i-abc")
assert dlg.save_btn.isEnabled() and dlg.copy_btn.isEnabled()
assert dlg.table.item(0,0).text() == "m-1"
check("镜像窗口：列出来了，选了机器时「做镜像」是亮的")

dlg2 = G.ImageDialog(None, [], A({"key_id":"a","key_secret":"b"}), "")
assert not dlg2.save_btn.isEnabled() and not dlg2.copy_btn.isEnabled()
assert "先在主窗口的列表里点一台机器" in dlg2.save_btn.toolTip()
check("没选机器 / 一个镜像都没有：按钮灰着，并且说清为什么按不动")

dlg3 = G.ImageDialog(None, rows, G.AutoDLPlatform("t"), "pro-1")
assert dlg3.save_btn.isEnabled() and not dlg3.copy_btn.isEnabled()
check("AutoDL 能做镜像但不能跨地域复制：复制按钮灰着")

print("\n镜像 %d 项全过" % len(ok))
