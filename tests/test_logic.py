from app.logic import parse_command, clean_text, summarize, load_sensors

sensors = {
    "sound": {"aliases": ["分贝", "噪声", "声音"], "promql": "sound_level_db", "name": "分贝"},
    "temperature": {"aliases": ["温度", "室温"], "promql": "temp", "name": "温度"},
}

def test_cmds():
    assert parse_command("@_user_1 当前分贝", sensors).action == "current"
    c = parse_command("@智能家庭助手 分贝趋势 24小时", sensors)
    assert c.action == "history" and c.sensor == "sound" and c.period == 86400
    assert parse_command("噪声历史 7天", sensors).period == 604800
    assert parse_command("室温", sensors).sensor == "temperature"
    assert parse_command("状态", sensors).action == "status"
    assert parse_command("温度 6小时", sensors).action == "history"
    assert parse_command("帮助", sensors).action == "menu"
    assert parse_command("趋势", sensors).action == "history"
    assert parse_command("趋势", sensors).sensor == "all"
    assert parse_command("菜单", sensors).action == "menu"

def test_decibel_energy_average():
    v = summarize([(1, 50), (2, 60)], True)
    assert round(v["avg"], 2) == 57.4
    assert summarize([(1, 10), (2, 20)], False)["avg"] == 15.0

def test_mention():
    assert clean_text('<at user_id="ou_a">智能家庭助手</at> 分贝') == "分贝"
