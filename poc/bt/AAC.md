# N3Pro 蓝牙 PoC —— AAC (A2DP Source) 实现记录

参考实现：bluez-alsa `src/a2dp-aac.c`（同样基于 libfdk-aac）。本机用 HiBy 插件
里内置的 FDK-AAC（`/usr/lib/alsa-lib/libasound_module_pcm_bluetooth.so` 导出
公开 API）。**已实测通过：QCY AilyBuds E10 上干净 440Hz，连续 12s 稳定。**

## 一、协商（实测抓包，已整体回放成功）
1. GET_CAPABILITIES 同 SBC（MAC@22，尾 `01 00`），响应 232B。
2. **BT_OPEN**：同样的 170B 布局，尾部 `[168]=02 [169]=02`（AAC Source SEP seid=2）；
   响应 180B。
3. **BT_SET_CONFIGURATION**（16B）：
   `00 02 10 00 02 00 05 0c 00 00 80 01 04 82 0c 00`
   响应 `01 02 0a 00 a7 02 a8 02 a7 02`（== AAC_MPEG-4 / MPEG-2,4 AAC LC）。
   耳机接受 → 协商成功（若耳机不支持 AAC，此步会被拒）。
4. BT_START_STREAM（4B `00 04 04 00`）→ NEW_STREAM → SCM_RIGHTS 取 L2CAP fd。

## 二、FDK-AAC ABI（枚举 v0.1.6 与 v2.0.2 完全一致！）
参数 id：
```
AACENC_AOT            = 0x0100
AACENC_BITRATE        = 0x0101
AACENC_SAMPLERATE     = 0x0103
AACENC_CHANNELMODE    = 0x0106
AACENC_AFTERBURNER    = 0x0200
AACENC_TRANSMUX       = 0x0300
AACENC_HEADER_PERIOD  = 0x0301
AACENC_AUDIOMUXVER    = 0x0304
```
常量：`AOT_AAC_LC=2`, `MODE_2=2`, **`TT_MP4_LATM_MCP1=6`**（注意不是 3！），
`IN_AUDIO_DATA=0`, `OUT_BITSTREAM_DATA=3`。
（早期版本 0x00xx 编号只存在于 2012 年前的 fdk，主流 0.1.x/2.x 均为上面这套。）

结构体（0.1.6 与 2.0.2 前缀兼容；OutArgs 按 4 个 INT 定义以兼容 2.0.2）：
```c
typedef struct { INT numBufs; void **bufs; INT *bufferIdentifiers;
                 INT *bufSizes; INT *bufElSizes; } AACENC_BufDesc;
typedef struct { INT numInSamples; INT numAncBytes; } AACENC_InArgs;
typedef struct { INT numOutBytes; INT numInSamples; INT numAncBytes;
                 INT bitResState; } AACENC_OutArgs;
typedef struct { UINT maxOutBufBytes, maxAncBytes, inBufFillLevel,
                      inputChannels, frameLength, nDelay, nDelayCore;
                 UCHAR confBuf[64]; UINT confSize; } AACENC_InfoStruct;
```
（InfoStruct 的 `maxOutBufBytes@0 / inputChannels@12 / frameLength@16` 在
0.1.6/2.0.2 中偏移一致，可安全共用。）

## 三、编码调用（照抄 bluez-alsa，实测全 0 返回）
```
aacEncOpen(&enc, 0x0F, channels);
aacEncoder_SetParam(AOT, AOT_AAC_LC);
aacEncoder_SetParam(BITRATE, 256000);
aacEncoder_SetParam(SAMPLERATE, 44100);
aacEncoder_SetParam(CHANNELMODE, MODE_2);
aacEncoder_SetParam(AFTERBURNER, 1);
aacEncoder_SetParam(TRANSMUX, TT_MP4_LATM_MCP1);
aacEncoder_SetParam(HEADER_PERIOD, 1);
aacEncEncode(enc, NULL,NULL,NULL,NULL);       // init
aacEncInfo(enc, &info);                        // frameLength=1024, inputChannels=2,
                                               // maxOutBufBytes=1536
```
每帧：
```
in_buf  = { .numBufs=1, .bufs=&pcm,      ids={IN_AUDIO_DATA},       elSizes={2} };
out_buf = { .numBufs=1, .bufs=&latm_ptr, ids={OUT_BITSTREAM_DATA},  elSizes={1} };
in_args.numInSamples = frameLength * channels;   // 注意是“样本数”不是帧数(2048)
aacEncEncode(enc, &in_buf, &out_buf, &in_args, &out_args);
// out_args.numOutBytes = LATM 字节数；out_args.numInSamples = 消耗样本数
```

### 三个实测坑（都踩过）
1. **输出缓冲区必须 ≥ `info.maxOutBufBytes`（1536）**：若只按 A2DP MTU
   (672) 开缓冲，FDK 会越界写入 → 栈被破坏 → segfault。先编码进大缓冲，
   再按 MTU 分片发送。
2. **编码器有初始延迟**：开头若干次 `aacEncEncode` 返回 `numOutBytes=0`
   （消耗样本但不立即出流）。此时要**继续喂帧并推进时间戳**，不能 break。
3. `out_bufSizes[]` 要填真实缓冲容量（≥maxOutBufBytes），不是 MTU。

## 四、RTP 打包（同 bluez-alsa，已实测）
- **12B RTP 头 + LATM 字节，没有 A2DP 媒体载荷头**（`rtp_a2dp_init(...,NULL,0)`）。
- **RTP 时间戳时钟 = 90000 Hz**：`ts_num += consumed_frames * 90000; ts = ts_num/90000;`
  （SBC 的时钟才是采样率。）
- 载荷 > MTU-12 时按 RFC 3016 分片：同一 ts、seq 递增、**最后一片置 markbit**，
  无额外头；未分片时整包 markbit。第二字节：mark=0xE0 / 非 mark=0x60。
- seq 每包 +1；ssrc 任意。

## 五、实测结果
`bt_aac <MAC> <secs> <freq> <bitrate>`：12s、256kbps，`529408 frames / 12.0s wall,
min_lead=15ms`，QCY AilyBuds E10 上干净 440Hz（用户确认）。与 SBC 版本共用
相同的约 100ms 领先节流（太快溢出/太慢断音）。

## 六、后续
- LDAC（seid 待抓，编码器 `libldacBT_enc` 公开 API）同法扩展。
- 移植进 Rockbox：`bt-n3pro.c`（A2DP source 用本方案；source SEP/编解码与
  采样率随播放动态配置），接入 pcm-alsa。
