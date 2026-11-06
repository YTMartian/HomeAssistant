"""Prometheus HTTP read-only client."""
from __future__ import annotations
import math
import time
from urllib.parse import urljoin
import requests

class PrometheusError(RuntimeError):
    pass

class Prometheus:
    def __init__(self, base_url, timeout=6):
        self.base = base_url.rstrip("/") + "/"
        self.timeout = timeout
        self.http = requests.Session()

    def _request(self, endpoint, params):
        try:
            r = self.http.get(urljoin(self.base, f"api/v1/{endpoint}"), params=params, timeout=self.timeout)
            r.raise_for_status()
            data = r.json()
        except (requests.RequestException, ValueError) as e:
            raise PrometheusError(f"Prometheus 请求失败: {e.__class__.__name__}") from e
        if data.get("status") != "success":
            raise PrometheusError(f"Prometheus 查询失败: {data.get('error', 'unknown')}")
        return data.get("data", {}).get("result", [])

    def current(self, query, stale_after=30):
        now = time.time()
        series = self._request("query", {"query": query})
        values = []
        for s in series:
            sample = s.get("value")
            if not sample:
                continue
            try:
                ts, val = float(sample[0]), float(sample[1])
            except (ValueError, TypeError):
                continue
            if math.isfinite(val):
                values.append((ts, val, s.get("metric", {})))
        if not values:
            return None
        # One expected sensor per config query. Prefer newest if multiple series match.
        ts, val, labels = max(values, key=lambda v: v[0])
        return {"timestamp": ts, "value": val, "labels": labels,
                "age_seconds": max(0, now-ts), "fresh": now-ts <= stale_after}

    def history(self, query, seconds, max_points=480):
        end = int(time.time())
        step = max(5, math.ceil(seconds/max_points))
        start = end-seconds
        result = self._request("query_range", {
            "query": query, "start": start, "end": end, "step": step
        })
        if not result:
            return []
        series = max(result, key=lambda s: len(s.get("values", [])))
        parsed = []
        for t, v in series.get("values", []):
            try:
                f = float(v)
                if math.isfinite(f):
                    parsed.append((float(t), f))
            except (ValueError, TypeError):
                continue
        return parsed
