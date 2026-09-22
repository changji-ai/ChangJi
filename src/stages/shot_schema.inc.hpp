// Shot 的 JSON Schema，当年由 pydantic 从 Python 侧的字段定义导出、原样嵌进来。
// **现在直接改这里**：导出它的那套 Python 早删了；今天的 tools/gen_prompts.py
// 管的是 prompts.toml（提示词），不生成这个文件。
// llm_shot_schema() 在这上面做删字段和收紧枚举的加工。改了 Shot 的定义，
// 这里要跟着手改。

#pragma once

namespace changji::stages::prompt {

inline constexpr const char* kShotSchemaJson =
    R"CJ({
 "$defs": {
  "CameraAngle": {
   "enum": [
    "low",
    "eye_level",
    "high",
    "overhead",
    "dutch"
   ],
   "title": "CameraAngle",
   "type": "string"
  },
  "CameraMove": {
   "enum": [
    "static",
    "pan_left",
    "pan_right",
    "tilt_up",
    "tilt_down",
    "push_in",
    "pull_out",
    "handheld",
    "orbit"
   ],
   "title": "CameraMove",
   "type": "string"
  },
  "CharacterInShot": {
   "additionalProperties": false,
   "description": "角色在本镜头中的表现。\n\n只有可变项。外观描述属于角色资产，不在这里，也不允许大模型在这里写。",
   "properties": {
    "char_id": {
     "description": "角色 id，必须是已注册角色之一",
     "title": "Char Id",
     "type": "string"
    },
    "expression": {
     "default": "",
     "description": "表情，如 愕然、隐忍",
     "maxLength": 40,
     "title": "Expression",
     "type": "string"
    },
    "action": {
     "default": "",
     "description": "本镜动作，如 后退半步",
     "maxLength": 80,
     "title": "Action",
     "type": "string"
    },
    "wardrobe_state": {
     "default": "default",
     "description": "服装状态 id，如 suit_torn",
     "maxLength": 40,
     "title": "Wardrobe State",
     "type": "string"
    },
    "face_pose": {
     "$ref": "#/$defs/FacePose",
     "default": "front",
     "description": "面部朝向"
    },
    "screen_pos": {
     "default": "center",
     "description": "画面位置：left / center / right",
     "title": "Screen Pos",
     "type": "string"
    }
   },
   "required": [
    "char_id"
   ],
   "title": "CharacterInShot",
   "type": "object"
  },
  "DialogueLine": {
   "additionalProperties": false,
   "description": "一句台词。时长字段由配音阶段回填，分镜阶段不填。",
   "properties": {
    "char_id": {
     "anyOf": [
      {
       "type": "string"
      },
      {
       "type": "null"
      }
     ],
     "default": null,
     "description": "说话角色。为空表示旁白",
     "title": "Char Id"
    },
    "text": {
     "maxLength": 200,
     "minLength": 1,
     "title": "Text",
     "type": "string"
    },
    "emotion": {
     "default": "neutral",
     "description": "情绪标签",
     "title": "Emotion",
     "type": "string"
    },
    "emotion_intensity": {
     "default": 0.5,
     "maximum": 1.0,
     "minimum": 0.0,
     "title": "Emotion Intensity",
     "type": "number"
    },
    "voice_id": {
     "anyOf": [
      {
       "type": "string"
      },
      {
       "type": "null"
      }
     ],
     "default": null,
     "description": "音色 id，由角色资产决定",
     "title": "Voice Id"
    },
    "audio_path": {
     "anyOf": [
      {
       "type": "string"
      },
      {
       "type": "null"
      }
     ],
     "default": null,
     "description": "相对项目根的路径",
     "title": "Audio Path"
    },
    "actual_duration_s": {
     "anyOf": [
      {
       "minimum": 0,
       "type": "number"
      },
      {
       "type": "null"
      }
     ],
     "default": null,
     "title": "Actual Duration S"
    }
   },
   "required": [
    "text"
   ],
   "title": "DialogueLine",
   "type": "object"
  },
  "Lens": {
   "description": "焦段。auto = 没填。",
   "enum": [
    "auto",
    "wide",
    "normal",
    "portrait",
    "tele"
   ],
   "title": "Lens",
   "type": "string"
  },
  "FacePose": {
   "description": "角色面部朝向。口型判定用。",
   "enum": [
    "front",
    "three_quarter",
    "profile",
    "back",
    "off_screen"
   ],
   "title": "FacePose",
   "type": "string"
  },
  "ShotSize": {
   "description": "景别。中景打头，其余由近到远。",
   "enum": [
    "MS",
    "ECU",
    "CU",
    "MCU",
    "MLS",
    "LS",
    "ELS"
   ],
   "title": "ShotSize",
   "type": "string"
  },
  "ShotStatus": {
   "description": "镜头在流水线上的位置。断点续跑靠它。",
   "enum": [
    "planned",
    "audio_done",
    "frame_done",
    "draft_done",
    "draft_rejected",
    "final_done",
    "final_rejected",
    "fallback",
    "locked"
   ],
   "title": "ShotStatus",
   "type": "string"
  },
  "Transition": {
   "enum": [
    "cut",
    "dissolve",
    "fade_in",
    "fade_out",
    "whip"
   ],
   "title": "Transition",
   "type": "string"
  }
 },
 "additionalProperties": false,
 "description": "一个镜头。",
 "properties": {
  "shot_id": {
   "description": "全局唯一，如 ep01_s03_sh007",
   "pattern": "^[a-z0-9_]+$",
   "title": "Shot Id",
   "type": "string"
  },
  "scene_id": {
   "pattern": "^[a-z0-9_]+$",
   "title": "Scene Id",
   "type": "string"
  },
  "order": {
   "description": "章内顺序",
   "minimum": 0,
   "title": "Order",
   "type": "integer"
  },
  "visual_desc": {
   "default": "",
   "description": "给人看的中文描述",
   "maxLength": 300,
   "title": "Visual Desc",
   "type": "string"
  },
  "first_frame_prompt": {
   "default": "",
   "maxLength": 1200,
   "title": "First Frame Prompt",
   "type": "string"
  },
  "last_frame_prompt": {
   "anyOf": [
    {
     "maxLength": 1200,
     "type": "string"
    },
    {
     "type": "null"
    }
   ],
   "default": null,
   "description": "为空则走单帧图生视频",
   "title": "Last Frame Prompt"
  },
  "motion_prompt": {
   "default": "",
   "description": "运动描述，给视频模型",
   "maxLength": 400,
   "title": "Motion Prompt",
   "type": "string"
  },
  "negative_prompt": {
   "default": "",
   "title": "Negative Prompt",
   "type": "string"
  },
  "shot_size": {
   "$ref": "#/$defs/ShotSize",
   "default": "MS"
  },
  "camera_angle": {
   "$ref": "#/$defs/CameraAngle",
   "default": "eye_level"
  },
  "camera_move": {
   "$ref": "#/$defs/CameraMove",
   "default": "static"
  },
  "lens": {
   "$ref": "#/$defs/Lens",
   "default": "auto"
  },
  "lighting": {
   "default": "",
   "description": "这一镜的光：时段、光源、方向、软硬",
   "maxLength": 80,
   "title": "Lighting",
   "type": "string"
  },
  "continuous_with_prev": {
   "default": false,
   "description": "紧接上一镜的动作（同一场景、同一时刻、动作连续）",
   "title": "Continuous With Prev",
   "type": "boolean"
  },
  "camera_id": {
   "anyOf": [
    {
     "type": "string"
    },
    {
     "type": "null"
    }
   ],
   "default": null,
   "description": "复用机位 id。同场景同机位保证不越轴",
   "title": "Camera Id"
  },
  "characters": {
   "items": {
    "$ref": "#/$defs/CharacterInShot"
   },
   "title": "Characters",
   "type": "array"
  },
  "location_id": {
   "anyOf": [
    {
     "type": "string"
    },
    {
     "type": "null"
    }
   ],
   "default": null,
   "title": "Location Id"
  },
  "prop_ids": {
   "items": {
    "type": "string"
   },
   "title": "Prop Ids",
   "type": "array"
  },
  "duration_s": {
   "default": 5.0,
   "description": "镜头时长",
   "exclusiveMinimum": 0,
   "maximum": 30,
   "title": "Duration S",
   "type": "number"
  },
  "duration_locked": {
   "default": false,
   "description": "真表示已由配音时长反推锁定，不可再调",
   "title": "Duration Locked",
   "type": "boolean"
  },
  "dialogue": {
   "items": {
    "$ref": "#/$defs/DialogueLine"
   },
   "title": "Dialogue",
   "type": "array"
  },
  "sfx": {
   "items": {
    "type": "string"
   },
   "title": "Sfx",
   "type": "array"
  },
  "bgm_cue": {
   "anyOf": [
    {
     "type": "string"
    },
    {
     "type": "null"
    }
   ],
   "default": null,
   "title": "Bgm Cue"
  },
  "needs_lipsync": {
   "default": false,
   "description": "由 derive_needs_lipsync 规则推导，不要让模型填",
   "title": "Needs Lipsync",
   "type": "boolean"
  },
  "transition_in": {
   "$ref": "#/$defs/Transition",
   "default": "cut"
  },
  "transition_dur_s": {
   "default": 0.0,
   "maximum": 2.0,
   "minimum": 0,
   "title": "Transition Dur S",
   "type": "number"
  },
  "subtitle_text": {
   "default": "",
   "title": "Subtitle Text",
   "type": "string"
  },
  "beat": {
   "default": "",
   "description": "叙事功能，如 反转、铺垫",
   "maxLength": 20,
   "title": "Beat",
   "type": "string"
  },
  "continuity_notes": {
   "default": "",
   "maxLength": 200,
   "title": "Continuity Notes",
   "type": "string"
  },
  "missing_info": {
   "description": "模型自报的信息缺口，供校验阶段检查",
   "items": {
    "type": "string"
   },
   "title": "Missing Info",
   "type": "array"
  },
  "status": {
   "$ref": "#/$defs/ShotStatus",
   "default": "planned"
  },
  "attempts": {
   "default": 0,
   "minimum": 0,
   "title": "Attempts",
   "type": "integer"
  },
  "frame_path": {
   "anyOf": [
    {
     "type": "string"
    },
    {
     "type": "null"
    }
   ],
   "default": null,
   "title": "Frame Path"
  },
  "end_frame_path": {
   "anyOf": [
    {
     "type": "string"
    },
    {
     "type": "null"
    }
   ],
   "default": null,
   "title": "End Frame Path"
  },
  "video_path": {
   "anyOf": [
    {
     "type": "string"
    },
    {
     "type": "null"
    }
   ],
   "default": null,
   "title": "Video Path"
  },
  "gate_notes": {
   "items": {
    "type": "string"
   },
   "title": "Gate Notes",
   "type": "array"
  }
 },
 "required": [
  "shot_id",
  "scene_id",
  "order"
 ],
 "title": "Shot",
 "type": "object"
})CJ";

// 分镜阶段允许大模型填的字段。外观类字段不在其中，这是刻意的。
//
// 2026-09-16 拿掉了六个**没人读**的字段：camera_id（没有任何越轴校验）、
// transition_in / transition_dur_s（装配是纯硬切，转场从没渲染过）、
// subtitle_text（成片字幕来自台词，不读它）、continuity_notes（只回显）、
// missing_info（零消费者）。模型填了也白填，还占着提示词和 GBNF。结构体里
// 那几栏留着（盘上格式），只是模型再也见不到。
//
// 2026-09-17 又拿掉 `visual_desc`，理由是**实测它一次都没被填过**：
//
//   | 项目 | 镜头 | visual_desc 是空的 |
//   |---|---|---|
//   | 互联测试剧（glm） | 142 | 108（76%） |
//   | 横屏测试剧（deepseek） | 151 | 151（100%） |
//
// 根子还是那一条：**它不在 `required` 里，不在表里的字段模型整个略过**
//（同一条机制 2026-09-13 从 198 镜里查出来过）。而它唯一的下游——按场拆镜
// 时拿上一镜当尾巴（`storyboard_run.cpp` 的 prev_tail）——本来就写着"空了
// 就退回 first_frame_prompt"，也就是说**今天百分之百走的是那条退路**。
// 留在 schema 里只是让每一次调用多背 125 个字符。
//
// 结构体里那一栏留着：界面的镜头抽屉里能手改，存量数据也还有。
inline constexpr const char* kLlmShotFields[] = {
    "shot_id",
    "scene_id",
    "order",
    "first_frame_prompt",
    "last_frame_prompt",
    "motion_prompt",
    "shot_size",
    "camera_angle",
    "camera_move",
    "lens",
    "lighting",
    "continuous_with_prev",
    "characters",
    "location_id",
    "duration_s",
    "dialogue",
    "beat",
};

/// beat 这一栏的枚举：这一镜在戏里干什么。**进 required、收成枚举**——
/// 2026-09-16 之前它是自由文本、不在 required 里、提示词里一个字没提，
/// 而关键镜判定（render.cpp 的 is_hero_shot）和景别兜底（diversify_shot_sizes
/// 的「情绪那一下才给 CU」）都靠它，等于两处死代码。
inline constexpr const char* kBeatKinds[] = {
    "铺垫", "推进", "对峙", "反应", "反转", "高潮", "钩子", "留白",
};

}  // namespace changji::stages::prompt
