# -*- coding: utf-8 -*-
import base64, os, sys
os.environ["QT_QPA_PLATFORM"] = "offscreen"
# tests/ 的上一层就是 tools/，被测的模块在那儿
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import autodl_gui as G
ok=[]
def check(n): ok.append(n); print("  ✓", n)
A = G.AliyunPlatform

BASE = {"spec":"cn-wulanchabu-a|ecs.gn8is.4xlarge", "image":"m-1", "sg":"sg-1",
        "vsw":"vsw-1", "disk":300, "disk_cat":"cloud_essd",
        "strategy":"SpotAsPriceGo", "price":"", "bandwidth":100,
        "name":"changji", "password":"Changji#2026",
        "oss_src":"oss://changji-models/wan/", "oss_dest":"/root/models",
        "ram_role":"changji-oss-read"}
# 假密钥要有辨识度：拿单个字母当密钥的话，"a" 在脚本里当然到处都是，
# 那条「没漏密钥」的断言就成了摆设
FAKE_ID, FAKE_SECRET = "LTAI5tZZZfakefakefake01", "s3cr3tNOTREALnotrealXYZ987"
p = A({"key_id": FAKE_ID, "key_secret": FAKE_SECRET})
p.set_context({"region":"cn-wulanchabu"})

# ---- 脚本内容 ----
sc = p.boot_script(BASE)
assert "oss-cn-wulanchabu-internal.aliyuncs.com" in sc, sc
assert "-internal." in sc and "oss-cn-wulanchabu.aliyuncs.com" not in sc
check("走内网 endpoint（公网那个几十 GB 全按公网流量收费，还慢）")
assert "--mode EcsRamRole --ecs-role-name changji-oss-read" in sc

# 预览那条路不走 set_context，所以地域必须从表单里取，不能读 self.region——
# 不然预览出来的 endpoint 跟真正发出去的不是一个地域（截图时就是这么发现的）
fresh = A({"key_id": FAKE_ID, "key_secret": FAKE_SECRET})   # 没 set_context，默认杭州
sc_preview = fresh.boot_script(dict(BASE, region="cn-wulanchabu"))
assert "oss-cn-wulanchabu-internal" in sc_preview, sc_preview
assert "hangzhou" not in sc_preview
check("开机脚本的地域从表单取，不读隐藏状态（预览和真发出去的必须是同一份）")
check("凭证走 EcsRamRole：实例从元数据现拿临时凭证")
assert "AccessKeyId" not in sc and "AccessKeySecret" not in sc
assert FAKE_SECRET not in sc and FAKE_ID not in sc
check("脚本里一个长期密钥都没有（UserData 机器上谁都读得到，绝不能塞 AK）")
assert "sync oss://changji-models/wan/ \"$DEST\"" in sc
assert "DEST=/root/models" in sc
check("同步的源和目标都对，源末尾补了斜杠")
assert "/var/log/changji-models.log" in sc and ".changji-models-ready" in sc
check("日志落盘 + 拉完留记号（ssh 上去 ls 一下就知道能不能开工）")

assert p.boot_script(dict(BASE, oss_src="")) == ""
assert G.PPIOPlatform("k").boot_script(BASE) == ""
check("不填就不装这一套；别家平台也不受影响")

# ---- 进 RunInstances ----
SENT = []
def fake(method, url, headers, body=None, timeout=30, form=None, raw=None):
    SENT.append(dict(form or {}))
    if form["Action"] == "RunInstances":
        return {"InstanceIdSets": {"InstanceIdSet": ["i-new"]}}
    if form["Action"] == "ListRoles":
        return {"Roles": {"Role": [{"RoleName": "changji-oss-read",
                                    "Description": "只读 OSS"}]}}
    return {}
G.request_json = fake
SENT[:] = []
p._image_sizes = {}
assert p.create(BASE) == "i-new"
f = SENT[0]
assert f["RamRoleName"] == "changji-oss-read"
got = base64.b64decode(f["UserData"]).decode()
assert got == sc
check("UserData 是 base64 的同一段脚本，RamRoleName 一起发过去")
SENT[:] = []
p.create(dict(BASE, oss_src=""))
assert "UserData" not in SENT[0] and "RamRoleName" not in SENT[0]
check("不拉模型时这两个参数一个都不发")

# ---- RAM 是另一个服务 ----
SENT[:] = []
roles = p.list_roles()
assert roles == [("changji-oss-read（只读 OSS）", "changji-oss-read")]
check("RAM 角色列得出来（ram.aliyuncs.com，域名不带地域、版本 2015-05-01）")

# 自己建的排前面：实测一个普通账号 38 个角色，全是云产品的服务角色，
# 要用的那个是自己建的
def many_roles(method, url, headers, body=None, timeout=30, form=None, raw=None):
    if form["Action"] == "ListRoles":
        return {"Roles": {"Role": [
            {"RoleName": "AliyunCSDefaultRole", "Description": "容器服务"},
            {"RoleName": "changji-oss-read", "Description": "只读 OSS"},
            {"RoleName": "AliyunECSImageImportDefaultRole", "Description": "镜像导入"},
            {"RoleName": "my-other-role", "Description": "别的"}]}}
    return fake(method, url, headers, body, timeout, form, raw)
G.request_json = many_roles
names = [v for _, v in A({"key_id": FAKE_ID, "key_secret": FAKE_SECRET}).list_roles()]
assert names[:2] == ["changji-oss-read", "my-other-role"], names
assert len(names) == 4        # 是排序不是过滤，一个都没少
check("自己建的角色排前面，Aliyun 开头的服务角色排后面（排序不过滤，不挡真能用的）")
G.request_json = fake

# 没给 RAM 权限时不能把整张表拖垮
def no_ram(method, url, headers, body=None, timeout=30, form=None, raw=None):
    if form["Action"] == "ListRoles":
        raise G.CloudError("NoPermission")
    if form["Action"] == "DescribeRegions":
        return {"Regions": {"Region": []}}
    return {"AvailableZones":{"AvailableZone":[]}, "InstanceTypes":{"InstanceType":[]},
            "Images":{"Image":[]}, "SecurityGroups":{"SecurityGroup":[]},
            "VSwitches":{"VSwitch":[]}}
G.request_json = no_ram
ch = A({"key_id": FAKE_ID, "key_secret": FAKE_SECRET}).load_choices()
assert ch["ram_role"] == [] and "image" in ch
check("账号没给 RAM 读权限：角色那栏空着，别的照样拉得到")
G.request_json = fake

# ---- 建机前的三条判据 ----
for bad, want in [
        (dict(BASE, oss_src="changji-models/wan"), "oss://"),
        (dict(BASE, ram_role=""), "RAM 角色"),
        (dict(BASE, bandwidth=0), "带宽")]:
    try:
        p.validate(bad)
    except G.CloudError as e:
        assert want in str(e), (want, e)
    else:
        raise AssertionError("这条没拦住：%s" % want)
check("三条本地判据：写法不对 / 没给角色 / 带宽填 0（装不了 ossutil）")
p.validate(BASE)
check("都填对了就放行")

# 地域和可用区对不上：不只是建机会失败，开机脚本的内网 endpoint 也会指错地方
p2 = A({"key_id": FAKE_ID, "key_secret": FAKE_SECRET})
p2.set_context({"region": "cn-hangzhou"})      # 地域杭州，规格却在乌兰察布
p2._image_sizes = {}
try:
    p2.validate(BASE)
except G.CloudError as e:
    assert "不在地域" in str(e) and "cn-hangzhou" in str(e), e
    check("地域和规格对不上：当场拦住（截图时真撞见过，内网 endpoint 会指错地域）")
else:
    raise AssertionError("地域对不上却放行了")

# 确认框要说清楚开机会干什么
sm = p.create_summary(BASE)
assert "oss://changji-models/wan/" in sm and "内网" in sm
check("确认框里写明「开机自动从 oss://… 拉模型，走内网不计流量费」")

print("\n开机拉模型 %d 项全过" % len(ok))
