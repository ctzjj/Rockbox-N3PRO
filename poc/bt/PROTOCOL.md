# N3Pro 蓝牙 PoC —— 直连 bluetoothd 的 A2DP Source 实现（已验证出声）

## 结论
绕过原厂 ALSA 蓝牙插件，直接与 BlueZ 4.101 (HiBy 版) 的 audio IPC 对话，
自建 SBC 编码 + RTP + L2CAP 推送，QCY AilyBuds E10 上播放出干净的 440Hz 正弦。
实现见 `bt_direct.c`（约 290 行，含全部调试），编码器 vendored 在 `sbc/`。

实机验证（2026-09-13）：20 次迭代调试后 "准确了"（无杂音/无啪啪声）。

## 一、栈事实
- BlueZ 4.101，Broadcom BCM4345C5（`/dev/ttyS0` 3Mbaud，`brcm_patchram_plus`）。
- Rockbox 模式下开机自启（S40bt_init）：dbus-daemon / brcm_patchram_plus /
  bluetoothd / bt-agent / bt-monitor / bt-media 全在跑；hci0 初始 DOWN，
  `hciconfig hci0 up` 即可（若折腾过 bluetoothd，killall 后用 setsid 重启）。
- 内核 BT 栈全内建（H4/L2CAP/SCO/RFCOMM），零内核工作。

## 二、audio IPC 协议（逆向自 LD_PRELOAD 抓包）
- **套接字**：抽象命名空间 `\0/org/bluez/audio`，**SOCK_STREAM**，
  connect 地址长度必须 = `sizeof(struct sockaddr_un)` = **110**（sun_path 其余补零）。
- **消息帧**（4 字节头）：`{u8 type, u8 code, u16 len(LE)}`，len = 含头的整条长度。
  type：0x00=request 0x01=response 0x03=error（error 载荷 = 1 字节 errno）。
  读法：读 4 字节头 → 再读 len-4。
- **消息序列**（对已配对设备）：
  1. `GET_CAPABILITIES` code=0x00, 171B：
     `00 00 ab 00` + 18 字节 0 + **MAC 字符串在偏移 22**（18 字节含 \0）
     + 0 填至 [168]，尾 `[169]=01 [170]=00`。→ 232B
  2. `BT_OPEN` code=0x01, 170B：同布局，尾 `[168]=05 [169]=02`（seid=5, lock=2）。→ 181B
  3. `BT_SET_CONFIGURATION` code=0x02, 17B：
     `00 02 11 00 05 00 01 0d 00 00 01 02 01 01 01 02 26`（SBC/JST/bp38）
     → 10B `01 02 0a 00 a7 02 a8 02 a7 02`（imtu 679 / omtu 680）
  4. `BT_START_STREAM` code=0x04, 4B：`00 04 04 00` → `01 04 04 00`
  5. 随后 `01 03 04 00`（NEW_STREAM）→ `recvmsg` 用 **SCM_RIGHTS 收到 L2CAP 流 fd**
- **fd 处理**：bluetoothd 传回的是**非阻塞** fd，需 `fcntl(F_SETFL, fl & ~O_NONBLOCK)`
  转阻塞以获得 L2CAP 背压。`setsockopt(SO_SNDBUF)` 建议 16KB（~320ms）。
- MAC 偏移错误 → EINVAL(22)；设备未连接 → EINVAL。

## 三、RTP 打包（关键坑）
```
[12B RTP 头][1B A2DP SBC 媒体载荷头][N × SBC 帧]
```
- RTP：`0x80 0x60`，seq(BE16) 每包 +1，timestamp(BE32) 每包 += 128×N，ssrc 任意。
- **1 字节媒体载荷头：低 4 位 = 本包 SBC 帧数（≤15）**，其余位 0。
  **漏掉这个头会让接收端解析错位一个字节 → 全白噪音！**（本项目最大坑）
- 每包 6 帧 = 13+6×89 = 547B < omtu 680 ✓。

## 四、SBC 编码
- vendored 自 bluez sbc（`sbc/` 目录，LGPL 2.1+，来源 heinervdm/bluez 镜像）：
  `sbc.c sbc.h sbc_primitives.c sbc_primitives.h sbc_math.h sbc_tables.h`，
  SIMD 头用空桩（`sbc_primitives_{mmx,iwmmxt,neon,armv6}.h` 未定义 build 宏即不用）。
- **新 ABI**：`sbc_encode(sbc,input,ilen,output,olen,ssize_t *written)` 6 参数，
  返回消费的输入字节数，输出长度经 written；`sbc_decode(..., size_t *written)`。
  `sbc_t`：`{ulong flags; u8 frequency,blocks,subbands,mode,allocation,bitpool,endian;
  void *priv; void *priv_alloc_base;}`（priv@12, priv_alloc_base@16；
  sbc_init 内部 malloc priv 并调 sbc_init_primitives）。
- **字段值用真 sbc.h 常量**（JST=3, BLK16=3, SB8=1, FREQ44=2, LE=0）。
  自造常量会导致 mode 变 STEREO 等 → 接收端白噪。
- 本机配置：44.1k / JointStereo / blocks16 / subbands8 / LOUDNESS / bitpool 38
  → codesize 512B、framelen 89B、2.902ms/帧。
- 实测：原厂插件 dlopen 导出的 sbc_encode 与 vendored 源码编出**逐字节相同**的帧，
  且帧可被 ffmpeg 正常解码（纯正弦）→ 编码器无问题。

## 五、发送节奏（第二个大坑）
- 症状：开始正常，播到后面出现啪啪声。
- 根因：**发送速率必须贴合实时**。L2CAP basic mode 无流控，发送超速会灌爆接收端
  缓冲区→丢包→后期啪啪；发送过慢则欠载→爆音。
- 解法：以墙钟维持约 **100ms 媒体领先量**（`tofday` 计算 lead=media_us-elapsed_us，
  `lead>LEAD_US` 时 `usleep(lead-LEAD_US)`），配合阻塞 send 硬背压。
  实测 12.0s 音频 / 11.9s 墙钟。
- 另：**热点循环里不要 printf 到 adb PTY**（无缓冲会阻塞），`sin()` 在软浮点 MIPS 上
  每包 768 次调用会成为瓶颈 → 改用整数正弦查找表。

## 六、编解码能力（插件导出符号，RX/TX 均可复用）
- SBC：`sbc_encode/sbc_decode`（本 PoC 已用）
- AAC：FDK `aacEnc*`（编码）+ `aacDecoder_*`（解码）→ TX/RX 都可行（iPhone 默认 AAC）
- LDAC：`A2DP_VendorLoadEncoderLdac` + `libldacBT_enc.so`（仅编码；索尼无公开解码器）
- APTX：`libbt-aptx-bridge.so`
- `libaac.so`、`libldacBT.so` 在 `/usr/lib`。

## 七、原厂 ALSA 路线为何放弃（记录备查）
- `pcm.btout {type bluetooth; device ...; profile a2dp}`（原厂同款写法）：
  open/prepare 成功，bluetoothd 侧全链路成功（SET_CONFIG→OPEN→START→STREAMING，
  NEW_STREAM+fd 两次送达），但 `snd_pcm_writei` 恒返回 EINVAL，state 停 PREPARED。
- 插件收 fd 后仅读 audio.conf + setsockopt 便本地失败（无系统调用）。
- 已尝试：二进制补丁插件所有 `li -22`、伪造 `/var/run/sys_client` 应答
  (`BT:CODEC/BT:CHANGE` 文本通知)、手动启 sys_server、强制 SBC、两台设备 —— 均无效。
- 无插件源码，判定为不可修的黑盒，改走直连。

## 八、Rockbox 集成路线（下一步）
- `firmware/target/hosted/cayin/n3pro/bt-n3pro.c`：直连 IPC 客户端 + RTP 发送
  → 作为 pcm 输出后端（A2DP Source），走 Rockbox 完整 DSP 链。
- 编解码：优先 vendored SBC（已就绪）；LDAC/AAC 复用插件导出符号或库。
- RX：同一 IPC 的 capture 变体 + `sbc_decode`/`aacDecoder` → DSP → 本地 AK4493。
- 连接管理：`bt-connect`/`bt-device`/`hcitool`（原厂二进制，Rockbox 下均可用）。
- 耳机按键：bluetoothd 已内建 uinput（AVRCP 按键→evdev）；元数据经 D-Bus。
