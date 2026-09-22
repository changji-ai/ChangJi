#!/usr/bin/env bash
# **拿假模型真跑一章，把只有跑起来才出现的那几句话读出来。**
#
#     sh cpp/tools/fake_run.sh [语言 …]
#     sh cpp/tools/fake_run.sh en ru de ar
#
# 为什么要有它：引擎说的话里**有一大半只在跑起来的时候才出现**——任务条上
# 那一行、干完那一下的回话、进度里的数。单元测试查的是表，单条接口查的是
# 报错，这两样都碰不到它们。而这一族正是复数、语序、动词一致这些最容易错的
# 地方（`Wrote 1 chapters` 就长在这儿）。
#
# ⚠️ **不花钱、不动真项目**：`cpp/tools/fake_llm.py` 当后端（照着 reply.txt
# 回同一份稿子），`HOME` 换成一个临时目录，端口挑 8240/8241。
#
# ⚠️ **每种语言各跑 1 章和 3 章。** 单数那一档是这族毛病的命门，而德语的
# `Kapitel` 不随数变——**只跑德语什么都验不出来**，英语俄语才看得见。
#
# 顺手扫一遍回包里有没有漏出汉字（用户自己写的那些除外）。
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ENGINE=""
for up in ../.. ../../.. ../../../..; do
    [ -x "$HERE/$up/build/changji" ] && { ENGINE="$HERE/$up/build/changji"; break; }
done
[ -n "$ENGINE" ] || { echo "找不到编好的引擎（<构建目录>/changji）"; exit 1; }
LANGS=${*:-en ru de}
FP=8240; EP=8241
WORK="$(mktemp -d)"; HOME_DIR="$WORK/home"
CFG="$HOME_DIR/Library/Application Support/changji"
PROJ="$CFG/projects/demo"
mkdir -p "$CFG" "$WORK/fake"
# ⚠️ **路径先解析掉软链。** `mktemp -d` 在 macOS 上给的是 `/var/folders/…`，
# 而 `/var` 是指向 `/private/var` 的软链——引擎记项目时记的是解析过的那条，
# 拿没解析的去 `/api/tasks?project=…` 过滤**筛出来是空的**：任务明明跑完了，
# 一个字都不出，而两条 POST 都回 `started: true`，看着一切正常。
PROJ="$(python3 -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$PROJ")"
cat > "$CFG/config.toml" <<TOML
[llm]
backend = "remote"
base_url = "http://127.0.0.1:$FP/v1"
model = "fake"
api_key = "x"
context_tokens = 32768
TOML
# ⚠️ **稿子要真过得了闸门，不然这一趟等于没跑。**
# 头一版每场只有 6 段、段段一样，三次全被退回——**而任务条照样写
# 「Wrote 1 chapter」**。那个 bug 就是这么撞见的（见 http/batch.cpp 里
# `done` / `wrote` / `failed` 那三个数）。过闸门要凑齐三样：每场段数够
# （`minItems`，约每场段数的一半）、每段至少 20 字、**至少三成的段落有人
# 说话**。
write_reply() {   # write_reply good|short
python3 - "$WORK/fake" "$1" <<'PYEOF'
import json, pathlib, sys
W, kind = pathlib.Path(sys.argv[1]), sys.argv[2]
ACT = ["他把抽屉一层层拉开，纸页翻动的声音在空屋子里格外响。",
       "外面的雨顺着玻璃往下淌，把对面楼的灯拉成歪斜的线。",
       "她站在门口没有进来，手还搭在门把上，像随时会走。",
       "他把那张单据推过去，指了指最底下那一行小字。",
       "楼下有人按了两声喇叭，她的肩膀几不可察地动了动。",
       "他把烟按灭在杯盖上，抬头看她，等她把话说完。",
       "风从走廊尽头灌进来，吹得桌上的纸角翻了一下。"]
SAY = ["她说：「这笔钱走的不是公司的账，你心里清楚得很。」",
       "他说：「我清楚，可清楚有什么用，签字的不是我。」",
       "她说：「那你就当没看见？这事压不住三个月。」",
       "他说：「压不住也得压，我还想把这个月的工资拿到手。」",
       "她说：「你以前不是这样的人。」",
       "他说：「以前我也没欠过谁两年的房租。」",
       "她说：「那张单子你留着，哪天想通了再找我。」"]
def scene(where, turn, off):
    return {"where": where, "turn": turn,
            "paragraphs": [(SAY if i % 2 else ACT)[(off + i) % 7] for i in range(14)]}
if kind == "short":      # 段数不够：闸门必退，专门用来验「写砸了」那一句
    body = {"scenes": [{"where": "办公室", "turn": "他发现账目对不上",
                        "paragraphs": [ACT[0]] * 3}]}
else:
    body = {"scenes": [scene("办公室", "他发现账目对不上", 0),
                       scene("走廊", "她拦住了他", 2),
                       scene("天台", "两个人把话说开了", 4)]}
(W / "reply.txt").write_text(json.dumps(body, ensure_ascii=False), encoding="utf-8")
PYEOF
}
write_reply good
kill_port() { t=$(lsof -nP -iTCP:"$1" -sTCP:LISTEN -t 2>/dev/null); [ -n "$t" ] && kill -9 $t; sleep 0.4; }
kill_port $FP; kill_port $EP
( cd "$WORK/fake" && nohup python3 "$HERE/fake_llm.py" $FP "$WORK/fake" > "$WORK/fake.log" 2>&1 & )
sleep 2

for L in $LANGS; do
  for n in 1 3; do
    kill_port $EP
    python3 - "$PROJ" "$n" <<'PY'
import json, pathlib, sys
P = pathlib.Path(sys.argv[1]); n = int(sys.argv[2])
P.mkdir(parents=True, exist_ok=True)
(P / "story.json").write_text(json.dumps({
    "schema_version": 1, "title": "demo", "premise": "一个下岗保安捡到一部旧手机",
    "chapters": [{"chapter_id": f"ch{i+1:02d}", "title": f"第{i+1}章",
                  "synopsis": "他在旧手机里发现了一段录音。", "text": "", "hooks": []}
                 for i in range(n)]}, ensure_ascii=False, indent=2), encoding="utf-8")
PY
    ( HOME="$HOME_DIR" CHANGJI_LANG="$L" nohup "$ENGINE" --port $EP > "$WORK/eng.log" 2>&1 & )
    sleep 3.5
    # ⚠️ **用它回的那个 `root` 当项目路径，别用自己拼的那个。**
    # `mktemp -d` 给的是 `/var/folders/…`，而引擎记的是解析过的
    # `/private/var/folders/…`——拿前者去 `/api/tasks?project=…` 过滤，
    # **筛出来是空的**，任务明明跑完了却一个字都不出。第一版就卡在这儿，
    # 而两条 POST 都回的 `started: true`，看着一切正常。
    curl -s -X POST "http://127.0.0.1:$EP/api/new" -H 'content-type: application/json' \
         -d "{\"path\":\"$PROJ\",\"name\":\"demo\"}" > /dev/null
    curl -s -X POST "http://127.0.0.1:$EP/api/story/chapters" -H 'content-type: application/json' \
         -d "{\"project\":\"$PROJ\"}" > /dev/null
    k=0
    while [ $k -lt 25 ]; do
      out=$(curl -s -G "http://127.0.0.1:$EP/api/tasks" --data-urlencode "project=$PROJ")
      echo "$out" | grep -q '"state":"done"' && break
      sleep 2; k=$((k+1))
    done
    echo "$out" > "$WORK/tasks_${L}_$n.json"
    echo "$out" | python3 -c "
import json, sys
for t in json.load(sys.stdin).get('done', []):
    print(f'  $L n=$n  任务名：{t.get(\"title\")}')
    print(f'          完成：{t.get(\"note\")}')
"
  done
done
# ---- 写砸了那一档 ----
#
# **这是守卫，不是演示。** 2026-09-22 撞见的：一章被闸门退回三次、正文一个字
# 没有，任务条上却写着「写完了 1 章」，而 error 是空的——`done` 同时当了
# 「走完几章」（进度条）和「写成几章」（总结句）两件事。修完之后这一档该说
# 的是「写完了 0 章，另有 1 章没写成」。
echo
echo "== 写砸了那一档（闸门必退，看总结句的数对不对）=="
write_reply short
for L in $LANGS; do
  kill_port $EP
  ( HOME="$HOME_DIR" CHANGJI_LANG="$L" nohup "$ENGINE" --port $EP > "$WORK/eng.log" 2>&1 & )
  sleep 3.5
  python3 - "$PROJ" <<'PYEOF'
import json, pathlib, sys
P = pathlib.Path(sys.argv[1])
P.mkdir(parents=True, exist_ok=True)
(P / "story.json").write_text(json.dumps({
    "schema_version": 1, "title": "demo", "premise": "一个下岗保安捡到一部旧手机",
    "chapters": [{"chapter_id": "ch01", "title": "第1章",
                  "synopsis": "他在旧手机里发现了一段录音。", "text": "", "hooks": []}]},
    ensure_ascii=False, indent=2), encoding="utf-8")
PYEOF
  curl -s -X POST "http://127.0.0.1:$EP/api/story/chapters" -H 'content-type: application/json' \
       -d "{\"project\":\"$PROJ\"}" > /dev/null
  k=0
  while [ $k -lt 25 ]; do
    out=$(curl -s -G "http://127.0.0.1:$EP/api/tasks" --data-urlencode "project=$PROJ")
    echo "$out" | grep -q '"state":"done"' && break
    sleep 2; k=$((k+1))
  done
  echo "$out" | python3 -c "
import json, sys
for t in json.load(sys.stdin).get('done', []):
    print('  $L 写砸了 →', t.get('note'))
"
done

kill_port $EP; kill_port $FP

echo
echo "== 回包里漏出来的汉字（用户自己写的除外）=="
python3 - "$WORK" <<'PY'
import json, re, pathlib, sys
W = pathlib.Path(sys.argv[1])
HAN = re.compile(r"[一-鿿]")
USER = ("demo", "第1章", "第2章", "第3章", "一个下岗保安捡到一部旧手机")
def walk(o, path=""):
    if isinstance(o, dict):
        for k, v in o.items(): yield from walk(v, f"{path}.{k}")
    elif isinstance(o, list):
        for i, v in enumerate(o): yield from walk(v, f"{path}[{i}]")
    elif isinstance(o, str) and HAN.search(o):
        yield path, o
bad = 0
for f in sorted(W.glob("tasks_*.json")):
    if f.name.startswith("tasks_zh"): continue
    d = json.loads(f.read_text(encoding="utf-8"))
    for p, v in walk(d):
        if any(u in v for u in USER): continue
        bad += 1
        print(f"  {f.stem}{p} = {v[:90]}")
print(f"  —— {bad} 处 ——")
PY
echo "日志和回包在 $WORK"
