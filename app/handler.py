"""Bot message and card button callbacks. Only enabled sensors are queried."""
import json
import logging
import time
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
from threading import Lock

from .logic import load_sensors, parse_command, summarize
from .prometheus import Prometheus, PrometheusError
from .feishu import FeishuAPI
from .card_ui import menu_card, current_card, status_card, trend_card, WINDOWS_BY_KEY

log = logging.getLogger("smarthome")

class Bot:
    def __init__(self, env):
        self.sensors = load_sensors(env["SENSORS_CONFIG"])
        self.prom = Prometheus(env["PROMETHEUS_URL"])
        # Card callback has to respond within 3 seconds; one query is <=1.7s.
        self.prom_button = Prometheus(env["PROMETHEUS_URL"], timeout=1.7)
        self.api = FeishuAPI(env["FEISHU_APP_ID"], env["FEISHU_APP_SECRET"])
        self.allow_chats = {x.strip() for x in env.get("ALLOWED_CHAT_IDS", "").split(",") if x.strip()}
        self.tz = env.get("TIMEZONE", "Asia/Shanghai")
        self.pool = ThreadPoolExecutor(max_workers=4, thread_name_prefix="query")
        self.seen = OrderedDict()
        self.lock = Lock()

    def _already_seen(self, key):
        with self.lock:
            now = time.monotonic()
            for k, stamp in list(self.seen.items()):
                if now - stamp > 3600:
                    self.seen.pop(k, None)
            if key in self.seen:
                return True
            self.seen[key] = now
            if len(self.seen) > 4000:
                self.seen.popitem(last=False)
            return False

    def on_message(self, event):
        msg = event.event.message
        sender = getattr(event.event, "sender", None)
        if getattr(sender, "sender_type", "") == "app":
            return
        if getattr(msg, "message_type", "") != "text":
            return
        chat_id = getattr(msg, "chat_id", "")
        if self.allow_chats and chat_id not in self.allow_chats:
            return
        if getattr(msg, "chat_type", "") == "group" and not getattr(msg, "mentions", None):
            return
        msg_id = getattr(msg, "message_id", "")
        if not msg_id or self._already_seen("msg:" + msg_id):
            return
        try:
            content = json.loads(msg.content).get("text", "")
        except (ValueError, TypeError, AttributeError):
            return
        self.pool.submit(self.process, msg_id, content)

    def status_lines(self, prom):
        lines = []
        for key, cfg in self.sensors.items():
            if not cfg.get("enabled"):
                continue   # user currently only wants the sound sensor
            try:
                cur = prom.current(cfg["promql"], cfg.get("stale_seconds", 30))
                if cur and cur["fresh"]:
                    lines.append(f"🟢 {cfg['name']}：在线，{cur['value']:.{cfg.get('precision',1)}f} {cfg['unit']}")
                else:
                    lines.append(f"🔴 {cfg['name']}：无近期数据")
            except PrometheusError:
                lines.append(f"🔴 {cfg['name']}：查询失败")
        return lines or ["暂无已启用传感器"]

    def build_trend(self, scope, period_key, prom):
        if period_key not in WINDOWS_BY_KEY:
            raise ValueError("不支持的时间范围")
        period_label, seconds = WINDOWS_BY_KEY[period_key]
        if scope == "all":
            items = [(k, c) for k, c in self.sensors.items() if c.get("enabled")]
        elif scope in self.sensors and self.sensors[scope].get("enabled"):
            items = [(scope, self.sensors[scope])]
        else:
            raise ValueError("传感器未启用")
        series = []
        for key, cfg in items[:3]:  # limit total chart payload and 3-second callback risk
            try:
                points = prom.history(cfg["promql"], seconds)
                stats = summarize(points, cfg.get("db_energy_avg", False)) if points else None
                series.append((key, cfg, points, stats))
            except PrometheusError:
                log.warning("趋势查询失败：%s", key, exc_info=True)
                series.append((key, cfg, [], None))
        return trend_card(series, period_key, self.tz, scope=scope)

    def _render(self, action, sensor="all", period="24h", prom=None):
        prom = prom or self.prom
        if action == "menu":
            return menu_card(self.sensors)
        if action == "status":
            return status_card(self.status_lines(prom))
        if action == "current":
            cfg = self.sensors.get(sensor)
            if not cfg or not cfg.get("enabled"):
                raise ValueError("传感器尚未启用")
            cur = prom.current(cfg["promql"], cfg.get("stale_seconds", 30))
            return current_card(sensor, cfg, cur, self.tz)
        if action == "trend":
            return self.build_trend(sensor, period, prom)
        raise ValueError("未知操作")

    def process(self, msg_id, content):
        try:
            cmd = parse_command(content, self.sensors)
            action = {"history": "trend"}.get(cmd.action, cmd.action)
            period = {3600:"1h", 21600:"6h", 86400:"24h", 604800:"7d"}.get(cmd.period,"24h")
            card = self._render(action, sensor=cmd.sensor, period=period)
            self.api.card(msg_id, card)
        except Exception:
            log.exception("handle message failed")
            try:
                self.api.text(msg_id, "⚠️ 查询失败，请查看机器人日志与 Prometheus 地址。")
            except Exception:
                log.exception("also failed to send error message")

    def on_card_action(self, event):
        """Synchronous Card 2.0 callback. Must return response within 3 seconds.
        The SDK deserializes the JSON into event.event.action.value.
        """
        from lark_oapi.event.callback.model.p2_card_action_trigger import P2CardActionTriggerResponse
        try:
            body = getattr(event, "event", None)
            context = getattr(event, "context", None) or getattr(body, "context", None)
            chat_id = getattr(context, "open_chat_id", "")
            if self.allow_chats and chat_id not in self.allow_chats:
                return P2CardActionTriggerResponse({"toast": {"type":"error", "content":"未授权的会话"}})
            action_item = getattr(body, "action", None)
            value = getattr(action_item, "value", None)
            if isinstance(value, str):
                value = json.loads(value)
            if not isinstance(value, dict):
                return P2CardActionTriggerResponse({"toast":{"type":"error", "content":"无效操作"}})
            op = value.get("action", "")
            sensor = value.get("sensor", "all")
            period = value.get("period", "24h")
            if op not in {"trend", "current", "status", "menu"} or period not in WINDOWS_BY_KEY:
                return P2CardActionTriggerResponse({"toast":{"type":"error", "content":"无效参数"}})
            if not isinstance(sensor, str) or sensor not in {"all", *self.sensors.keys()}:
                return P2CardActionTriggerResponse({"toast":{"type":"error", "content":"未知传感器"}})
            # On card clicks, direct-return the rebuilt card (not a second chat reply).
            card = self._render(op, sensor=sensor, period=period, prom=self.prom_button)
            return P2CardActionTriggerResponse({"card":{"type":"raw", "data":card}})
        except Exception:
            log.exception("card action callback failed")
            return P2CardActionTriggerResponse({"toast":{"type":"error", "content":"暂时无法切换，请重试"}})
