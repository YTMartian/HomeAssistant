import json
from app.card_ui import (menu_card, current_card, status_card,
                         trend_card, WINDOWS_BY_KEY)
from app.logic import parse_command

sensors = {
    "sound": {"name":"分贝", "enabled": True, "aliases": ["分贝", "声音"],
              "unit":"dB(A)（估算）", "precision":1, "db_energy_avg":True},
    "temperature": {"name":"温度", "enabled":False, "aliases":["温度"], "unit":"°C"},
}

def iter_buttons(card):
    for e in card["body"]["elements"]:
        if e.get("tag") == "button":
            yield e
        elif e.get("tag") == "column_set":
            for c in e["columns"]:
                yield from (x for x in c["elements"] if x.get("tag") == "button")

def test_bare_trend_all_sensors():
    command = parse_command("@智能家庭助手 趋势", sensors)
    assert command.action == "history" and command.sensor == "all" and command.period == 86400

def test_menu_buttons():
    c = menu_card(sensors)
    assert c["schema"] == "2.0"
    buttons = list(iter_buttons(c))
    assert any(x["behaviors"][0]["value"]["action"] == "trend" for x in buttons)
    assert all(x["behaviors"][0]["value"]["sensor"] != "temperature" for x in buttons)

def test_history_native_charts_and_filters():
    cfg = sensors["sound"]
    samples = [(1793600000+i*120, 50+(i%8)) for i in range(80)]
    stats = {"last":57,"min":50,"max":57,"avg":54.2,"count":80}
    c = trend_card([("sound",cfg,samples,stats)], "24h", "Asia/Shanghai")
    assert c["config"]["update_multi"] is True
    assert len([e for e in c["body"]["elements"] if e["tag"] == "chart"]) == 1
    buttons = list(iter_buttons(c))
    ids = {b["behaviors"][0]["value"]["period"] for b in buttons
           if b["behaviors"][0]["value"]["action"] == "trend"}
    assert {"1h","6h","24h","7d"}.issubset(ids)
    assert all(b["behaviors"][0]["type"] == "callback" for b in buttons)
    assert "img" not in json.dumps(c)
    assert len(json.dumps(c,ensure_ascii=False).encode()) < 30000

def test_current_and_status_buttons():
    val={"fresh":True,"value":53.4,"timestamp":1793600000,"age_seconds":2}
    c=current_card("sound", sensors["sound"], val, "Asia/Shanghai")
    assert any(b["behaviors"][0]["value"]["action"] == "trend" for b in iter_buttons(c))
    s=status_card(["🟢 分贝：在线"])
    assert "设备" in s["header"]["title"]["content"] or "状态" in s["header"]["title"]["content"]
