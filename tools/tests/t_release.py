# -*- coding: utf-8 -*-
import sys
import os
# tests/ 的上一层就是 tools/，被测的模块在那儿
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import autodl_gui as G
G.time.sleep = lambda s: None          # 别真等 3 秒一轮

ok = []
def check(n): ok.append(n); print("  ✓", n)

class Rec(object):
    """把平台的四个原子动作换成记账，只看调用顺序。"""
    def __init__(self, cls, states, delete_fails=0):
        self.p = cls("k"); self.calls = []; self.states = list(states)
        self.left = delete_fails
        self.p.status = self._status
        self.p.power_off = self._off
        self.p.release = self._rel
    def _status(self, i):
        self.calls.append("status")
        return self.states.pop(0) if len(self.states) > 1 else self.states[0]
    def _off(self, i): self.calls.append("stop")
    def _rel(self, i):
        self.calls.append("delete")
        if self.left:
            self.left -= 1
            raise G.CloudError("instance is running")
    def run(self, **kw):
        logs = []
        self.p.release_safely("x", logs.append, **kw)
        return self.calls

# ---- AutoDL：文档说释放前必须关机，所以一定先查、先关 ----
r = Rec(G.AutoDLPlatform, ["running", "running", "shutting_down", "shutdown"])
assert r.run() == ["status", "stop", "status", "status", "status", "delete"], r.calls
check("AutoDL 开着：查 → 关 → 轮询到 shutdown → 释放")

r = Rec(G.AutoDLPlatform, ["shutdown"])
assert r.run() == ["status", "delete"], r.calls
check("AutoDL 已关机：不多点一次关机，直接释放")

r = Rec(G.AutoDLPlatform, ["exited"])
assert r.run() == ["status", "delete"], r.calls
check("AutoDL 状态叫 exited 也认得是关好了（不是只认 shutdown 一个词）")

r = Rec(G.AutoDLPlatform, ["running"])
try:
    r.run(wait_s=0.01)
except G.CloudError as e:
    assert "delete" not in r.calls, r.calls
    check("AutoDL 关不掉：拒绝释放，不静悄悄当成功（%s）" % str(e)[:28])
else:
    raise AssertionError("关不掉却释放了")

# ---- PPIO：没说要先停机，所以先直接删；删不动才退回停机 ----
r = Rec(G.PPIOPlatform, ["running"])
assert r.run() == ["delete"], r.calls
check("PPIO 常见路径：一步删掉，不让用户白等一轮停机")

r = Rec(G.PPIOPlatform, ["stopping", "stopped"], delete_fails=1)
assert r.run() == ["delete", "stop", "status", "status", "delete"], r.calls
check("PPIO 删不动：退回 停机 → 等停稳 → 再删")

r = Rec(G.PPIOPlatform, ["running"], delete_fails=1)
try:
    r.run(wait_s=0.01)
except G.CloudError as e:
    assert r.calls.count("delete") == 1, r.calls   # 没有第二次删
    check("PPIO 停不下来：不再删第二次，把真情况报出来")
else:
    raise AssertionError("停不下来却删了")

# ---- 守卫真的会红吗：把「等停稳」拆掉，看用例逮不逮得住 ----
saved = G.Platform._wait_off
G.Platform._wait_off = lambda self, i, log, w: "running"   # 假装永远等不到
broke = 0
for cls, kw in ((G.AutoDLPlatform, {}), (G.PPIOPlatform, {"delete_fails": 1})):
    r = Rec(cls, ["running"], **kw)
    try:
        r.run(wait_s=0.01)
    except G.CloudError:
        broke += 1
G.Platform._wait_off = saved
assert broke == 2
# 再证一次：把守卫拿掉（等停稳直接返回 stopped），用例就会放行 —— 说明它真在测东西
G.Platform._wait_off = lambda self, i, log, w: "stopped"
r = Rec(G.AutoDLPlatform, ["running"])
r.run(wait_s=0.01)
assert "delete" in r.calls
G.Platform._wait_off = saved
check("守卫真会红：等不到就抛、等得到才放行，不是恒真断言")

print("\n释放流程 %d 项全过" % len(ok))
