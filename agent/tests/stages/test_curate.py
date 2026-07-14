import subprocess

from orchestrator.stage import StageContext
from pzt_client import PztClient
from stages.curate import CurateStage


def _make_client(responses_by_subcommand, call_log):
    def fake_runner(argv):
        call_log.append(argv)
        subcommand = argv[1]
        return subprocess.CompletedProcess(argv, 0, stdout=responses_by_subcommand[subcommand], stderr="")
    return PztClient(pzt_bin="/fake/pzt", runner=fake_runner)


def test_curate_fresh_run_calls_curate_then_retags_final_selection():
    call_log = []
    client = _make_client({"curate": '{"requested": 9, "returned": 9, "selected": ["a.jpg", "b.jpg"]}',
                            "tag": '{}'}, call_log)
    stage = CurateStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {"count": 9, "apply_tag": "精选"})

    assert call_log[0] == ["/fake/pzt", "curate", "proj-1", "--count", "9", "--apply-tag", "精选", "--json"]
    assert call_log[1] == ["/fake/pzt", "tag", "clear", "proj-1", "精选", "--json"]
    assert call_log[2] == ["/fake/pzt", "tag", "apply", "proj-1", "a.jpg", "精选", "--json"]
    assert call_log[3] == ["/fake/pzt", "tag", "apply", "proj-1", "b.jpg", "精选", "--json"]
    assert len(call_log) == 4
    assert output.ok is True
    assert output.data["selected"] == ["a.jpg", "b.jpg"]
    assert output.data["requested"] == 9
    assert output.data["returned"] == 2


def test_curate_maps_command_failure_to_stage_failure():
    def fake_runner(argv):
        return subprocess.CompletedProcess(argv, 1, stdout="", stderr='{"error": "usage", "message": "x"}\n')

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    stage = CurateStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {"count": 9, "apply_tag": "精选"})

    assert output.ok is False


def test_curate_with_exclude_overfetches_filters_and_truncates():
    call_log = []
    client = _make_client({"curate": '{"requested": 3, "returned": 3, "selected": ["a.jpg", "b.jpg", "c.jpg"]}',
                            "tag": '{}'}, call_log)
    stage = CurateStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {"count": 2, "apply_tag": "精选", "exclude": ["b.jpg"]})

    assert call_log[0] == ["/fake/pzt", "curate", "proj-1", "--count", "3", "--apply-tag", "精选", "--json"]
    assert output.data["selected"] == ["a.jpg", "c.jpg"]
    assert output.data["requested"] == 2
    assert output.data["returned"] == 2
    # 重新打标只落在最终裁掉 exclude 之后的两张：over-fetch 出来但被排
    # 除的 b.jpg 不该保留标签。
    assert call_log[1] == ["/fake/pzt", "tag", "clear", "proj-1", "精选", "--json"]
    assert call_log[2] == ["/fake/pzt", "tag", "apply", "proj-1", "a.jpg", "精选", "--json"]
    assert call_log[3] == ["/fake/pzt", "tag", "apply", "proj-1", "c.jpg", "精选", "--json"]
    assert len(call_log) == 4
