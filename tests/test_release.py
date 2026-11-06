import json
from pathlib import Path
from app.card_ui import menu_card, trend_card, current_card
from app.feishu import FeishuAPI

HERE = Path(__file__).resolve().parents[1]

def test_real_issue_300302_fixed_and_sound_only():
    sensors = json.loads((HERE/'config'/'sensors.json').read_text(encoding='utf-8'))
    assert list(sensors) == ['sound']
    assert sensors['sound']['promql'] == 'sound_level_dba{job="sound-meter"}'
    cfg = sensors['sound']
    sample = [(1793600000+i*60, 49+(i%9)) for i in range(25)]
    stats = {'last':54, 'min':49, 'max':57, 'avg':53, 'count':25}
    cards = [menu_card(sensors), trend_card([('sound',cfg,sample,stats)],'1h','Asia/Shanghai')]
    cards.append(current_card('sound', cfg, {'fresh':True,'timestamp':1793600000,'value':52,'age_seconds':2}, 'Asia/Shanghai'))
    for card in cards:
        assert card['schema'] == '2.0'
        assert card['config']['update_multi'] is True
        assert json.dumps(card,ensure_ascii=False)

def test_feishu_http_error_includes_api_reason():
    class Response:
        ok = False
        status_code = 400
        def json(self):
            return {'code':230099,'msg':'update_multi is false'}
    try:
        FeishuAPI._ok(Response())
        assert False, 'should raise'
    except RuntimeError as e:
        assert '230099' in str(e)
        assert 'update_multi' in str(e)
