# 语言图标生成脚本：SVG 模板 → Edge 无头渲染 PNG → PIL 合成 .ico
# 用法：在仓库根目录执行  python resources/icons/make_icons.py
# 依赖：Edge（渲染 SVG）、Pillow（合成 ICO）；产物写入 resources/icons/*.ico
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT_PNG = os.path.join(HERE, "png")

EDGE_CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]

# (文件名, 徽标缩写, 徽标底色)——底色取各语言官方识别色的暗化版本（白字可读）
LANGS = [
    ("cpp",     "C++", "#00599C"),
    ("csharp",  "C#",  "#68217A"),
    ("java",    "J",   "#E76F00"),
    ("js",      "JS",  "#B7950B"),
    ("ts",      "TS",  "#3178C6"),
    ("go",      "GO",  "#00919C"),
    ("rust",    "RS",  "#CE422B"),
    ("swift",   "SW",  "#F05138"),
    ("php",     "PHP", "#777BB4"),
    ("python",  "PY",  "#3776AB"),
    ("json",    "{}",  "#8CB445"),
    ("markdown","MD",  "#519ABA"),
    ("config",  "CFG", "#B84545"),
    ("html",    "<>",  "#E34C26"),
    ("css",     "CSS", "#1572B6"),
    ("sql",     "SQL", "#336791"),
    ("log",     "LOG", "#6E6E6E"),
    ("text",    "TXT", "#8C8C8C"),
]

SVG_TMPL = """<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 64 64">
  <!-- 语法高亮语言图标：{label}（生成自 make_icons.py，请勿手改） -->
  <defs>
    <linearGradient id="paper" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#3C3C3C"/>
      <stop offset="1" stop-color="#252526"/>
    </linearGradient>
    <linearGradient id="fold" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="{accent}"/>
      <stop offset="1" stop-color="{accent}"/>
    </linearGradient>
  </defs>
  <path d="M12 4h30l12 12v44a2 2 0 0 1-2 2H14a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2z"
        fill="url(#paper)" stroke="#858585" stroke-width="1.5"/>
  <path d="M42 4v10a2 2 0 0 0 2 2h10z" fill="url(#fold)" stroke="#858585" stroke-width="1.5"/>
  <g stroke-linecap="round" stroke-width="3" fill="none" opacity="0.55">
    <line x1="18" y1="22" x2="34" y2="22" stroke="#D4D4D4"/>
    <line x1="18" y1="30" x2="42" y2="30" stroke="#D4D4D4"/>
    <line x1="18" y1="38" x2="28" y2="38" stroke="#D4D4D4"/>
  </g>
  <rect x="14" y="44" width="36" height="14" rx="3" fill="{accent}"/>
  <text x="32" y="54.2" font-family="'Segoe UI','Microsoft YaHei',Arial,sans-serif"
        font-weight="700" font-size="{fs}" fill="#FFFFFF"
        text-anchor="middle">{label}</text>
</svg>
"""


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def make_svg(name, label, accent):
    n = len(label)
    fs = 11 if n >= 4 else (13 if n == 3 else 17)
    svg = SVG_TMPL.format(label=esc(label), accent=accent, fs=fs)
    path = os.path.join(HERE, name + ".svg")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(svg)


def edge_png(edge, svg_path, png_path):
    url = "file:///" + svg_path.replace("\\", "/")
    subprocess.run(
        [edge, "--headless", "--disable-gpu", "--default-background-color=00000000",
         "--window-size=256,256", "--screenshot=" + png_path, url],
        check=True, capture_output=True, timeout=60)


def main():
    edge = next((p for p in EDGE_CANDIDATES if os.path.exists(p)), None)
    if not edge:
        sys.exit("未找到 Edge，无法渲染 SVG")
    os.makedirs(OUT_PNG, exist_ok=True)

    for name, label, accent in LANGS:
        make_svg(name, label, accent)
        svg = os.path.join(HERE, name + ".svg")
        png = os.path.join(OUT_PNG, name + ".png")
        edge_png(edge, svg, png)
        print("svg+png:", name)

    # 隐藏 Edge 无头模式的临时配置目录提示；PNG → ICO（含 16/32/48/256）
    from PIL import Image
    for name, _, _ in LANGS:
        png = os.path.join(OUT_PNG, name + ".png")
        img = Image.open(png).convert("RGBA")
        ico = os.path.join(HERE, name + ".ico")
        img.save(ico, format="ICO",
                 sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (256, 256)])
        print("ico:", name)


if __name__ == "__main__":
    main()
