"""Feishu Card 2.0 responsive controls. No image uploads."""
from __future__ import annotations
from datetime import datetime
from zoneinfo import ZoneInfo
from .chart_card import build_trend_card

WINDOW_CHOICES = [("1小时", "1h", 3600), ("6小时", "6h", 21600),
                  ("24小时", "24h", 86400), ("7天", "7d", 604800)]
WINDOWS_BY_KEY = {key: (label, seconds) for label, key, seconds in WINDOW_CHOICES}


def button(label: str, action: str, *, sensor: str = "all", period: str = "24h", primary=False):
    return {
        "tag": "button", "type": "primary" if primary else "default", "size": "small",
        "text": {"tag": "plain_text", "content": label},
        "behaviors": [{"type": "callback", "value": {
            "action": action, "sensor": sensor, "period": period,
        }}],
    }


def button_row(items):
    # 兼容优先：按钮直接放进 body.elements，不再使用嵌套分栏。
    # Card 2.0 直接支持 button 元素，能排除 column_set 兼容性问题。
    return items


def make_card(title: str, elements: list, template="blue"):
    # 将按钮列表铺平成顶层元素，尽量采用 Card 2.0 最简 JSON。
    flat = []
    for element in elements:
        if isinstance(element, list):
            flat.extend(element)
        else:
            flat.append(element)
    return {
        "schema": "2.0", "config": {"update_multi": True},
        "header": {"title": {"tag": "plain_text", "content": title}, "template": template},
        "body": {"elements": flat},
    }


def menu_card(sensors):
    names = [s["name"] for s in sensors.values() if s.get("enabled")]
    enabled = [k for k, s in sensors.items() if s.get("enabled")]
    rows = [
        {"tag": "markdown", "content": "**智能家庭助手** · 直接点击按钮查询，无需记命令。"},
        {"tag": "markdown", "content": "当前已接入：" + ("、".join(names) or "暂无")},
        button_row([button("📈 全部趋势", "trend", period="24h", primary=True),
                    button("📟 设备状态", "status")]),
    ]
    if enabled:
        rows.append(button_row([button(f"🔊 {sensors[k]['name']}", "current", sensor=k)
                                for k in enabled[:3]]))
    rows.append({"tag": "markdown", "content": "也支持直接发送：`趋势` / `分贝` / `状态` / `菜单`"})
    return make_card("🏠 智能家庭助手 · 功能菜单", rows, "turquoise")


def current_card(sensor_key, cfg, value, tz):
    if not value or not value["fresh"]:
        detail = "⚠️ 暂无最新读数。请检查 ESP32 Wi-Fi 和 Prometheus targets。"
    else:
        stamp = datetime.fromtimestamp(value["timestamp"], ZoneInfo(tz)).strftime("%m-%d %H:%M:%S")
        detail = (f"**{value['value']:.{cfg.get('precision',1)}f} {cfg['unit']}**  \n"
                  f"采样时间：{stamp}  \n"
                  f"延迟：{value['age_seconds']:.0f} 秒")
        if cfg.get("db_energy_avg"):
            detail += "  \n*未经专业声学校准，仅用于环境趋势参考。*"
    elements = [
        {"tag": "markdown", "content": detail},
        button_row([button("📈 查看趋势", "trend", sensor=sensor_key, period="24h", primary=True),
                    button("🔄 刷新", "current", sensor=sensor_key),
                    button("🏠 菜单", "menu")]),
    ]
    return make_card(f"{cfg['name']} · 当前读数", elements)


def status_card(status_lines):
    return make_card("🏠 家庭传感器状态", [
        {"tag": "markdown", "content": "  \n".join(status_lines)},
        button_row([button("📈 全部趋势", "trend", period="24h", primary=True),
                    button("🔄 刷新", "status"), button("🏠 菜单", "menu")]),
    ], "green")


def trend_card(series_data, period_key, tz, scope="all"):
    """series_data: [(key, cfg, samples, stats)], no fabricated data."""
    period_label, _ = WINDOWS_BY_KEY[period_key]
    elements = [
        {"tag": "markdown", "content": "**选择时间范围**（点击后直接更新本卡片）："},
        button_row([button(label, "trend", sensor=scope, period=key,
                           primary=(key == period_key))
                    for label, key, _ in WINDOW_CHOICES]),
    ]
    if not series_data:
        elements.append({"tag": "markdown", "content": "⚠️ 当前时间范围暂无足够的传感器数据。"})
    for key, cfg, samples, stats in series_data:
        if stats and len(samples) >= 2:
            unit = cfg["unit"]
            mean_title = "近似能量均值" if cfg.get("db_energy_avg") else "采样平均值"
            summary = (f"**{cfg['name']}** · 最新 **{stats['last']:.1f} {unit}**  \n"
                       f"{mean_title} {stats['avg']:.1f} · 最低 {stats['min']:.1f} · 最高 {stats['max']:.1f}")
            elements.append({"tag": "markdown", "content": summary})
            chart = build_trend_card(samples, cfg, period_label, tz)
            chart_node = next(x for x in chart["body"]["elements"] if x["tag"] == "chart")
            elements.append(chart_node)
        else:
            elements.append({"tag": "markdown", "content": f"**{cfg['name']}**：暂无足够数据生成趋势。"})
    elements.append(button_row([
        button("🔄 刷新", "trend", sensor=scope, period=period_key),
        button("📟 状态", "status"), button("🏠 菜单", "menu")]))
    elements.append({"tag": "markdown", "content": "数据：Prometheus · 分贝未经专业校准；图中数据仅供参考。"})
    return make_card(f"📈 环境趋势 · 最近{period_label}", elements)
