import subprocess, shlex

FONT = "font='Microsoft YaHei'"
FONT_B = "font='Microsoft YaHei Bold'"

def run(args):
    r = subprocess.run(args, capture_output=True, text=True, shell=True)
    if r.returncode: print(r.stderr[-1500:]); raise SystemExit(1)

def caption(txt):
    return (f"drawbox=x=0:y=ih-130:w=iw:h=130:color=0x101418@0.75:t=fill,"
            f"drawtext={FONT_B}:text='{txt}':fontsize=52:fontcolor=0xE8EAED:x=(w-text_w)/2:y=h-105")

def enc(extra=""):
    return f"-c:v libx264 -preset medium -crf 19 -pix_fmt yuv420p -r 30 {extra}"

# 1. 片头 5s
run("ffmpeg -hide_banner -y -f lavfi -i color=c=0x101418:s=1920x1080:d=5:r=30 -vf \""
    f"drawtext={FONT_B}:text='ZNote':fontsize=180:fontcolor=0xE8EAED:x=(w-text_w)/2:y=330,"
    f"drawtext={FONT}:text='Windows 记事本风格文本编辑器':fontsize=56:fontcolor=0x9AA0A6:x=(w-text_w)/2:y=590,"
    f"drawtext={FONT}:text='C++20  ·  Direct2D/DirectWrite  ·  为超大文件而生':fontsize=36:fontcolor=0x5F6368:x=(w-text_w)/2:y=690,"
    "fade=t=in:st=0:d=0.6,fade=t=out:st=4.2:d=0.8,format=yuv420p\" " + enc() + " title.mp4")

# 2. 语法高亮 Ken Burns 8s
run("ffmpeg -hide_banner -y -loop 1 -t 8 -i still_code.png -vf \""
    "scale=2112:1188,zoompan=z='min(zoom+0.00045,1.10)':x='iw/2-(iw/zoom/2)':y='ih/2-(ih/zoom/2)':d=1:s=1920x1080:fps=30,"
    + caption("语法高亮 · 中英混排 · 关键字分色") + ",fade=t=in:st=0:d=0.5,fade=t=out:st=7.2:d=0.8,format=yuv420p\" "
    + enc() + " seg_code.mp4")

# 3. 大文件 12s (3.2~15.2)
run("ffmpeg -hide_banner -y -ss 3.2 -t 12 -i scene_big_raw.mp4 -vf \""
    "scale=1920:1080," + caption("64MB · 50 万行 —— 秒开，Ctrl+End 直达文件末尾") +
    ",fade=t=in:st=0:d=0.5,fade=t=out:st=11.2:d=0.8,format=yuv420p\" " + enc() + " seg_big.mp4")

# 4. 输入+Emoji 10s (8~18)
run("ffmpeg -hide_banner -y -ss 8 -t 10 -i scene_type_raw.mp4 -vf \""
    "scale=1920:1080," + caption("实时编辑 · Emoji 彩色渲染 · 多编码自动识别") +
    ",fade=t=in:st=0:d=0.5,fade=t=out:st=9.2:d=0.8,format=yuv420p\" " + enc() + " seg_type.mp4")

# 5. 撤销 8s (26~34)
run("ffmpeg -hide_banner -y -ss 26 -t 8 -i scene_type_raw.mp4 -vf \""
    "scale=1920:1080," + caption("1000 步撤销 / 重做 · PieceTable 编辑内核") +
    ",fade=t=in:st=0:d=0.5,fade=t=out:st=7.2:d=0.8,format=yuv420p\" " + enc() + " seg_undo.mp4")

# 6. 片尾 6s
run("ffmpeg -hide_banner -y -f lavfi -i color=c=0x101418:s=1920x1080:d=6:r=30 -vf \""
    f"drawtext={FONT_B}:text='轻量 · 快速 · 流畅':fontsize=120:fontcolor=0xE8EAED:x=(w-text_w)/2:y=380,"
    f"drawtext={FONT}:text='ZNote — 让大文件编辑像记事本一样简单':fontsize=52:fontcolor=0x8AB4F8:x=(w-text_w)/2:y=600,"
    "fade=t=in:st=0:d=0.6,fade=t=out:st=5.1:d=0.9,format=yuv420p\" " + enc() + " outro.mp4")

# 拼接
with open("concat.txt","w") as f:
    for n in ["title.mp4","seg_code.mp4","seg_big.mp4","seg_type.mp4","seg_undo.mp4","outro.mp4"]:
        f.write(f"file '{n}'\n")
run("ffmpeg -hide_banner -y -f concat -safe 0 -i concat.txt -c copy ZNote_intro.mp4")
print("DONE")
