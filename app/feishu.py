"""Feishu OpenAPI client: token, text and native chart card replies."""
import json
import threading
import time
import requests

ROOT = "https://open.feishu.cn/open-apis"

class FeishuAPI:
    def __init__(self, app_id, app_secret):
        self.app_id, self.app_secret = app_id, app_secret
        self.http = requests.Session()
        self._token, self._expiry = "", 0.0
        self._lock = threading.RLock()

    def token(self):
        with self._lock:
            if time.monotonic() < self._expiry:
                return self._token
            resp = self.http.post(f"{ROOT}/auth/v3/tenant_access_token/internal", json={
                "app_id": self.app_id, "app_secret": self.app_secret}, timeout=10)
            resp.raise_for_status()
            body = resp.json()
            if body.get("code") != 0:
                raise RuntimeError(f"飞书获取token失败: {body.get('msg')}")
            self._token = body["tenant_access_token"]
            self._expiry = time.monotonic() + max(60, int(body.get("expire", 7200))-180)
            return self._token

    def _headers(self):
        return {"Authorization": "Bearer " + self.token()}

    @staticmethod
    def _ok(response):
        # 飞书经常以 HTTP 400 返回详细的卡片 JSON 字段错误。
        # 先读取响应体再抛异常，否则 requests.raise_for_status() 会隐藏关键原因。
        try:
            body = response.json()
        except ValueError:
            body = None
        if not response.ok or not isinstance(body, dict) or body.get("code") != 0:
            # 不记录授权 header / token，最多打印 1600 字符响应体。
            detail = (json.dumps(body, ensure_ascii=False) if body is not None else response.text)
            raise RuntimeError(f"飞书 API HTTP {response.status_code}, 响应: {detail[:1600]}")
        return body

    def reply(self, message_id, msg_type, content):
        # message_id comes from trusted Feishu event; encoded as URL segment
        from urllib.parse import quote
        url = f"{ROOT}/im/v1/messages/{quote(message_id, safe='')}/reply"
        return self._ok(self.http.post(url, headers=self._headers(), json={
            "msg_type": msg_type,
            "content": json.dumps(content, ensure_ascii=False)
        }, timeout=12))

    def text(self, message_id, content):
        return self.reply(message_id, "text", {"text": content})

    def card(self, message_id, card_json):
        """Reply with Card JSON 2.0 native chart; no image upload involved."""
        return self.reply(message_id, "interactive", card_json)
