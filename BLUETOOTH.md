# N3Pro 蓝牙双向音频 — 调研结论与 Rockbox 集成方案

状态：调研完成，方案已定，待实施。
所有"已确认"条目均来自原厂固件静态分析（rootfs @ `build/rootfs`）+ 实机（Rockbox 模式下）验证。

---

## 一、原厂机制（逆向结论）

### 1.1 硬件与内核

| 项 | 结论 |
|---|---|
| 芯片 | Broadcom **BCM4345C5**（WiFi/BT 二合一，BT 走 UART） |
| 传输 | `/dev/ttyS0`，3 Mbaud，H4 协议，硬件流控 |
| 固件 | `/lib/firmware/BCM4345C5.hcd`，由 `brcm_patchram_plus` 下载（`--enable_hci --baudrate 3000000 --no2bytes --tosleep=50000 --use_baudrate_for_download --enable_lpm`），BD_ADDR 来自 `sa_config bt_addr` |
| 内核支持 | BT 核/H4/BCSP/HCILL/L2CAP/SCO/RFCOMM/BNEP/HIDP **全部内建**（dmesg 确认），无需内核工作 |
| 电源 | `/sys/class/rfkill/rfkill0`（bluetooth）、`rfkill3`（hci0） |

### 1.2 用户态栈：BlueZ 4.101（HiBy 深度补丁版）

开机脚本 `S40bt_init` → `bt_init`（在 Rockbox 模式下**同样执行**，实测进程全在）：

```
dbus-daemon → rfkill on → brcm_patchram_plus → hciconfig hci0 up
→ bluetoothd → hciconfig hci0 reset → bt-agent & bt-monitor & bt-media
```

实测（Rockbox 运行中）：dbus、patchram、bluetoothd、bt-agent、bt-monitor、bt-media 全部存活，hci0 待机（DOWN，用时 up）。**即：Rockbox 侧零成本拥有完整 BT 栈。**

HiBy 对 bluetoothd 的补丁能力：
- A2DP SEP 内建 **APTX / UAT / LDAC**（`ldac_getcap_ind`、`aptx_getcap_ind`、`print_uat` 等）
- **AVRCP uinput**：耳机按键 → `/dev/uinput` 虚拟输入设备 → 内核 evdev 事件（`init_uinput`、"AVRCP: uinput initialized"）→ Rockbox 按键驱动可直接读取！
- **AVRCP 元数据**：`parse_player_metadata`/`get_metadata`/`list_metadata`/`avrcp_player_event`，原厂播放器有 `title/genre/album/artist` 显示（`"%s btaddr = %s title = %s …"`）

### 1.3 音频路径（双向）

原厂 ALSA 插件 `libasound_module_pcm_bluetooth.so`（BlueZ4 ioplug，经 D-Bus 与 bluetoothd 通信）：

**发送（连蓝牙耳机，本机 = A2DP Source）：**
- `bt-connect --device <mac> --audiosink`（org.bluez.AudioSink 连远端）
- 原厂播放器连接后把配置**运行时写入 `/etc/asound.conf`**（文件里遗留的 `# device for bluetooth` 注释即占位符；strings 证据 `pcm.%s{`、`profile a2dp`）：
  ```
  pcm.XX { type bluetooth; device "MAC"; profile "a2dp" }
  ```
- 之后就是普通 ALSA PCM，播放即编码发送。编码器：SBC（插件内置）、LDAC+ABR（`libldacBT_enc.so`）、aptX（`libbt-aptx-bridge.so`）、AAC
- 编解码切换：`bt_codec LDAC_HQ|LDAC_SQ|LDAC_MQ|LDAC_ABR|APTX|AAC|UAT_*|SBC`（改写 audio.conf `DefaultCodec=`）+ `bt_ldac on|off`

**接收（手机推流，本机 = A2DP Sink）：**
- `bt-connect --device <mac> --audiosource`（连远端 AudioSource）
- 同一个 bluetooth PCM 用 **CAPTURE** 打开 → SBC 解码后的 PCM（插件含 `sbc_decode`）
- `/etc/bluetooth/audio.conf` 已开 `Enable=Source,Sink`，`LDACSinks=1, AACSinks=1, UATSinks=1`
- **RX 实际可用编解码：SBC 已确认**（插件带解码器）。AAC/LDAC/UAT 的 sink SEP 会注册，但插件符号只见 SBC 解码 + LDAC 编码；手机若坚持 AAC 可能协商失败 → 兜底 `AACSinks=0` 强制 SBC（**待 PoC 实测**，验证脚本 bt_recon6.sh 已备）

### 1.4 控制与元数据

| 功能 | 机制 | 原厂工具 |
|---|---|---|
| 耳机→本机按键 | AVRCP CT → bluetoothd TG → **uinput** → evdev | （内核事件，无需工具） |
| 本机→手机控制 | AVRCP CT 命令 | `bt-control --op play\|pause\|forward\|backward\|volumeup\|volumedown\|rewind\|fast_forward` |
| 标题/艺术家/专辑 | AVRCP 1.3 元数据（bluetoothd 已解析） | D-Bus 信号（需监听） |
| 封面 | ✗ 仅 HiBy 私有 UAT 编码 + 手机装 HiByApp 才有；标准 AVRCP 无封面 | — |
| 扫描/配对 | bt-agent 默认 PIN 0000 自动应答；耳机免 PIN | `hcitool scan` / `bt-adapter` |
| 配对持久化 | `/var/lib/bluetooth`（UBIFS 可写，重启保留）+ `/data/bt_list.txt` | — |

---

## 二、Rockbox 集成方案

总原则：**100% 复用原厂用户态栈**（它本来就在跑），Rockbox 只做「连接管理 + 音频路由 + UI」。全部新逻辑进独立文件 `firmware/target/hosted/cayin/n3pro/bt-n3pro.c`（符合 AGENTS.md 独立文件规则），共享代码仅少量 guard。

### 2.1 输出方向（Rockbox → 耳机）— 天然走完整管线

```
文件 → codec → DSP 完整链(EQ/Space'80/crossfeed/… 不变)
     → pcm-alsa.c → pcm.btout (bluez4 插件: SBC/LDAC 编码) → 耳机
```

- 连接成功后写 `/etc/asound.conf` 并 `pcm_alsa_set_playback_device("btout")`（现成 API，erosqnative 先例）
- 断开时还原 asound.conf、切回 `plughw:0,0`

### 2.2 输入方向（手机 → N3Pro 当解码耳放）— 显式过 DSP

采用 `firmware/usbstack/usb_audio.c`（Rockbox 自带 UAC 设备栈）的既有模式——它就是"外部 PCM → DSP → mixer"的官方先例（USB DAC 也正在按此改造，两者复用同一骨架）：

```
手机 → A2DP → bluetoothd(SBC 解码) → pcm.btin (CAPTURE)
   → 采集线程: S32/S16 → dsp_process(dsp_get_config(CODEC_IDX_AUDIO))
   → mixer 通道 → pcm-alsa → AK4493 本地口
```

- DSP 实例与正常播放共用 → EQ/Space'80 等设置自动生效（usb_audio.c 同款初始化：`DSP_RESET / STEREO_INTERLEAVED / SAMPLE_DEPTH 16 / SET_FREQUENCY`）
- 本地播放进行中时旁路 DSP（codec 线程独占该实例，`pcm_is_playing()` 判断）
- 音量：输出为本地口 → 现有 AK4493 硬件音量直接可用

### 2.3 音量模型（决议：单一系统音量，1:1 映射，不引入第二个音量概念）

- **输出(TX)**：`pcm.btout` 用 ALSA **softvol** 包一层，控件 `BtVol` 挂 card 0；蓝牙激活时 `audiohw_set_volume()` 的 CAYIN 分支改写 `BtVol`——与 AK4493 音量同一 0–100 档、同一 dB 曲线，按 Rockbox 音量键手感完全一致；断开蓝牙无感切回硬件音量。耳机端 AVRCP VOL+/-（uinput）同样映射 Rockbox 音量键，闭环。
  - 备选（softvol 挂 card 0 不可行时）：pcm_alsa 软件音量 或 AVRCP 步进
- **输入(RX)**：硬件音量（AK4493）即总音量；手机端电平建议拉满（SBC 流电平由手机音量决定），UI 提示；进阶可选：监听手机发来的 AVRCP `SetAbsoluteVolume`（bluetoothd 已有 `avrcp_set_volume`）1:1 联动本机音量。

### 2.4 蓝牙输入专用屏幕（决议：需要控制按钮）

- 新 `apps/bt_rx_screen.c`（FM 收音屏幕同款模式，CAYIN guard 进 apps/SOURCES）
- 触屏软按钮：**上一首 / 播放暂停 / 下一首**（+ 音量条），物理按键同映射
- 按键 → `bt-control --device <mac> --op …` 发给手机
- 显示：标题 / 艺术家 / 专辑（D-Bus 元数据监听）+ 编解码 / 采样率 / VU；**无封面**（见 1.4）
- D-Bus 监听：最小 libdbus 客户端（设备上有 `libdbus-1.so.3`；构建期用自写最小声明头，不引新依赖）

### 2.5 输出方向的耳机按键

button-n3pro.c 增加读取 uinput 设备（AVRCP 连接时出现的 `/dev/input/eventN`），映射：
`KEY_PLAYPAUSE→播放/暂停`、`KEY_NEXTSONG→下一首`、`KEY_PREVIOUSSONG→上一首`、`KEY_VOLUMEUP/DOWN→音量`，走现有 keymap。

### 2.6 联动与细节

- BT 输出激活时：强制 transistor（胆灯灭）、LED 沿用采样率色、状态栏蓝牙图标
- 编解码首版 SBC；LDAC 切换（`bt_codec` + bluetoothd 重启）二期
- 省电：锁屏/关屏 N 分钟可 `bt_done`（下次用时 `bt_init`）
- 配对列表持久化到 Rockbox 自己的配置目录（同时兼容读 `/data/bt_list.txt`）

### 2.7 侵入面

| 文件 | 改动 |
|---|---|
| `target/hosted/cayin/n3pro/bt-n3pro.c/.h` | **新增**，全部连接/路由/采集/D-Bus 逻辑 |
| `apps/bt_rx_screen.c` + `apps/SOURCES` | 新增 + 2 行 guard |
| `button-n3pro.c` | +uinput 设备读取与映射 |
| `hibylinux_codec.c` | ~15 行 guard（BT 时音量走 softvol 分支） |
| `settings_menu.c` | 1 个 guarded 菜单入口 |
| 内核 / rootfs 常驻 / 共享行为 | **零** |

---

## 三、分期计划

| 期 | 内容 | 验证点 |
|---|---|---|
| P1 | PoC：实机连耳机 + 测试程序播正弦波 | bluetooth PCM 与 pcm-alsa 参数协商；顺便跑 bt_recon6.sh 验 RX 解码器符号 |
| P2 | TX 集成：设备切换 + softvol 音量 + 扫描/连接 UI + 耳机按键 | 音量手感、EQ 生效于耳机 |
| P3 | RX 直通 + DSP（无 UI 先出声） | EQ/Space'80 对手机流生效；SBC 稳定 |
| P4 | RX 屏幕 + 控制按钮 + 元数据 | 手机上下曲/暂停受控；标题刷新 |
| P5 | 打磨：图标/自动重连/LDAC 切换/省电 | — |

## 四、风险清单

1. bluetooth ioplug 的 period/buffer 协商（P1 首验）
2. softvol 控件挂 card 0 的可行性（备选已列）
3. RX 方向手机选 AAC 时协商失败 → `AACSinks=0` 兜底
4. bluetoothd 老栈（4.101）对新手机兼容性——原厂同栈在卖，风险低
5. `/etc/asound.conf` 为 rootfs 运行时文件：断开时必须还原（原厂同手法）
6. D-Bus 元数据信号格式需在 PoC 阶段抓包确认（dbus-monitor）
7. **`/etc/asound.conf` 改为 tmpfs + 符号链接覆盖（已实现）**
   - 目的：该文件是 rootfs 上的运行时配置，目前每次改动都要写 flash。现阶段已先缓解为
     "**内容不变就不重写**"（`bt_write_asound()` 先读旧内容比较），蓝牙重构时再研究彻底方案。
   - 思路：正式内容写到 `/tmp/asound.conf`，再把 `/etc/asound.conf` 换成指向它的符号链接。
   - 依据：alsa-lib 读该文件走普通 `open()`，内核会跟随符号链接；标准 `alsa.conf` 以
     `@hooks { func load files ["/etc/asound.conf" …] errors false }` 加载它，文件缺失/悬空只表示
     "没有用户配置"，不会报错；现有 `fopen(BT_ASOUND_CONF,"w")` 会透过链接直接落到 tmpfs，
     代码几乎不用改。
   - 注意：
     - 首次建链是一次 flash 写（`unlink` + `symlink`），需 rootfs 支持符号链接（`/` 为 UBIFS，
       支持）；只读或失败时必须降级回普通文件写法。
     - `/tmp` 是 tmpfs：重启后内容丢失、链接悬空 → 没有 `btvol` PCM 定义。系统本身不受影响，
       蓝牙路由建立时我们会重新写入，可自愈。
     - 凡是用 `stat()` 判断该文件是否存在的组件，在首次写入前会认为"不存在"。
     - 备选：`mount --bind /tmp/asound.conf /etc/asound.conf`（同样不持久，需重新挂载）。
   - 已实现：`bt_write_asound()` 先经 `bt_asound_target()` 惰性把 `/etc/asound.conf` 换成指向
     `/tmp/asound.conf` 的符号链接（失败则降级写普通文件），内容仍只在变化时重写；随“把蓝牙
     抽成通用框架”重构一并落地（设备层 `firmware/target/hosted/cayin/n3pro/n3pro-bluetooth.c`）。
