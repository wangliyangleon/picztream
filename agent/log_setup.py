"""常驻 agent 的日志配置（AG-21）：把原来散在 consumer/worker/transport 的
stdout print 收敛到 `pzt.agent` logger 树，带时间戳、级别、logger 名，同时
落盘 `<state_dir>/agent.log`（滚动）+ console。各模块只 `logging.getLogger
("pzt.agent.<name>")` 取 logger 打日志，是否落盘/去哪落由这里一处决定。
"""
from __future__ import annotations

import logging
from logging.handlers import RotatingFileHandler
from pathlib import Path

_LOGGER_NAME = "pzt.agent"
_FORMAT = "%(asctime)s %(levelname)s %(name)s: %(message)s"


def configure_logging(state_dir: Path | str, level: int = logging.INFO) -> logging.Logger:
    """配置 pzt.agent logger：console + 滚动文件。幂等（重复调用先清旧
    handler，不叠加）。返回配好的 logger。"""
    state_dir = Path(state_dir)
    state_dir.mkdir(parents=True, exist_ok=True)

    logger = logging.getLogger(_LOGGER_NAME)
    logger.setLevel(level)
    logger.propagate = False  # 不冒泡到 root，避免重复/污染
    for h in list(logger.handlers):
        logger.removeHandler(h)

    formatter = logging.Formatter(_FORMAT)
    console = logging.StreamHandler()
    console.setFormatter(formatter)
    logger.addHandler(console)

    file_handler = RotatingFileHandler(
        state_dir / "agent.log", maxBytes=5 * 1024 * 1024, backupCount=3, encoding="utf-8")
    file_handler.setFormatter(formatter)
    logger.addHandler(file_handler)
    return logger
