"""Feishu Smart Home Assistant; messages and card callbacks via WebSocket."""
import logging
import os
import sys
import lark_oapi as lark
from .handler import Bot
from .ws_compat import CardAwareWSClient


def main():
    logging.basicConfig(level=os.getenv("LOG_LEVEL", "INFO"),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    required = ["FEISHU_APP_ID", "FEISHU_APP_SECRET", "PROMETHEUS_URL", "SENSORS_CONFIG"]
    missing = [k for k in required if not os.getenv(k)]
    if missing:
        sys.exit("缺少环境变量: " + ", ".join(missing))
    env = dict(os.environ)
    bot = Bot(env)
    handler = (lark.EventDispatcherHandler.builder("", "")
        .register_p2_im_message_receive_v1(bot.on_message)
        .register_p2_card_action_trigger(bot.on_card_action)
        .build())
    cli = CardAwareWSClient(env["FEISHU_APP_ID"], env["FEISHU_APP_SECRET"],
                            event_handler=handler, log_level=lark.LogLevel.INFO)
    logging.info("智能家庭助手按钮版启动，使用 WebSocket 消息与卡片回调。")
    cli.start()

if __name__ == "__main__":
    main()
