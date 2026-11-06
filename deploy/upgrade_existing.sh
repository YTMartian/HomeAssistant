#!/usr/bin/env bash
set -euo pipefail
# usage: sudo bash deploy/upgrade_existing.sh /mnt/mydb/apps/Feishu_SmartHome_Assistant_v1_1
DEST="${1:-/mnt/mydb/apps/Feishu_SmartHome_Assistant_v1_1}"
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ ! -f "$DEST/docker-compose.yml" || ! -f "$DEST/.env" ]]; then
  echo "错误：$DEST 不是已配置好的智能家庭助手项目（缺少 docker-compose.yml/.env）" >&2
  exit 2
fi
if [[ "$SRC" == "$DEST" ]]; then
  echo "安装源和目标目录相同；无需覆盖。" >&2
  exit 2
fi
STAMP="$(date +%Y%m%d-%H%M%S)"
echo "备份 $DEST/app 到 app.bak.$STAMP"
cp -a "$DEST/app" "$DEST/app.bak.$STAMP"
# Copy only app code; retain live .env, sensors.json, Dockerfile, compose and mirror settings.
cp -a "$SRC/app/." "$DEST/app/"
cp -a "$SRC/requirements.txt" "$DEST/requirements.txt"
cd "$DEST"
echo '构建并启动现有 Docker 服务…'
docker compose up -d --build --force-recreate
echo '完成。查看日志: docker compose logs --tail=80'
