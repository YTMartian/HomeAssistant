# ESP32-C3 分贝仪 Wi-Fi / Prometheus 升级包

## 本次升级内容

- 以你上传的 `SoundMeter_595_ESP32C3.ino` 为基础，不改 INMP441 采样、A 计权滤波、74HC595 刷新任务、引脚和数码管显示参数。
- 新增 ESP32-C3 的 2.4 GHz Wi-Fi、非阻塞式连接与自动重连、TCP 80 端口的 Prometheus `/metrics` HTTP 接口。
- 麦克风每约 0.5 秒更新一次测量结果；Prometheus 设为每 1 秒抓取。断网不应阻止本地数码管继续显示。
- 同时提供 `sound_level_db`（估算未计权分贝，与现有飞书 v1.1 配置相符）和 `sound_level_dba`（估算 A 计权分贝，与原本数码管显示的数值相符）。
- 原始代码备份为 `BACKUP_original_SoundMeter_595_ESP32C3.ino`。

> 注意：此固件不是专业校准声级计。这里的 dB(A) 只是基于已有数字滤波器的估算值，真实准确度取决于麦克风、算法、安装结构和校准。

## 1. Arduino IDE 打开与填写 Wi-Fi

1. 打开目录 `SoundMeter_595_ESP32C3_WiFi/` 内同名的 `.ino`。
2. 选择开发板管理器安装的 `esp32 by Espressif Systems` 3.x，板型 `ESP32C3 Dev Module`；USB 连接开发板。
3. 找到：

   ```cpp
   const char WIFI_SSID[] = "YOUR_2_4G_WIFI_NAME";
   const char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";
   ```

   用你自己的 **2.4 GHz Wi-Fi** 名称和密码替换。不要把填有真实密码的源码公开分享。
4. 检查原有 `SEGMENT_ACTIVE_LOW`、`DIGIT_ACTIVE_HIGH`、`SEGMENT_FIRST`、`REVERSE_DIGITS`、`SEGMENT_MAP` 值保持与你已正常工作的实物一致。这些值本次都未修改。
5. 编译并烧录。建议串口监视器设置为 **115200 波特率**。首秒为声音滤波预热，启动后稍后开始显示分贝。
6. 记下串口打印的 `Wi-Fi ready, IP: ...`。若打印未显示，也可在路由器 DHCP 客户端表中查找 `soundmeter-c3`。

若 Wi-Fi 连不上：确认 2.4 GHz、密码、信号和路由器是否开启无线客户端隔离。Wi-Fi 不通时数码管仍可本地使用。

## 2. 验证 ESP32 HTTP 指标

假设 ESP32 获取到 `192.168.31.180`：

```bash
curl -fsS --max-time 5 http://192.168.31.180/metrics
```

正常数据格式示例（**仅示例，不是实际值**）：

```text
# TYPE sound_meter_audio_up gauge
sound_meter_audio_up 1
# TYPE sound_level_db gauge
sound_level_db 53.40
# TYPE sound_level_dba gauge
sound_level_dba 49.80
```

实际响应还包括 `# HELP`、读数新鲜度、设备运行时间和 Wi-Fi 信号等指标。
`/metrics` 不要求输入凭证；**仅用于可信家庭局域网，绝对不要把设备的 80 端口映射到公网**。

如果读取不到分贝值但有 `sound_meter_audio_up 0`，检查麦克风与程序串口输出；读取到 `audio_up 1` 才代表最近 5 秒有有效测量值。

## 3. 第一台 TrueNAS Prometheus（192.168.31.77:30104）

ESP32 的 IP 以实际地址为准，建议在路由器上设置 DHCP 地址保留。

在**现有** Prometheus `prometheus.yml` 的 `scrape_configs:` 下追加文件 `prometheus_sound_meter.yml` 里的 job。**不要替换整个文件**，避免把 NAS/node_exporter/smartctl 现有任务覆盖掉。

```yaml
  - job_name: "sound-meter"
    scrape_interval: 1s
    scrape_timeout: 800ms
    metrics_path: /metrics
    static_configs:
      - targets: ["192.168.31.180:80"]  # 替换为 ESP32 实际地址
        labels:
          room: "test_room"
```

Prometheus 容器使用的配置文件可能通过 TrueNAS Apps 数据集挂载。先用 `sudo docker ps` 找到容器，再使用 `docker inspect <容器名>` 核实挂载路径，在宿主机实际配置文件中修改（推荐 `vim`）。修改后按你的部署方式重新加载/重启 Prometheus。不要直接编辑容器内部文件，也不要盲目覆盖旧配置。若启用了热加载，可向 `/-/reload` 发送 POST；否则重启 Prometheus 容器。

测试目标和指标：

```bash
curl -sG 'http://127.0.0.1:30104/api/v1/query' --data-urlencode 'query=up{job="sound-meter"}'
curl -sG 'http://127.0.0.1:30104/api/v1/query' --data-urlencode 'query=sound_level_db{job="sound-meter"}'
curl -sG 'http://127.0.0.1:30104/api/v1/query' --data-urlencode 'query=sound_level_dba{job="sound-meter"}'
```

看到 `data.result` 有返回且目标 `up=1` 后，飞书机器人就有可查询的数据了。历史折线图需要时间积累，不会自动填充旧数据。

## 4. 现有飞书智能家庭助手 v1.1 的指标对应

当前项目 `/mnt/mydb/apps/Feishu_SmartHome_Assistant_v1_1/config/sensors.json`（以你实际运行目录为准），原来查询的是：

```json
"promql": "sound_level_db{job=\"sound-meter\"}",
"unit": "dB SPL（估算，未计权）"
```

**默认不需要修改机器人**：因为这个固件会输出 `sound_level_db`，保持原配置即可。但这表示飞书查的是**未计权值**，数码管显示的是**估算 A 计权值**，读数可能不同。

如果希望飞书与数码管显示相同的指标，把以上两项改为：

```json
"promql": "sound_level_dba{job=\"sound-meter\"}",
"unit": "dB(A)（估算）"
```

编辑后按照实际项目方式重启机器人容器即可。仅改现有 `sound` 对应的两行；温湿度配置不需要启用。

## 5. Grafana 可用 PromQL

| 目标 | PromQL |
|---|---|
| 实时估算 dB(A) | `sound_level_dba{job="sound-meter"}` |
| 实时未计权 dB SPL | `sound_level_db{job="sound-meter"}` |
| 最近 1 分钟最高估算 dB(A) | `max_over_time(sound_level_dba{job="sound-meter"}[1m])` |
| 最近 1 分钟最小估算 dB(A) | `min_over_time(sound_level_dba{job="sound-meter"}[1m])` |
| 当前设备在线（Prometheus 抓取成功） | `up{job="sound-meter"}` |
| 麦克风数据正常 | `sound_meter_audio_up{job="sound-meter"}` |

**dB 是对数单位**，`avg_over_time` 算出来的是 dB 读数的算术平均，不能直接当作严格的能量等效连续声级 `LAeq`。要得到准确的 LAeq，应在相应时间段内对线性能量进行平均，再取对数。本版保持原始源码的约 0.5 秒 RMS 计算逻辑，没有另加 LAeq 计算。

## 6. 常见问题

- **HTTP 无法连接**：先检查串口有没有分配 Wi-Fi IP；TrueNAS 必须能访问 ESP32 所在 Wi-Fi，不能开启客户端隔离。
- **Prometheus `up=0`**：目标 IP/端口错、网络不通或服务未连 Wi-Fi。
- **Prometheus `up=1`，但无分贝**：看 `sound_meter_audio_up` 是否为 0，检查 INMP441 的 SD/SCK/WS 和 GND。
- **飞书“无近期数据”**：机器人指标名称/`job` 标签与真实 Prometheus 不一致，或数据尚未开始采集。
- **数码管乱码**：该程序保持上传文件中的 595 显示驱动参数。不要使用 TM1637 库；对比你旧程序的四个 profile 选项和 `SEGMENT_MAP`。
- **新固件重启/数码管闪烁**：检查 USB 供电和 3.3V 稳压能力；ESP32-C3 使用 Wi-Fi 时瞬时电流会增加。

## 验证范围

本次输出经过源码结构及差异检查，保留了原有引脚和滤波器参数；**未在你的 ESP32-C3 实物上烧录测试**。若 Arduino 编译报错，请提供完整的 IDE 错误日志以及安装的 `esp32 by Espressif Systems` 版本。
