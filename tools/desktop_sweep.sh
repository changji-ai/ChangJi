#!/bin/sh
# 把桌面端的每一档界面都真开一遍，**只看它红不红**。
#
# 为什么要有这个：QML 那层的错**在界面上是不响的**——一句 `ReferenceError`
# 只写进 stderr，而出错的那条绑定停在默认值上（`visible: index === 0` 抛了
# 之后 `visible` 就是 `true`）。自检那几个口子一开，`main.cpp` 里那个消息
# 钩子会把这种告警变成退出码 1。
#
# ⚠️ **但它只抓得到"真求过值"的那些。** 一条只在窄窗那一支里求值的表达式，
# 在宽窗上一辈子不响；一格没开过，它里头的绑定一次都不会跑。所以光跑一趟
# 默认状态等于没跑——这个脚本的全部意义就是**把每一档都摆出来一次**：
# 五格 × 明暗 × 宽窄、两格摞着、最大化、拉出去、设置那四类、收起侧栏、关窗。
#
# 用法（第二个参数是给它当 HOME 的目录，里头要有配好的项目库）：
#
#     sh cpp/tools/desktop_sweep.sh ../build/desktop/changji-desktop.app/Contents/MacOS/changji-desktop ~/某个测试用的 HOME [站在哪一部片子上]
#
# 第三个参数可以不给；给了的话每一趟都从那一部片子开始，**跟上一次谁动过
# 什么无关**（理由见底下 `lastProject` 那段）。
#
# 红了的那一档会把前三行告警印出来，退出码是红的个数。

set -u
APP="${1:?第一个参数：changji-desktop 可执行文件的路径}"
FAKEHOME="${2:?第二个参数：给它当 HOME 的目录}"
OUT="${TMPDIR:-/tmp}/changji-sweep"
mkdir -p "$OUT"

# ⚠️ **偏好那一族不认 HOME，得单独扳回来。**
#
# macOS 上 QSettings 走 cfprefsd（域 `com.changji.场记`），换 HOME 换不掉它
# ——于是**上一趟把侧栏收起来了，下一趟一上来就是收着的**，而设置那颗齿轮
# 就住在那条栏里。2026-09-21 第二次跑这个脚本时实撞：三档设置全红，报的是
# `点不着「齿轮」：visible=0 宽高=-16x28`，而第一趟是全绿的。
#
# 所以开跑之前先把它扳回来；底下「收侧栏」那一趟也在同一次运行里扳回去。
#
# ⚠️ **`lastProject` 也在这个域里，而这一趟扫的每一张图都指望它。**
# 2026-09-21 实撞：拿另一个 HOME 单独试了一下换片子，那一下把 `lastProject`
# 写成了另一个 HOME 里的一部片子——**下一趟扫描四档全红**，报的是
# 「点不着『格·story』：visible=0」「点不着『上下文条』：visible=0」
# 「画面上没有『场记这段』」，看着像界面坏了，其实是这一趟开在一部空片子上。
#
# 第三个参数就是给它的：**这一趟要站在哪一部片子上**。不给就不动它
#（接着用上一次留下的那个，和以前一样）。
if command -v defaults >/dev/null 2>&1; then
  defaults write com.changji.场记 sideOpen -bool true 2>/dev/null || true
  if [ "${3:-}" != "" ]; then
    defaults write com.changji.场记 lastProject -string "$3" 2>/dev/null || true
    # ⚠️ **「开哪一条对话」那一笔也得清掉。**
    #
    # 钉住片子还不够：每部片子还单记着「上次说的是哪一条」
    #（`lastChat/<路径的 base64>`，见 `chat_model.cpp`）。别处点过一次别的
    # 对话，那一笔就留在这个域里——而这一趟扫描会从那一条开始，于是
    # 「底下那一排」那几档报的是「画面上没有『场记这段』」（那一条里
    # 一句场记的话都没有）。2026-09-21 实撞，和 `lastProject` 那次一模一样。
    #
    # 清掉就落回这部片子默认那一条（空 id 的「原先那条」），每趟都一样。
    key="lastChat/$(printf '%s' "$3" | base64)"
    defaults delete com.changji.场记 "$key" 2>/dev/null || true
  fi
fi

# ---- 素材跑完要还原 ----
#
# ⚠️ **这一趟扫描会往素材里写字。** 「说一句」「正在说」那两档是**真敲键盘**
# 的——每跑一趟，那条对话尾巴上就多一条没人答的用户消息（这台机器上没配
# 密钥，场记答不上来）。攒够了之后可见的末尾全是用户的话，于是
# 「底下那一排」那一档报「画面上没有『场记这段』」——**看着像界面坏了，
# 其实是扫描把自己的素材写坏了**（2026-09-21 实撞，查了半天）。
#
# 所以：开跑前记下那份对话，跑完（不管红没红）放回去。一趟扫描于是
# **不改变任何东西**，第一百趟和第一趟看到的是同一份数据。
RESTORE_FROM=""
RESTORE_TO=""
if [ "${3:-}" != "" ] && [ -f "$3/chat.jsonl" ]; then
  RESTORE_TO="$3/chat.jsonl"
  RESTORE_FROM="$OUT/chat.jsonl.before"
  mkdir -p "$OUT"
  cp "$RESTORE_TO" "$RESTORE_FROM"
  # `trap` 管住半路 Ctrl-C 和中途退出那两条路。
  trap 'if [ -n "$RESTORE_FROM" ] && [ -f "$RESTORE_FROM" ]; then cp "$RESTORE_FROM" "$RESTORE_TO"; fi' EXIT INT TERM
fi

bad=0
run() {
  name="$1"; shift
  # ⚠️ **`QML_DISABLE_DISK_CACHE=1` 不能省。**
  #
  # Qt 把编译过的 QML 缓在 `$HOME/Library/Caches` 里，而这个卷是 HFS+
  # ——**时间戳只有一秒精度**（CLAUDE.md 开头那条，ninja 也栽在同一处）。
  # 改完马上编、编完马上跑的时候，缓存看不出源文件变过，于是**跑的是上一版
  # 的界面**：2026-09-21 实撞，改了图标之后连着截了四张图，张张都是旧的，
  # 连早就删掉的那颗齿轮都还在。加上这一条才看见新的。
  ( env HOME="$FAKEHOME" QML_DISABLE_DISK_CACHE=1 CHANGJI_DESKTOP_SHOT="$OUT/$name.png" "$@" "$APP" \
      > "$OUT/$name.log" 2> "$OUT/$name.err" )
  code=$?
  if [ $code -ne 0 ]; then
    bad=$((bad+1))
    echo "✗ $name 退出码 $code"
    grep -E "QML |qrc:" "$OUT/$name.err" | head -3
  else
    echo "✓ $name"
  fi
}

# 五格 × 明暗 × 宽窄。
#
# **窄那一档给两个尺寸**：只给一档的话程序一上来就站在那条线的某一边，
# 「拖过去的那一下」根本不发生——而挂在窗口宽度上的绑定有一整族（并排还是
# 盖上来、盖上来时多宽、对话让不让位），只在窄的那一支里求值的表达式在宽窗
# 上一辈子不响。
for g in story shots film assets script; do
  run "格_$g"     CHANGJI_DESKTOP_SHOT_MS=6000 CHANGJI_DESKTOP_SHOT_OPEN=$g CHANGJI_DESKTOP_SIZE=1240x820
  run "格_${g}_暗" CHANGJI_DESKTOP_SHOT_MS=6000 CHANGJI_DESKTOP_SHOT_OPEN=$g CHANGJI_DESKTOP_SCHEME=dark CHANGJI_DESKTOP_SIZE=1240x820
  run "格_${g}_窄" CHANGJI_DESKTOP_SHOT_MS=6000 CHANGJI_DESKTOP_SHOT_OPEN=$g CHANGJI_DESKTOP_SIZE=1240x820,760x760 CHANGJI_DESKTOP_SIZE_MS=2500
done

run 两格   CHANGJI_DESKTOP_SHOT_MS=7500 CHANGJI_DESKTOP_TAP=格·story,格·shots CHANGJI_DESKTOP_TAP_MS=3500 CHANGJI_DESKTOP_SIZE=1240x820
run 两格窄 CHANGJI_DESKTOP_SHOT_MS=8000 CHANGJI_DESKTOP_TAP=格·story,格·shots CHANGJI_DESKTOP_TAP_MS=3500 CHANGJI_DESKTOP_SIZE=1240x820,780x780 CHANGJI_DESKTOP_SIZE_MS=2600
run 最大化 CHANGJI_DESKTOP_SHOT_MS=7000 CHANGJI_DESKTOP_SHOT_OPEN=shots CHANGJI_DESKTOP_TAP=最大化 CHANGJI_DESKTOP_TAP_MS=4500
run 拉出去 CHANGJI_DESKTOP_SHOT_MS=7000 CHANGJI_DESKTOP_SHOT_OPEN=shots CHANGJI_DESKTOP_TAP=新窗口 CHANGJI_DESKTOP_TAP_MS=4500 CHANGJI_DESKTOP_SHOT_WIN=2
run 设置   CHANGJI_DESKTOP_SHOT_MS=6500 CHANGJI_DESKTOP_TAP=齿轮 CHANGJI_DESKTOP_TAP_MS=3800
run 设置暗 CHANGJI_DESKTOP_SHOT_MS=6500 CHANGJI_DESKTOP_TAP=齿轮 CHANGJI_DESKTOP_TAP_MS=3800 CHANGJI_DESKTOP_SCHEME=dark
run 设置每类 CHANGJI_DESKTOP_SHOT_MS=15500 CHANGJI_DESKTOP_TAP=齿轮,设置类·film,设置类·gate,设置类·tts,设置类·look,设置类·box,设置类·gguf,设置类·peer,设置类·llm CHANGJI_DESKTOP_TAP_MS=3500
# **收了要再打开**：这一下是落盘的，不扳回去下一趟就从"收着"开始（见上面）。
run 收侧栏 CHANGJI_DESKTOP_SHOT_MS=7000 CHANGJI_DESKTOP_TAP=收侧栏,开侧栏 CHANGJI_DESKTOP_TAP_MS=3800
run 上下文条 CHANGJI_DESKTOP_SHOT_MS=6500 CHANGJI_DESKTOP_TAP=上下文条 CHANGJI_DESKTOP_TAP_MS=3800
# ---- 真说一句话 ----
#
# 上面那些档都是"摆出来看"，而**对话里那几种东西只有真说一句才长出来**：
# 人说的那个气泡、底下那行「在想…」、工具回的那几行小字、场记那段
# Markdown（标题/列表/代码/引用/表格那一套排版也在这一趟里才跑）。
# 不走这一趟的话，那一整族绑定一次都不求值——而这个脚本的全部意义就是
# 「把每一档都摆出来一次」。
#
# ⚠️ **这两趟要有个能应答的大模型才完整**。连不上也不白跑：那时候走的是
# 报错那条路（照样是一整套 delegate），QML 那层有错一样会红。
run 说一句 CHANGJI_DESKTOP_SHOT_MS=14000 CHANGJI_DESKTOP_SAY='第一章怎么样了' CHANGJI_DESKTOP_SAY_MS=4000
# 正在说的那一档：截图卡在它还没说完的时候（气泡在长、底下那行在喘）。
run 正在说 CHANGJI_DESKTOP_SHOT_MS=5600 CHANGJI_DESKTOP_SAY='第一章怎么样了' CHANGJI_DESKTOP_SAY_MS=4000

# 每条回话底下那一排（复制 / 分叉 / 朗读 / 多久之前）。
#
# **朗读那颗要真按下去**：念出来那一段是个 `Loader` 装着的 `MediaPlayer`，
# 不按的话那个 Component **一次都不实例化**，里头写错什么都不会响
#（同"一格没开过它里头的绑定一次都不跑"，见文件头）。
#
# ⚠️ **这一趟不按分叉。** 按一下就在盘上多一条对话，跑十趟多十条——
# 而这个脚本得能反复跑（上头「收侧栏」那条吃过一次亏）。复制写的是剪贴板、
# 朗读盖的是同一个 say.wav，两样都不留痕迹。
#
# ⚠️ **第一下是 `~`（只挪不按）。** 这一排现在是**停上去才显示**的，
# 鼠标没过去之前它 `opacity` 是 0——直接点报的是「visible=0」。先挪过去
# 让那一段进入"鼠标压着"，后面几下才点得着（`~` 之后鼠标就停在那儿了，
# 而那一排是那一段的孩子，所以接着点第二颗也还是亮的）。
run 底下那一排 CHANGJI_DESKTOP_SHOT_MS=9000 CHANGJI_DESKTOP_TAP='~场记这段,复制这段,朗读这段' CHANGJI_DESKTOP_TAP_MS=5000
# 人说的那一条底下那一排（时间 · 复制 · 修改）。**改那一颗要真按下去**：
# 它把原话放回输入框，而那条路上有一道"输入框里还有字就别盖"的门。
run 我这段底下 CHANGJI_DESKTOP_SHOT_MS=8000 CHANGJI_DESKTOP_TAP='~复制我这段,复制我这段,改我这段' CHANGJI_DESKTOP_TAP_MS=4500

# 输入框里那段 Markdown 边写边描（`md_source.hpp`）。
#
# **末尾留一个换行就不发出去**，停在"正在写"那一屏上——要看的正是那一屏：
# 记号还在、被它包住的那段已经粗了。顺带这一趟也是"输入框跟着内容长高"
# 那条的唯一一次求值（别的档里那个框都是空的）。
run 写Markdown CHANGJI_DESKTOP_SHOT_MS=11000 CHANGJI_DESKTOP_SAY='# 第三章要改的\n\n- 第 5 场那段对白**删掉两句**\n- 把 `shots_read` 那一步跳过\n\n> 钩子在第 2 场，别动它\n' CHANGJI_DESKTOP_SAY_MS=5000

# 局域网感知：设置里那个开关 + 输入框底下那盏灯。
#
# **真拨一次**：那一下会去 POST /api/lan、把 mDNS 转起来，而这条路上
# 一个平台没接上时接口回 501——拨完再拨回去，不给别的档留下一个开着的感知。
run 局域网 CHANGJI_DESKTOP_SHOT_MS=17000 CHANGJI_DESKTOP_TAP='齿轮,设置类·box,局域网感知,局域网感知,关设置,~局域网灯' CHANGJI_DESKTOP_TAP_MS=5000

# 一条对话右边那颗「⋯」：点开是置顶和删掉。
#
# ⚠️ **只掀开，不挑**：置顶落在 cfprefsd 里（不认 HOME），挑一下就留给了
# 下一趟——而下一趟的每一张图都会多出那一行。同 `lastProject` 那条。
run 那颗点   CHANGJI_DESKTOP_SHOT_MS=10000 CHANGJI_DESKTOP_TAP='~对话·互联·原先那条,⋮·' CHANGJI_DESKTOP_TAP_MS=6000

# 输入框上面那两颗药丸：哪一部片子 · 哪一章。**只掀开，不挑**（同下面那条）。
run 挑片子   CHANGJI_DESKTOP_SHOT_MS=11000 CHANGJI_DESKTOP_TAP='项目条' CHANGJI_DESKTOP_TAP_MS=7000

# 输入框底下那一条：厂商 · 模型 · 想多久。三张单子各开一次。
#
# ⚠️ **只掀开，不挑**。挑一下会写进这个 HOME 的 config.toml，于是下一趟扫
# 出来的每一张图上那一条都不一样了——而这个 HOME 是所有档共用的。
run 挑厂商   CHANGJI_DESKTOP_SHOT_MS=11000 CHANGJI_DESKTOP_TAP='挑厂商' CHANGJI_DESKTOP_TAP_MS=7000
run 挑模型   CHANGJI_DESKTOP_SHOT_MS=11000 CHANGJI_DESKTOP_TAP='挑模型' CHANGJI_DESKTOP_TAP_MS=7000
run 想多久   CHANGJI_DESKTOP_SHOT_MS=11000 CHANGJI_DESKTOP_TAP='想多久' CHANGJI_DESKTOP_TAP_MS=7000

# ---- 从右往左那一版（阿拉伯语）----
#
# **翻过来之后是另一套布局**，不是同一套换了几个字：侧栏挪到右边、面板
# 挪到左边、`Row` 里的顺序反过来、`anchors.left` 指的成了右。而这一套里
# 有一整族东西是拿 `x` 摆的（标题栏上那三样），`LayoutMirroring` 管不着
# ——只在这一支里求值的表达式，在从左往右的窗上一辈子不响。
#
# ⚠️ **左上角那三颗系统按钮不跟着翻**（macOS 的阿拉伯语版也是这样），
# 所以翻过来之后压在它们底下的是另外几样东西：图标条、面板标题行上那
# 三颗。让路那几笔只在这一支里算。
run 从右往左     CHANGJI_DESKTOP_LANG=ar CHANGJI_DESKTOP_SHOT_MS=6500 CHANGJI_DESKTOP_SIZE=1240x820
run 从右往左面板 CHANGJI_DESKTOP_LANG=ar CHANGJI_DESKTOP_SHOT_OPEN=shots CHANGJI_DESKTOP_SHOT_MS=6500 CHANGJI_DESKTOP_SIZE=1240x820
run 从右往左设置 CHANGJI_DESKTOP_LANG=ar CHANGJI_DESKTOP_TAP=齿轮 CHANGJI_DESKTOP_TAP_MS=3800 CHANGJI_DESKTOP_SHOT_MS=6500

# 关窗那一趟走的是 `onClosing`（截图那条走 quit()，不经过它）。
run 关窗   CHANGJI_DESKTOP_CLOSE_TEST=1 CHANGJI_DESKTOP_SHOT_MS=6000

echo "—— 红了 $bad 个，图在 $OUT ——"
exit $bad
