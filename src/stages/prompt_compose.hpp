#pragma once

// 提示词组装。
//
// **这是角色一致性真正生效的地方。** 同一个角色在几十个镜头里拿到的
// 身份层完全相同，因为它是从资产库读出来的同一个字符串——模型碰不到它。
//
// 分层顺序是固定的，**不能改**：
//
//     身份层（角色外观，逐字节不变）
//     场景层（空间与光线，每场固定）
//     镜头层（景别、机位、动作，每镜可变）
//     风格层（全片统一）
//
// 顺序不能改是因为提示词里靠前的词权重更高。顺序一变画面重心就跟着变，
// 同一个角色在不同镜头里会显得不是同一个人。
//
// ---
//
// 这一层和出图后端无关：ComfyUI 也好、进程内 sd.cpp 也好，
// 拿到的是同一份提示词。所以它属于**契约**，要和 Python 逐字节一致。

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "models/character.hpp"
#include "models/shot.hpp"

namespace changji::stages {

class RenderError : public std::runtime_error {
public:
    explicit RenderError(const std::string& what) : std::runtime_error(what) {}
};

/// 一个镜头拼好的正负提示词。
struct PromptBundle {
    std::string positive;
    std::string negative;
    /// 视频模型的负向词：镜头自己的 + [style].negative_video。和 `negative`
    /// 分开是因为图像那份（「多余的手指」）对出片模型没意义，而出片模型真会
    /// 犯的错（溶解、形变、片中硬切）图像那份一个字没提。
    std::string negative_video;
    /// 视频模型的正向词里「画面」那一半：景别机位焦段、这一镜的光、画面
    /// 描述——**不带身份层和场景资产层**。首帧已经把长相、服装、场景定死了，
    /// 再把整段外观喂给出片模型，它会试着重画一遍角色，脸在动的过程中变形
    /// （prompt_compose.hpp 顶上那条设计声明；2026-09-16 之前 video_positive
    /// 是整段 positive + motion，声明只约束了模型没约束代码）。
    /// 留画面描述是为了让它知道「推的是谁」（render.hpp 那条的理由）。
    std::string video_scene;
    /// 风格层（全片统一的画风），视频正向词末尾也带。
    std::string style_layer;
    /// 角色定妆图和场景空景图的相对路径。图像模型那条路会用，
    /// 视频模型那条路忽略。
    std::vector<std::string> reference_images;
    /// **这一张要基础文生图权重（[models].image_base），不是 Edit。**
    /// 定妆图和空景图是从纯文字画的，一张参考图都没有；Edit 权重没有编辑
    /// 源会退化成文生图，出来的不能看。首帧不填（它喂参考图，正是 Edit
    /// 的活）。跟着提示词走是为了**跨机**：画参考图和出首帧走同一条
    /// FrameRenderer（进程内或派给别的机器），这条路上只有它能带东西。
    bool base_model = false;
    /// 种子由发起方定死时填这个；不填的话首帧按 shot_id + attempts 算。
    /// 定妆图那条路填：同一个角色重出还是那张脸（http::ref_seed）。
    std::optional<std::int64_t> seed_override;
};

/// 这两句光说的是不是同一个时段。空串一律算「说不准」，返回 false。
///
/// **给"要不要喂那张空景图"用的**，见 PromptComposer::compose_with。
bool lighting_clashes(const std::string& location_light,
                      const std::string& shot_light);

/// 把分镜表和资产库拼成提示词。
class PromptComposer {
public:
    explicit PromptComposer(models::AssetLibrary assets);

    PromptBundle compose(const models::Shot& shot) const;

    /// 尾帧的提示词：和 compose 一样的分层，只是画面描述换成
    /// `last_frame_prompt`。没填尾帧时和 compose 结果相同。
    PromptBundle compose_end(const models::Shot& shot) const;

    /// 给视频模型的运动描述。**它只管动作，不重复外观**——
    /// 重复的话运动模型会试图重新"画"一遍角色，
    /// 而首帧已经把长相定死了，两者打架的结果是脸在动的过程中变形。
    ///
    /// **按 MiniMax-H3 的时间码格式**（2026-09-14 起）：官方的提示词改写器
    /// 就是把整镜写成 `[0-2秒] …… [2-5秒] ……` 这样的段。分镜模型照规则
    /// 写了时间码的话，**运镜词不再前插**（规则要它自己把运镜名、幅度、
    /// 速度写进段里，前插一次就是「镜头缓慢推近, 缓慢推近」），角色的动作
    /// 并进**第一段**（它是这一镜的起始状态，接在末尾会被读成最后一段的
    /// 事）。没写时间码的整镜当一段：运镜词 + 描述 + 动作，前面补
    /// `[0-时长秒]`。
    std::string motion_prompt(const models::Shot& shot) const;

    /// 这部电影是写实线还是动画线。
    ///
    /// 暴露出来是因为**后端那一层也要用它拼提示词**，而后端拿不到资产库。
    /// 原来两个视频后端都是写死 REALISTIC，动画线的项目在那一步
    /// 会用错分隔符（见 video_positive）。
    models::StyleLine style_line() const { return style_line_; }

private:
    /// compose / compose_end 的公共实现，`picture` 是画面描述那一层。
    PromptBundle compose_with(const models::Shot& shot,
                              const std::string& picture) const;
    /// 项目页「负向」框里用户自己加的那截（去掉读资产库时补进去的默认串）。
    std::string project_negative_extras() const;

    models::AssetLibrary assets_;
    models::StyleLine style_line_;
    std::string sep_;
};

/// 景别、机位、运镜的中文说法。写实线用自然语言，模型对中文景别词有反应。
///
/// 单独暴露是为了能测：表里少一项的话，那个景别拼出来是空的，
/// 而空的景别层意味着模型自己决定构图——同一章里景别会乱跳。
const std::string& shot_size_zh(models::ShotSize v);
const std::string& angle_zh(models::CameraAngle v);
const std::string& move_zh(models::CameraMove v);
/// 焦段。AUTO 是空串（没填就一个字不加）。
const std::string& lens_zh(models::Lens v);

/// 这一章里哪几镜**一张参考图都拿不到**。按 shot_id 回，顺序照分镜表。
///
/// **为什么要在开跑之前问这一句。** 首帧那一族 2026-09-15 起是
/// Qwen-Image-Edit——图像**编辑**模型，手上没有编辑源时退化成文生图，出来
/// 的是彩色噪点。而闸门拦不住它：那东西的方差比真图还大，「不是空图」那条
/// 判据一路绿灯（tools/fetch_models.sh 上记着这台机器上栽过的那次）。
/// 一镜两分钟、一章二十几镜——跑完再说就太晚了。
///
/// **只有收参考图的模型才在乎**（`ModelsConfig::accepts_reference_images`）：
/// 纯文生图的本来就不传，缺不缺都一样。调用方先判那一条再来问这个。
///
/// **大特写（ECU）不算在内。** 那一档是**故意**不带参考图的——见 compose
/// 里 insert_shot 那段：带上的话半个房间会被拉进一个特写里。它没有"补一张
/// 图就能解决"的办法，算进来等于让有大特写的那一章永远出不来。
///
/// 角色或场景没注册的镜头也不算：那是另一条错，出图那一步会原样抛
/// `RenderError`，说的话也不一样。
std::vector<std::string> shots_without_refs(
    const std::vector<models::Shot>& shots,
    const models::AssetLibrary& assets);

}  // namespace changji::stages
