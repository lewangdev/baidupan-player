<p align="right">
  <a href="README.md">English</a> · <strong>简体中文</strong>
</p>

<h1 align="center">云盘随身听</h1>

<p align="center">
  <strong>把百度网盘里的音乐，装进一枚随身工牌。</strong><br>
  运行在 FoloToy AI Passport(ESP32-C3,无 PSRAM)上的 MP3/WAV 流式播放固件。
</p>

---

## 功能

- **直接播放网盘音频**:不占本地存储,不需要手机 App。可按文件夹浏览,也可按修改时间列出网盘内全部 MP3/WAV。
- **扫码登录**:屏幕显示二维码,手机登录百度账号确认即可;授权保存在设备上并自动续期。
- **网页配网**:首次开机自动开启热点 `BaiduPlayer-XXXX`,扫屏幕二维码连上后在弹出的网页里选网络、填密码。
- **连续播放**:自动播放下一首并加载下一页;下载中断后按 HTTP Range 断点续传;音量重启后保留。
- **中文文件名**:内置字库覆盖 GB2312 全部 7445 个字符。
- **三键操作**:专为 240×320 圆角屏设计;30 秒无操作自动熄屏,音乐继续播放。

## 快速开始

1. **准备百度凭据**:在[百度网盘开放平台](https://pan.baidu.com/union/console)创建应用,把
   `main/bp_baidu_keys.example.h` 复制为 `main/bp_baidu_keys.h`,填入 AppKey 与 SecretKey。
   该文件已被 Git 忽略,切勿提交。
2. **编译烧录**(ESP-IDF 5.5.3):

   ```bash
   idf.py set-target esp32c3
   idf.py build
   idf.py -p <端口> flash          # 首次安装
   idf.py -p <端口> app-flash      # 之后更新,保留 Wi-Fi 与授权
   ```

   完整门禁 `./tools/validate.sh` 会运行仓库检查、主机测试并生成校验过的合并镜像。
3. **连接 Wi-Fi**:手机扫屏幕二维码加入热点 `BaiduPlayer-XXXX`,配网网页会自动弹出(或访问
   `http://192.168.4.1`)。仅支持 2.4 GHz。
4. **授权并播放**:首页选择“全部音频”或“浏览网盘”,扫码授权后选中歌曲按 OK 即可播放。

> 用你自己的凭据编译出的固件内含 SecretKey,不要公开 `build/` 目录下的产物。

## 按键

| 页面 | 上 / 下 | OK | 长按 OK | 长按上 / 下 |
| --- | --- | --- | --- | --- |
| 首页 | 选择 | 进入 | 回到播放页 | 长按下:熄屏 |
| 列表 | 选择 | 打开文件夹 / 播放 / 翻页 | 上一级 / 首页 | 快速移动 5 行 |
| 播放 | 音量 ±10 | 暂停 / 继续 | 返回 | 上一首 / 下一首 |
| 无线网络 | — | 开启配网热点 | 返回 | — |

## 支持的格式

| 格式 | 说明 |
| --- | --- |
| MP3 | MPEG-1/2 Layer III,CBR 与 VBR,任意采样率(Helix 解码) |
| WAV | 16 位 PCM,单/双声道,8–48 kHz |

板载单喇叭,双声道下混为单声道。FLAC、AAC/M4A、OGG 会显示但置灰。暂不支持拖动进度;
播放过程中不能读取新列表,因为设备内存放不下两条 TLS 连接。

## 工作原理

```text
百度 OAuth(设备码) ─► xpan list / categorylist ─► filemetas 下载直链
                                                       │
控制任务 ─► 下载任务(HTTPS、302 跳转、Range 续传) ─► 20 KB 环形缓冲
                                                       │
                        解码任务(Helix MP3 / WAV) ─► 单声道 PCM ─► ES8311 / I2S
```

| 模块 | 职责 |
| --- | --- |
| [`main/bp_baidu.c`](main/bp_baidu.c) | 设备码授权、加锁的单次刷新令牌、目录与音频列表、下载直链 |
| [`main/bp_player.c`](main/bp_player.c) | 流式播放流水线、播放列表、自动续播、分阶段内存日志 |
| [`main/bp_ui.c`](main/bp_ui.c) | LVGL 页面,启动时一次创建,切页只切换可见性 |
| [`main/bp_wifi.cc`](main/bp_wifi.cc) | STA 自动回连与 SoftAP 强制门户配网 |
| [`main/bp_media.c`](main/bp_media.c) | 带主机测试的纯逻辑:格式、ID3v2、WAV 头、路径 |

ESP32-C3 全部可用堆约 180 KB。实测播放时:每首开始前空闲约 106 KB,CDN 的 TLS 连接建立后约
52 KB,解码中最低约 20 KB,每首结束后完全回收。详细说明、串口命令与限制见
[docs/baidupan-player.zh_CN.md](docs/baidupan-player.zh_CN.md)。

## 致谢

- 基于 [FoloToy AI Passport](https://github.com/FoloToy/ai-passport)(板级支持、构建与校验工具),
  其模板文档保留在 [docs/README.zh_CN.md](docs/README.zh_CN.md)。
- 百度网盘接入参考 [netdisk-recording-badge](https://github.com/openbrt/netdisk-recording-badge)。
- Wi-Fi 与配网门户来自 [esp-wifi-connect](https://github.com/78/esp-wifi-connect),MP3 解码来自
  [libhelix-mp3](https://components.espressif.com/components/chmorgan/esp-libhelix-mp3),中文字体为思源黑体(SIL OFL)。

以 [MIT 许可证](LICENSE)发布。
