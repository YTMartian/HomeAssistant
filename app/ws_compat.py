"""Feishu SDK WebSocket compatibility for CARD callback frames.

lark-oapi SDK 1.6.4 had an issue dropping CARD frames. The stable 1.7.x
source still exposes a CARD early return. This small adapter maps the
CARD type to EVENT before deferring to official handling; an EVENT frame
is left unchanged. Remove this adapter when the official SDK fully fixes
this behavior. Scoped only to the card transport, never to sensor data.
"""
import logging
import lark_oapi as lark
from lark_oapi.ws.const import HEADER_TYPE
from lark_oapi.ws.enum import MessageType

log = logging.getLogger("smarthome.ws")

class CardAwareWSClient(lark.ws.Client):
    async def _handle_data_frame(self, frame):
        for item in frame.headers:
            if item.key == HEADER_TYPE and item.value == MessageType.CARD.value:
                item.value = MessageType.EVENT.value
                log.debug("CARD frame forwarded to event callback dispatcher")
                break
        return await super()._handle_data_frame(frame)
