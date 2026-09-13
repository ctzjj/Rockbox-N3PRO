# N3Pro 蓝牙 PoC —— AAC (A2DP Source) 调研与实现要点

参考实现：bluez-alsa `src/a2dp-aac.c`（MIT，同样基于 libfdk-aac）。以下为适配本机
（BlueZ 4.101 + HiBy 插件内含 FDK-AAC）所需的全部要点。

## 一、协商（已实测抓包，可整体回放）
1. GET_CAPABILITIES 同 SBC（MAC@22，尾 `01 00`）。
2. **BT_OPEN**：同样布局，尾部改为 `[168]=02 [169]=02`（**AAC Source SEP seid = 2**）。
3. **BT_SET_CONFIGURATION**（16B）：
   `00 02 10 00 | 02 00 05 0c 00 00 80 01 04 82 0c 00`
   （另一变体第 9 字节为 01：`...05 0c 01 00 80 01...`，重试时出现）
4. BT_START_STREAM（4B `00 04 04 00`）→ NEW_STREAM → SCM_RIGHTS 取 L2CAP fd。
- seid 来源：bluetoothd 日志 `Register codec = 02` + `Register AAC as Source`；
  caps 响应尾部 SEP 表中 codec=00 为 SBC(seid 1/3/4/5)、codec=02 对应 seid=2。
- 实测：`bt_codec AAC` + 重启 bluetoothd 后，插件协商即用 seid=2（shim 抓包确认）。

## 二、编码（FDK-AAC，插件 .so 已导出公开 API）
可用符号：`aacEncOpen / aacEncoder_SetParam / aacEncEncode / aacEncInfo / aacEncClose`
（另有 HiBy 封装 `aac_init/aac_start_encode/aac_encode`，168B 上下文，未用）。

bluez-alsa 的配置序列（可直接照抄）：
```
aacEncOpen(&h, 0x0F, channels);
aacEncoder_SetParam(h, AACENC_AOT, AOT_AAC_LC);
aacEncoder_SetParam(h, AACENC_BITRATE, bitrate);       // 如 256000
aacEncoder_SetParam(h, AACENC_SAMPLERATE, rate);       // 44100
aacEncoder_SetParam(h, AACENC_CHANNELMODE, MODE_2);    // stereo
aacEncoder_SetParam(h, AACENC_AFTERBURNER, 1);
aacEncoder_SetParam(h, AACENC_TRANSMUX, TT_MP4_LATM_MCP1);  // =3
aacEncoder_SetParam(h, AACENC_HEADER_PERIOD, 1);
aacEncEncode(h, NULL,NULL,NULL,NULL);                  // 初始化
aacEncInfo(h, &info);   // frameLength(1024) inputChannels maxOutBufBytes nDelay
```
- 输入：`info.frameLength * info.inputChannels` 个 S16（1024×2）。
- 输出：LATM (AudioMuxElement) 字节流，`out_args.numOutBytes`。
- 每次 `aacEncEncode` 编码一帧（1024 样本）。

## 三、RTP 打包（与 SBC 不同！）
- **RTP 头后直接跟 LATM 字节，没有 A2DP 媒体载荷头**（bluez-alsa:
  `rtp_a2dp_init(..., NULL, 0)`，编码输出直接写入 payload 区）。
- 载荷超过 MTU 时按 RFC 3016 分片（无需额外头），最后一片置 markbit；
  未分片则整包置 markbit。
- **RTP 时间戳时钟 = 90000 Hz**（不是采样率）：
  `rtp_state_init(&rtp, rate, 90000)`，每帧 `ts += frameLength * 90000 / rate`。
  （SBC 的时钟才是采样率。）
- seq 每包 +1；ssrc 任意。

## 四、待办
1. 转写 FDK `aacenc_lib.h` 所需枚举/结构（AACENC_PARAM 值、AACENC_BufDesc/InArgs/
   OutArgs/InfoStruct、AOT_AAC_LC=2、MODE_2、TT_MP4_LATM_MCP1=3）。
2. bt_direct 增加 AAC 路径：SET_CONFIG/OPEN 换 AAC 字节，编码换 FDK，RTP 去媒体头、
   时钟 90k。
3. 实机试听；必要时尝试 SET_CONFIG 另一变体（第 9 字节 01）。
