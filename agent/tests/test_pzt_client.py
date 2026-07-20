import subprocess

import pytest

from pzt_client import PztClient, PztCommandError, default_pzt_bin


def test_call_appends_json_flag_and_returns_parsed_stdout():
    captured = {}

    def fake_runner(argv):
        captured["argv"] = argv
        return subprocess.CompletedProcess(argv, 0, stdout='{"a": 1}\n', stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    result = client.call("images", "proj-1")

    assert result == {"a": 1}
    assert captured["argv"] == ["/fake/pzt", "images", "proj-1", "--json"]


def test_call_raises_pzt_command_error_with_parsed_stderr_on_failure():
    def fake_runner(argv):
        return subprocess.CompletedProcess(
            argv, 1, stdout="", stderr='{"error": "project_not_found", "message": "project not found: x"}\n'
        )

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    with pytest.raises(PztCommandError) as exc_info:
        client.call("images", "x")

    assert exc_info.value.code == "project_not_found"
    assert exc_info.value.message == "project not found: x"


def test_call_falls_back_to_raw_stderr_when_it_is_not_json():
    def fake_runner(argv):
        return subprocess.CompletedProcess(argv, 1, stdout="", stderr="segfault, no JSON here")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    with pytest.raises(PztCommandError) as exc_info:
        client.call("images", "x")

    assert exc_info.value.code == "unknown"
    assert "segfault" in exc_info.value.message


def test_default_pzt_bin_points_at_repo_build_release_cli_pzt(monkeypatch):
    monkeypatch.delenv("PZT_BIN", raising=False)
    assert default_pzt_bin().parts[-3:] == ("build_release", "cli", "pzt")


def test_default_pzt_bin_respects_env_override(monkeypatch):
    monkeypatch.setenv("PZT_BIN", "/custom/path/pzt")
    assert default_pzt_bin() == __import__("pathlib").Path("/custom/path/pzt")


def test_default_pzt_bin_prefers_repo_build_over_path(monkeypatch):
    # 仓库内构建物存在时优先用它(dev 用自己刚构建的,不用 PATH 上 brew 装的)。
    from pathlib import Path
    monkeypatch.delenv("PZT_BIN", raising=False)
    monkeypatch.setattr(Path, "exists", lambda self: True)
    monkeypatch.setattr("pzt_client.shutil.which", lambda name: "/opt/homebrew/bin/pzt")
    assert default_pzt_bin().parts[-3:] == ("build_release", "cli", "pzt")


def test_default_pzt_bin_falls_back_to_path_when_no_repo_build(monkeypatch):
    # brew 装的 agent 没有仓库构建物:回落到 PATH 上的 pzt(pzt-agent depends_on pzt)。
    from pathlib import Path
    monkeypatch.delenv("PZT_BIN", raising=False)
    monkeypatch.setattr(Path, "exists", lambda self: False)
    monkeypatch.setattr("pzt_client.shutil.which", lambda name: "/opt/homebrew/bin/pzt")
    assert default_pzt_bin() == Path("/opt/homebrew/bin/pzt")


@pytest.mark.skipif(not default_pzt_bin().exists(), reason="build_release/cli/pzt not built")
def test_real_binary_wiring_smoke():
    # 唯一一条碰真二进制的测试:用一个必定报错、不产生任何云端调用/不
    # 花钱的命令(查一个不存在的项目),验证 PztClient 真的能起子进程、
    # 真的能把 stderr 的 JSON 错误解析回 PztCommandError——只验证这一
    # 层"接线"，不验证任何业务逻辑(那是 core 自己的 CLI smoke 测的事)。
    client = PztClient()
    with pytest.raises(PztCommandError) as exc_info:
        client.call("images", "this-project-does-not-exist-d1-smoke")
    assert exc_info.value.code == "project_not_found"
