<p align="right">
  <strong>简体中文</strong> · <a href="baidupan-player.md">English</a>
</p>

# 云盘随身听

在 FoloToy AI Passport 上流式播放百度网盘里的 MP3 / WAV 音频。百度网盘的接入方式
(设备码扫码授权、令牌刷新、xpan 接口)参考了
[netdisk-recording-badge](https://github.com/openbrt/netdisk-recording-badge)。

## 准备百度网盘应用凭据

1. 在[百度网盘开放平台](https://pan.baidu.com/union/console)完成开发者认证并创建应用,
   记下 AppKey 与 SecretKey。
2. 复制 `main/bp_baidu_keys.example.h` 为 `main/bp_baidu_keys.h`(已被 Git 忽略),填入凭据后重新编译。
   缺少该文件时固件用占位符编译,设备无法完成授权。

## 上手

1. 配网(仅 2.4 GHz):首次开机没有已存网络时,设备自动开启热点 `BaiduPlayer-XXXX` 并显示二维码。
   手机扫码连上热点后会弹出配网网页(未弹出就访问 `http://192.168.4.1`),选择网络并输入密码;
   设备试连成功才保存,完成后自动关闭热点并联网。之后可在“设置 → 无线网络”按 OK 再次开启热点。
   可保存多个网络,开机自动连接已存网络。也可用 USB 串口输入 `WIFI SET 网络名称|密码`。
2. 授权:首页选“全部音频”或“浏览网盘”,屏幕显示二维码;用手机扫码,登录百度账号并确认授权。
   授权保存在本机,只需一次;“设置 → 网盘账号”可退出。
3. 播放:“全部音频”按修改时间列出网盘内所有 MP3/WAV;“浏览网盘”按文件夹浏览。
   选中歌曲按 OK 播放,播完自动播放下一首,当前页播完会自动加载下一页。

## 按键

| 页面 | 上 / 下 | OK | 长按 OK | 长按上 / 下 |
| --- | --- | --- | --- | --- |
| 首页 | 选择 | 进入 | 回到播放页 | 长按下:熄屏 |
| 列表 | 选择 | 打开文件夹 / 播放 / 翻页 | 上一级 / 首页 | 快速移动 5 行 |
| 播放 | 音量 ±10 | 暂停 / 继续 | 返回 | 上一首 / 下一首 |
| 设置与信息页 | 选择 | 进入 / 执行 | 返回 | — |
| 配网热点 | — | — | 关闭热点并返回 | — |

30 秒无操作自动熄屏,音乐继续播放;熄屏时任意键只唤醒,不触发操作。

## 支持的格式与限制

- MP3(MPEG-1/2 Layer III,CBR/VBR,任意采样率)与 16 位 PCM WAV(单/双声道,8–48 kHz)。
  板载单喇叭,双声道下混为单声道。
- 暂不支持 FLAC、AAC/M4A、OGG 等;浏览网盘时只显示文件夹和可播放的文件,其余文件不显示。
- 暂不支持拖动进度。VBR MP3 的总时长按平均码率估算。
- 网络中断时按 HTTP Range 从断点续传;长时间暂停后服务器断开也会自动续上。
- 百度对非会员账号的下载限速可能影响高码率 WAV,MP3 通常不受影响。

## 实现概览

| 模块 | 职责 |
| --- | --- |
| `main/bp_baidu.c` | 设备码授权、令牌刷新(加锁,refresh_token 只能用一次)、目录与音频列表、下载直链 |
| `main/bp_player.c` | 控制任务 → 下载任务(跟随 302、Range 续传)→ 环形缓冲(按剩余内存 16–64 KiB)→ 解码任务(Helix MP3 / WAV)→ I2S |
| `main/bp_ui.c` | 全部页面在启动时一次创建,切页只切换可见性 |
| `main/bp_media.c` | 纯逻辑:格式识别、ID3v2 跳过、WAV 解析、时长、路径(主机测试 `tests/test_bp_media.c`) |
| `main/bp_console.c` | 串口命令:`WIFI SET/LIST/DEL/AP`、`BAIDU AUTH/STATUS/LOGOUT`、`STATE`、`KEY`、`UI` |

中文字体 `bp_font_16` 收录 GB2312 全部字符以显示任意文件名,由 `tools/generate_bp_fonts.py` 生成。
