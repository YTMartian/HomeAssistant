"""Pure command parsing and sensor transformations. No Feishu dependency."""
import json
import math
import re
from dataclasses import dataclass
from pathlib import Path

WINDOWS = {"1小时": 3600, "6小时": 21600, "24小时": 86400,
           "7天": 604800, "1h": 3600, "6h": 21600,
           "24h": 86400, "7d": 604800, "1天": 86400}

@dataclass(frozen=True)
class Command:
    action: str   # menu | status | current | history
    sensor: str = "all"
    period: int = 86400
    period_label: str = "24小时"


def clean_text(raw: str) -> str:
    raw = re.sub(r"<at\b[^>]*>.*?</at>", " ", raw, flags=re.I | re.S)
    raw = re.sub(r"@_user_\d+", " ", raw)
    raw = raw.replace("@智能家庭助手", " ").replace("智能家庭助手", " ")
    return re.sub(r"\s+", " ", raw).strip()


def parse_command(text: str, sensors: dict) -> Command:
    text = clean_text(text).lower()
    if not text or any(t in text for t in ("帮助", "help", "菜单", "指令")):
        return Command("menu")
    if any(t in text for t in ("状态", "设备", "在线", "传感器列表")):
        return Command("status")
    matches = []
    for key, cfg in sensors.items():
        for alias in cfg.get("aliases", []):
            if alias.lower() in text:
                matches.append((len(alias), key))
    sensor = max(matches)[1] if matches else "all"
    periods = [(len(w), w, secs) for w, secs in WINDOWS.items() if w in text]
    if periods:
        _, label, seconds = max(periods)
    else:
        label, seconds = "24小时", 86400
    if any(t in text for t in ("趋势", "历史", "变化", "曲线", "统计")) or bool(periods):
        return Command("history", sensor, seconds, label)
    if sensor != "all":
        return Command("current", sensor)
    return Command("menu")


def load_sensors(path: str) -> dict:
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(data, dict) or not data:
        raise ValueError("sensors.json must be a nonempty object")
    for key, s in data.items():
        if not isinstance(s.get("promql"), str) or not s["promql"].strip():
            raise ValueError(f"Invalid PromQL for {key}")
        if not isinstance(s.get("aliases"), list) or not s["aliases"]:
            raise ValueError(f"Missing aliases for {key}")
    return data


def summarize(points, is_db: bool):
    clean = [float(y) for _, y in points if math.isfinite(float(y))]
    if not clean:
        return None
    if is_db:
        peak = max(clean)
        avg = peak + 10 * math.log10(sum(10 ** ((x - peak) / 10) for x in clean) / len(clean))
    else:
        avg = sum(clean) / len(clean)
    return {"min": min(clean), "max": max(clean), "avg": avg, "count": len(clean), "last": clean[-1]}
