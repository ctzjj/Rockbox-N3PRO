# N3Pro 蓝牙 PoC —— LDAC (A2DP Source) 与「原厂插件可用」结论

## 结论（最重要）
**原厂 ALSA 插件 `libasound_module_pcm_bluetooth.so` 对 LDAC 是可用的**：
`pcm.bluetooth { type bluetooth; device <MAC>; profile a2dp }` +
`audio.conf DefaultCodec=LDAC_HQ`，用普通 `snd_pcm_open/writei`（S16/44100/2ch）
即可输出，实测 441000 帧实时播完 9.88s，耳机为干净 440Hz（用户确认）。

→ **Rockbox 接入不需要自己实现 A2DP/编码**，只要把播放设备切到这个 PCM。
（SBC/AAC 之前直连 IPC 的 PoC 仍保留，作为备选/无插件依赖方案。）

## 实测步骤（设备上）
```sh
bt_codec LDAC_HQ                 # 写 /etc/bluetooth/audio.conf DefaultCodec
killall -9 bluetoothd; sleep 2; hciconfig hci0 up; setsid bluetoothd
cat > /etc/asound.conf <<'EOF'
pcm.bluetooth { type bluetooth; device "84:AC:60:73:BD:6F"; profile "a2dp" }
EOF
/tmp/bt_plug_sine 10 440         # snd_pcm_open("bluetooth") + S16/44100/2ch
```
- 插件在 open 时自动向已配对设备发起 A2DP 连接（无需先 bt-device -c）。
- 会打印 `A2DP_VendorLoadEncoderLdac` / `ldac_get_handle`（dlopen libldacBT.so）。
- 插件尝试连 `/var/run/sys_client` 失败（HiBy 私有守护进程）**不影响**工作。
- 写是实时的（writei 阻塞到 A2DP 吞吐），state 3=RUNNING。
- 编码质量由 `DefaultCodec` 决定：LDAC_HQ / LDAC_SQ / LDAC_MQ / LDAC_ABR / SBC / AAC / APTX / UAT_*。
  改 DefaultCodec 后需重启 bluetoothd。

## 抓到的 LDAC 协商字节（直连 IPC 备选方案用）
- GET_CAPABILITIES：与 SBC/AAC 相同（171B，MAC@22，尾 `01 00`）。
- **BT_OPEN**：170B，尾 `[168]=0x03 [169]=0x02` → **LDAC Source SEP seid = 3**。
- **BT_SET_CONFIGURATION**（12B）：`00 02 0c 00  03 00 0d 08 00 00 20 01`
  （seid=03；其后 7 字节为 LDAC 配置，等价 vendor 0x0000012D / codec 0x00AA + 2 字节能力）。
- BT_START_STREAM：`00 04 04 00` → NEW_STREAM `01 03 04 00` + SCM_RIGHTS fd。
- 数据包：**12B RTP 头 + 1B 载荷头 + 若干 LDAC 帧**，包长 673B。
  RTP 头实测：`80 01 <seq16> 00 00 <ts16?>`；ts 每包 +0x100（256）。载荷头/帧长待精确
  解析（LDAC_HQ/44.1k）。libldacBT 公开 API：`ldacBT_get_handle / ldacBT_init_handle_encode
  (h, mtu, eqmid, channel_mode, sampling_freq) / ldacBT_encode / ldacBT_get_bitrate`。
  插件另导出封装 `ldac_init / ldac_init_handle_encode / ldac_encode / ldac_get_codesize`。

## 对 Rockbox 意味着什么
1. **输出（TX）**：`pcm_alsa_set_playback_device("bluetooth")`（pcm-alsa.c 现成 API）+
   首次使用前写好 `/etc/asound.conf` 与 DefaultCodec，即得完整 DSP 管线输出。
   音量：插件 PCM 无 mixer → 需 softvol 包装或 DSP 软件音量（待验证）。
2. **输入（RX）**：插件应同样支持 capture（待验证）——若可用，RX 也走现成插件，
   再进 `dsp_process()` 输出到本机 AK4493。
3. **耳机按键**：bluetoothd 内建 AVRCP→uinput，映射到 button-n3pro.c 即可。
