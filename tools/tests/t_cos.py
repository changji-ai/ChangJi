# -*- coding: utf-8 -*-
import base64, json, os, sys
os.environ["QT_QPA_PLATFORM"] = "offscreen"
# tests/ 的上一层就是 tools/，被测的模块在那儿
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import autodl_gui as G
ok=[]
def check(n): ok.append(n); print("  ✓", n)
T = G.TencentPlatform
FAKE_ID, FAKE_KEY = "AKIDfakefakefake0123456789", "s3cr3tNOTREALnotrealABC987"

BASE = {"region":"ap-shanghai", "spec":"ap-shanghai-2|GN10Xp.2XLARGE40",
        "image":"img-1", "sg":"sg-1", "subnet":"subnet-1", "disk":300,
        "disk_type":"CLOUD_PREMIUM", "strategy":"market", "price":"",
        "bandwidth":100, "name":"changji", "password":"Changji#2026",
        "cos_src":"cos://changji-models-1250000000/wan22/",
        "cos_dest":"/root/models", "cam_role":"changji-cos-read"}
p = T({"secret_id": FAKE_ID, "secret_key": FAKE_KEY})
p.set_context({"region":"ap-shanghai"})

sc = p.boot_script(BASE)
assert "cos-internal.ap-shanghai.myqcloud.com" in sc
assert "cos.ap-shanghai.myqcloud.com" not in sc
check("走内网域名 cos-internal.<地域>.myqcloud.com（公网那个几十 GB 全按流量收费）")

assert "metadata.tencentyun.com/latest/meta-data/cam/security-credentials/changji-cos-read" in sc
check("临时凭证从实例元数据现拿（CAM 角色），不是写死的密钥")
assert FAKE_ID not in sc and FAKE_KEY not in sc
assert "SecretKey" not in sc.replace("TmpSecretKey", "")
check("脚本里一个长期密钥都没有")
assert "chmod 600 /root/.cos.yaml" in sc
check("凭证文件 chmod 600（里面是临时凭证，别给同机别的用户看）")

assert "name: changji-models-1250000000" in sc and "sync cos://models/wan22/" in sc
assert "DEST=/root/models" in sc
check("桶名和前缀拆对了，写进 ~/.cos.yaml 再 sync")
assert "coscli-linux-amd64" in sc and "/var/log/changji-models.log" in sc
assert ".changji-models-ready" in sc
check("装 coscli、日志落盘、拉完留记号，跟阿里云那半截一个形状")

# 预览那条路不走 set_context，地域必须从表单取
fresh = T({"secret_id": FAKE_ID, "secret_key": FAKE_KEY})   # 默认广州
sc2 = fresh.boot_script(dict(BASE, region="ap-beijing"))
assert "cos-internal.ap-beijing.myqcloud.com" in sc2 and "guangzhou" not in sc2
check("地域从表单取，预览和真发出去的是同一份")

assert p.boot_script(dict(BASE, cos_src="")) == ""
check("不填就不装这一套")

assert T.split_cos("cos://b-123/a/b/") == ("b-123", "a/b/")
assert T.split_cos("cos://b-123") == ("b-123", "")
check("cos:// 地址拆桶名和前缀，没前缀也认")

# ---- 进 RunInstances ----
SENT = []
def fake(method, url, headers, body=None, timeout=30, form=None, raw=None):
    act = headers["X-TC-Action"]; SENT.append((act, json.loads(raw)))
    if act == "RunInstances":
        return {"Response":{"InstanceIdSet":["ins-1"]}}
    if act == "DescribeRoleList":
        # 用真账号里真实的那几个名字：服务角色不是统一前缀，
        # 是名字里带 QCSLinkedRole / QcsRole
        return {"Response":{"List":[
            {"RoleName":"CSIP_QCSLinkedRoleInGlobalScene","Description":"服务相关角色"},
            {"RoleName":"TIONE_QcsRole","Description":"TI 平台服务角色"},
            {"RoleName":"changji-cos-read","Description":"只读 COS"}]}}
    return {"Response":{}}
G.request_json = fake
p._image_sizes = {}
SENT[:] = []
assert p.create(BASE) == "ins-1"
act, b = SENT[0]
assert b["CamRoleName"] == "changji-cos-read"
assert base64.b64decode(b["UserData"]).decode() == sc
check("UserData 是 base64 的同一段脚本，CamRoleName 一起发过去")
SENT[:] = []
p.create(dict(BASE, cos_src=""))
assert "UserData" not in SENT[0][1] and "CamRoleName" not in SENT[0][1]
check("不拉模型时这两个参数一个都不发")

# CAM 是第三个服务
SENT[:] = []
roles = p.list_roles()
assert roles[0][1] == "changji-cos-read"      # 自己建的排前面
assert len(roles) == 3                        # 排序不过滤
assert [n for _, n in roles[1:]] == ["CSIP_QCSLinkedRoleInGlobalScene", "TIONE_QcsRole"]
check("CAM 角色列得出来（cam.tencentcloudapi.com，版本 2019-01-16），自己建的排前面")

# ---- 四条判据 ----
for bad, want in [
        (dict(BASE, cos_src="changji-models/wan"), "cos://"),
        (dict(BASE, cos_src="cos://nocappid/wan"), "APPID"),
        (dict(BASE, cam_role=""), "CAM 角色"),
        (dict(BASE, bandwidth=0), "带宽")]:
    try:
        p.validate(bad)
    except G.CloudError as e:
        assert want in str(e), (want, e)
    else:
        raise AssertionError("这条没拦住：%s" % want)
check("四条判据：写法 / 桶名少 APPID / 没给角色 / 带宽 0")
p.validate(BASE)
check("都填对了就放行")

# ---- UserData 尺寸：两家上限不同 ----
assert T.USERDATA_KB == 16 and G.AliyunPlatform.USERDATA_KB == 32
big = "x" * (17 * 1024)
try:
    p.encode_userdata(big)
except G.CloudError as e:
    assert "16 KB" in str(e), e
    check("腾讯云 UserData 超 16 KB 当场拦（接口只回一句参数非法，不说哪个）")
G.AliyunPlatform({"key_id":"a","key_secret":"b"}).encode_userdata(big)   # 32KB 上限，放行
check("同一段脚本在阿里云那边（32 KB）放行——上限是各家自己的")

sm = p.create_summary(BASE)
assert "cos://changji-models-1250000000/wan22/" in sm and "内网" in sm
check("确认框写明开机会去哪儿拉、拉到哪儿")

print("\n腾讯云拉模型 %d 项全过" % len(ok))
