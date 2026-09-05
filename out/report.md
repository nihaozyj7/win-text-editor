# ZNote 介绍/宣传视频 — 制作报告

## 产出
- **成品**: `out/ZNote_intro.mp4`（1920×1080, 30fps, 约 46 秒, 1.6MB, 无音频）
- 结构：
  1. 0:00 片头字卡（ZNote · Windows 记事本风格文本编辑器）
  2. 0:05 语法高亮 Ken Burns 运镜（真实界面静帧）
  3. 0:13 64MB / 50 万行大文件：PageDown 滚动 + Ctrl+End 直达末尾（真实录屏）
  4. 0:23 实时输入、Emoji 彩色渲染（真实录屏）
  5. 0:33 撤销演示（真实录屏）
  6. 0:41 片尾字卡

## 素材与假设
- 无现成素材，全部素材为本机实拍：启动 `build/editor.exe`，用 ffmpeg gdigrab 以 2560×1440 原生分辨率录制真实操作（`scene_big_raw.mp4`、`scene_type_raw.mp4`、`still_code.png`）。
- 演示文件为自动生成的 `out/demo/big_demo.txt`（64MB）与 `out/demo/highlight_demo.cpp`。
- 品牌名取 "ZNote"（源自演示代码注释），如需改名可重渲字卡。
- 假设视频无配乐/旁白（环境无 TTS 通道挂载）；如需配音或背景音乐可后续叠加。

## 已解决问题
- 中文字幕首版用 'Microsoft YaHei Bold' 字体名渲染成豆腐块，改为 fontconfig 名 'Microsoft YaHei' 后正常。
- 大文件场景首录时窗口未最大化，已重录；剪入时避开窗口切换过渡帧（从原始录制 6.2s 起）。

## 遗留风险
- 无音频轨（QC 的 silence 警告为预期行为）。
- 录屏中任务栏可见；已用字幕条弱化，如需纯窗口录制可改 gdigrab window 定位。
