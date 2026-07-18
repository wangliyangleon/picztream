import logging

import pytest

from log_setup import configure_logging


@pytest.fixture
def clean_agent_logger():
    # 隔离全局 pzt.agent logger 状态，测完还原，别把 handler 泄漏给其它测试。
    logger = logging.getLogger("pzt.agent")
    saved_handlers, saved_prop, saved_level = logger.handlers[:], logger.propagate, logger.level
    logger.handlers = []
    yield logger
    for h in logger.handlers:
        h.close()
    logger.handlers = saved_handlers
    logger.propagate = saved_prop
    logger.level = saved_level


def test_configure_logging_writes_timestamped_file(tmp_path, clean_agent_logger):
    # AG-21：配置后日志落盘 state_dir/agent.log，带级别/logger 名。
    configure_logging(tmp_path)
    logging.getLogger("pzt.agent.consumer").info("hello world")

    for h in clean_agent_logger.handlers:
        h.flush()
    contents = (tmp_path / "agent.log").read_text(encoding="utf-8")
    assert "hello world" in contents
    assert "pzt.agent.consumer" in contents
    assert "INFO" in contents


def test_configure_logging_is_idempotent(tmp_path, clean_agent_logger):
    # 重复调用不叠加 handler。
    configure_logging(tmp_path)
    n = len(clean_agent_logger.handlers)
    configure_logging(tmp_path)
    assert len(clean_agent_logger.handlers) == n
