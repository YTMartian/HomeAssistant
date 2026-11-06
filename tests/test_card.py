import json
import pytest
from app.chart_card import build_trend_card

cfg = {'name': '分贝', 'unit': 'dB SPL(估算)'}

def test_chart_is_native_component_no_image():
    samples = [(1730000000 + i * 10, 35.0 + (i % 8)) for i in range(1000)]
    card = build_trend_card(samples, cfg, '24小时', 'Asia/Shanghai')
    assert card['schema'] == '2.0'
    chart = [e for e in card['body']['elements'] if e['tag'] == 'chart'][0]
    assert chart['chart_spec']['type'] == 'line'
    assert 2 <= len(chart['chart_spec']['data']['values']) <= 120
    assert 'img' not in json.dumps(card)
    assert len(json.dumps(card, ensure_ascii=False).encode()) < 30000

def test_insufficient_data():
    with pytest.raises(ValueError):
        build_trend_card([(1, 2.0)], cfg, '1小时')

def test_climate_card():
    card = build_trend_card([(1730000000,24.5), (1730000300,25)], {'name':'温度','unit':'°C'},'1小时')
    assert card['header']['title']['content'].startswith('温度')
