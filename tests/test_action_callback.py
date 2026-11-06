"""Offline simulation of a Feishu Card 2.0 callback (no account credentials)."""
import sys
import types
from types import SimpleNamespace as NS
from app.handler import Bot


def test_card_callback_returns_update(monkeypatch):
    # Substitute only the callback response model; do not need Lark credentials.
    full_name = "lark_oapi.event.callback.model.p2_card_action_trigger"
    for n in ["lark_oapi", "lark_oapi.event", "lark_oapi.event.callback",
              "lark_oapi.event.callback.model"]:
        monkeypatch.setitem(sys.modules, n, types.ModuleType(n))
    fake = types.ModuleType(full_name)
    class Response:
        def __init__(self, data): self.data = data
    fake.P2CardActionTriggerResponse = Response
    monkeypatch.setitem(sys.modules, full_name, fake)

    bot = object.__new__(Bot)
    bot.allow_chats = {"oc_test"}
    bot.sensors = {"sound": {"enabled": True}}
    bot.prom_button = object()
    calls = []
    def render(action, sensor, period, prom):
        calls.append((action, sensor, period, prom))
        return {"schema": "2.0", "body":{"elements":[]}}
    bot._render = render
    event = NS(event=NS(context=NS(open_chat_id="oc_test"),
                        action=NS(value={"action":"trend","sensor":"all","period":"1h"})))
    resp = bot.on_card_action(event)
    assert resp.data["card"]["type"] == "raw"
    assert resp.data["card"]["data"]["schema"] == "2.0"
    assert calls[0][:3] == ("trend", "all", "1h")

    event.event.context.open_chat_id = "oc_other"
    resp2 = bot.on_card_action(event)
    assert resp2.data["toast"]["type"] == "error"
    assert len(calls) == 1
