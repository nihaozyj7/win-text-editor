FF="ffmpeg -y -hide_banner -loglevel error"
FONT_B="C\:/Windows/Fonts/msyhbd.ttc"
FONT="C\:/Windows/Fonts/msyh.ttc"
$FF -f lavfi -i color=c=0x101418:s=1920x1080:d=5:r=30 -vf "
drawtext=fontfile=$FONT_B:text='ZNote':fontsize=170:fontcolor=0xE8EAED:x=(w-text_w)/2:y=340,
drawtext=fontfile=$FONT:text='Windows 记事本风格文本编辑器':fontsize=54:fontcolor=0x9AA0A6:x=(w-text_w)/2:y=580,
drawtext=fontfile=$FONT:text='C++20  ·  Direct2D/DirectWrite  ·  为超大文件而生':fontsize=36:fontcolor=0x5F6368:x=(w-text_w)/2:y=680,
fade=t=in:st=0:d=0.6,fade=t=out:st=4.3:d=0.7" -c:v libx264 -preset medium -pix_fmt yuv420p title.mp4
$FF -f lavfi -i color=c=0x101418:s=1920x1080:d=6:r=30 -vf "
drawtext=fontfile=$FONT_B:text='轻量 · 快速 · 流畅':fontsize=110:fontcolor=0xE8EAED:x=(w-text_w)/2:y=400,
drawtext=fontfile=$FONT:text='ZNote — 让大文件编辑像记事本一样简单':fontsize=48:fontcolor=0x8AB4F8:x=(w-text_w)/2:y=600,
fade=t=in:st=0:d=0.6,fade=t=out:st=5.2:d=0.8" -c:v libx264 -preset medium -pix_fmt yuv420p outro.mp4
