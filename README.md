# 飞书「智能家庭助手」完整版 v1.2.2（分贝监控）

这是 **可独立部署的完整源码**，已合并此前 v1.2 按钮版、v1.2.1 HTTP 400 诊断修复，以及实际验证生效的 `config.update_multi=True`。

**功能**：飞书长连接、`菜单` 按钮、`分贝` 当前值、`趋势` 默认24小时原生 Card 2.0 折线图、1小时/6小时/24小时/7天切换、`状态`、刷新、Prometheus 查询。当前只启用一项分贝指标；不要求温湿度硬件。原生图表属于飞书 Card 2.0 组件，非图片。

## 运行环境

- 第一台 TrueNAS：`192.168.31.77`
- 已有 Prometheus：`http://192.168.31.77:30104`
- ESP32-C3：`192.168.31.180:80`（通过 `/metrics` 提供分贝数据）
- 已部署的机器人目录：`/mnt/mydb/apps/Feishu_SmartHome_Assistant_v1_1`
- Docker + Docker Compose、可访问飞书开放平台的网络

## A. 在现有项目上升级（推荐；保留你的配置）

1. 把本完整项目 ZIP 解压到 `/mnt/mydb/apps/`，解压后应有 `/mnt/mydb/apps/Feishu_SmartHome_Assistant_v1_2_2`。
2. 以 SSH 登录第一台 NAS，执行：

```bash
cd /mnt/mydb/apps/Feishu_SmartHome_Assistant_v1_2_2
sudo bash deploy/upgrade_existing.sh /mnt/mydb/apps/Feishu_SmartHome_Assistant_v1_1
```

该脚本会自动备份旧 `app/`，只覆盖 Python 应用代码和依赖声明，并对原来的 Compose 服务执行 `up -d --build --force-recreate`。**不会覆盖** `.env`、`config/sensors.json`、Dockerfile、docker-compose.yml，因此不影响你已设置的第一台 NAS 地址、飞书凭证以及可能用到的 Docker Hub 镜像缓存配置。

如果不想使用脚本，也可手动执行：

```bash
cd /mnt/mydb/apps/Feishu_SmartHome_Assistant_v1_1
sudo cp -a app "app.bak.$(date +%Y%m%d-%H%M%S)"
sudo cp -a /mnt/mydb/apps/Feishu_SmartHome_Assistant_v1_2_2/app/. app/
sudo docker compose up -d --build --force-recreate
sudo docker compose logs --tail=80
```

**验证卡片修复已经进入 Docker 镜像**：

```bash
sudo docker exec feishu-smart-home-assistant grep -n update_multi /app/app/card_ui.py
```

应出现 `"update_multi": True`（而非 False）。

## B. 全新安装（在另一台主机或从零部署时）

1. 将本项目的 `app/`、`config/`、`Dockerfile`、`docker-compose.yml`、`requirements.txt`、`.env.example` 复制到目标文件夹。
2. `cp .env.example .env`，使用 `vim .env` 填写飞书 `FEISHU_APP_ID`、`FEISHU_APP_SECRET` 和 Prometheus 地址。
3. 确保机器人应用已在飞书开发者后台开启机器人、订阅 `im.message.receive_v1`，并配置 **`card.action.trigger` 长连接回调**，发布应用版本。
4. `sudo chmod 600 .env`；执行 `sudo docker compose up -d --build`。
5. 如果 `python:3.12-slim` 从 Docker Hub 拉取失败，可沿用你当前 TrueNAS Dockerfile 的 `mirror.gcr.io/library/python:3.12-slim` 镜像来源（镜像缓存有可能不可用）。

**不得在新老两个目录同时运行相同飞书 App ID 的机器人**：否则可能出现随机回复、重复消息或回调无法对应。

## C. 飞书命令

| 发送内容 | 功能 |
| --- | --- |
| `菜单` 或 `帮助` | 功能按钮菜单 |
| `分贝` | 查询当前估算 dB(A)，卡片按钮可刷新 |
| `趋势` | 查看所有已启用传感器的趋势，当前只有分贝，默认24小时 |
| `状态` | 查看设备是否有近期数据 |
| 点击图表上的 `1小时` / `6小时` / `24小时` / `7天` | 卡片原位切换时间范围 |

完整按钮更新依赖 `config.update_multi=True`。若遇到 HTTP 400，请看 `docker compose logs` 中的飞书 `code` / `msg`，避免把含临时 access_key 的 WebSocket URL 粘贴给别人。

## D. Prometheus、ESP32 和 Grafana

- 现有 ESP32 固件的接口：`curl http://192.168.31.180/metrics`
- 指标：`sound_level_dba`（A 计权估算）、`sound_level_db`（未计权估算）、`sound_meter_audio_up`、Wi-Fi RSSI 等。
- 现有 Prometheus target `up{job="sound-meter"}` 应返回 1。
- 飞书 `config/sensors.json` 默认查询 `sound_level_dba{job="sound-meter"}`。Grafana 也使用同一个 PromQL。
- `examples/prometheus_sound_meter.yml` 是添加到现有 `scrape_configs` 的片段，**不要整文件覆盖你的 NAS 监控配置**。
- `extras/ESP32_C3_SoundMeter_WiFi` 提供原来 Wi-Fi 固件的源码作备份。**你当前分贝仪已经正常上报，不需要重新烧录。**

## E. 测试及故障排查

在开发电脑安装 Python3.12 环境（可选）：

```bash
python -m pip install -r requirements-dev.txt
python -m pytest -q
```

在 NAS 上：

```bash
cd /mnt/mydb/apps/Feishu_SmartHome_Assistant_v1_1
sudo docker compose ps
sudo docker compose logs --tail=100
curl -sG 'http://192.168.31.77:30104/api/v1/query' \
    --data-urlencode 'query=sound_level_dba{job="sound-meter"}'
```

- 仍回复旧文字菜单：容器可能尚未重建或旧实例仍在运行。
- HTTP 400 且提示 `update_multi is false`：检查容器里的 `app/card_ui.py` 应为 `True`。
- 原生卡片出现，但按钮无效：检查 `card.action.trigger` 回调权限与事件订阅、服务器回调日志。
- 当前分贝无数据：检查 ESP32 的 `/metrics`、Prometheus `up{job="sound-meter"}` 和 `config/sensors.json` 中的 PromQL。
- 图表无数据：采集刚开始时历史数据不足，先等实际数据累积；7天范围不会凭空生成历史。

## 注意事项

- 不要把真实的 `.env` 或飞书 App Secret 上传到公开仓库；本项目只有 `.env.example` 占位符。
- 机器人只对内部 Prometheus 发起只读查询；不要把 ESP32 `/metrics` 端口映射到公网。
- 原代码采用长连接回调兼容处理，仍需在你的飞书客户端实测按钮点击更新。这里不能直接连接你的服务器进行端到端验证。
- 分贝仪未经专业校准，数据仅作为日常趋势参考。
