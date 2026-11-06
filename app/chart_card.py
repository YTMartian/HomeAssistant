"""Feishu Card JSON 2.0 native VChart line chart. Never create an image."""
from datetime import datetime
from zoneinfo import ZoneInfo
import math

MAX_CHART_POINTS = 120  # keep reply size small for Feishu card APIs


def build_trend_card(points, cfg, period_label, tz='Asia/Shanghai'):
    """Build a native Card 2.0 chart from Prometheus (timestamp, value) pairs.

    Input must be real Prometheus samples. Empty or non-finite readings are discarded.
    """
    filtered = [(float(t), float(v)) for t, v in points if math.isfinite(float(v))]
    if len(filtered) < 2:
        raise ValueError('At least two valid history samples required')
    if len(filtered) > MAX_CHART_POINTS:
        # Even spacing from first to last without importing numpy.
        n = len(filtered)
        indices = sorted(set(round(i * (n-1) / (MAX_CHART_POINTS - 1))
                             for i in range(MAX_CHART_POINTS)))
        filtered = [filtered[i] for i in indices]
    zone = ZoneInfo(tz)
    long_range = period_label in ('7天', '7d')
    values = []
    for t, v in filtered:
        d = datetime.fromtimestamp(t, zone)
        label = d.strftime('%m-%d %H:%M' if long_range else '%H:%M')
        values.append({'time': label, 'value': round(v, 2)})
    # chart_spec is a VChart spec (not ECharts or a PNG URL).
    spec = {
        'type': 'line',
        'data': {'values': values},
        'xField': 'time',
        'yField': 'value',
        'point': {'visible': False},
        'line': {'style': {'lineWidth': 2}},
    }
    title = f"{cfg['name']} · 最近{period_label}"
    unit = cfg['unit']
    return {
        'schema': '2.0',
        'config': {'wide_screen_mode': True},
        'header': {
            'title': {'tag': 'plain_text', 'content': title},
            'template': 'blue'
        },
        'body': {'elements': [
            {'tag': 'markdown', 'content': f"**{cfg['name']}历史趋势**（{unit}）"},
            {'tag': 'chart', 'aspect_ratio': '16:9', 'preview': True,
             'color_theme': 'brand', 'chart_spec': spec},
            {'tag': 'markdown', 'content': '数据源：Prometheus；数值为采样或降采样结果，仅供环境趋势参考。'},
        ]},
    }
