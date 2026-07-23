import json
import subprocess

from orchestrator.driver import Driver
from orchestrator.stage import StageContext
from orchestrator.types import Plan, RunState, RunStatus, StageSpec, StageStatus
from pzt_client import PztClient
from stages.curate import CurateStage
from stages.ingest import IngestStage
from store.run_store import RunStore


def _make_client(responses_by_subcommand, call_log):
    def fake_runner(argv):
        call_log.append(argv)
        subcommand = argv[1]
        return subprocess.CompletedProcess(argv, 0, stdout=responses_by_subcommand[subcommand], stderr="")
    return PztClient(pzt_bin="/fake/pzt", runner=fake_runner)


def test_driver_advances_ingest_to_curate_when_dedup_is_absent_from_plan(tmp_path):
    # W2026-07-21 目标三决策二的回归钉子：这是 CurateStage.inputs 声明
    # "Dedup" 时会炸的场景（Driver._next_pending 把 Dedup 当成永远解不开
    # 的依赖），用真实的 IngestStage/CurateStage（而非抽象 FakeStage）跑
    # 一遍才测得出来。
    call_log = []
    client = _make_client({
        "new": '{"image_count": 3}',
        "curate": '{"requested": 3, "returned": 3, "selected": ["a.jpg", "b.jpg", "c.jpg"]}',
        "tag": '{}',
    }, call_log)
    plan = Plan(stages=[
        StageSpec(name="Ingest", params={"folder": "/photos"}),
        StageSpec(name="Curate", params={"count": 3, "apply_tag": "精选"}),
    ])
    run = RunState(run_id="run-1", project_id="proj-1", plan=plan,
                    stage_states={s.name: StageStatus.PENDING for s in plan.stages},
                    status=RunStatus.RUNNING)
    driver = Driver(
        stages={"Ingest": IngestStage(client=client), "Curate": CurateStage(client=client)},
        store=RunStore(tmp_path),
    )

    driver.advance(run)  # Ingest
    driver.advance(run)  # Curate

    assert run.stage_states == {"Ingest": StageStatus.DONE, "Curate": StageStatus.DONE}
    assert run.outputs["Curate"].data["selected"] == ["a.jpg", "b.jpg", "c.jpg"]


def test_curate_stage_declares_ingest_not_dedup_as_input():
    # W2026-07-21 目标三：Dedup 可能不在这次 Plan 里，声明依赖 "Dedup" 会
    # 让 Driver 的拓扑检查把它当成永远解不开的依赖（回归钉子）。
    stage = CurateStage(client=_make_client({}, []))

    assert stage.inputs == ["Ingest"]


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


def test_curate_with_ai_enabled_appends_ai_and_provider_flags():
    call_log = []
    client = _make_client({"curate": '{"requested": 2, "returned": 2, "selected": ["a.jpg", "b.jpg"], '
                                      '"ai_fallback_count": 0}',
                            "tag": '{}'}, call_log)
    stage = CurateStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {"count": 2, "apply_tag": "精选", "ai_enabled": True, "provider": "gemini"})

    assert call_log[0] == ["/fake/pzt", "curate", "proj-1", "--count", "2", "--apply-tag", "精选",
                            "--ai", "--provider", "gemini", "--json"]
    assert output.ok is True


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


def _images_json(*, path_tags):
    images = [{"path": path, "tags": tags} for path, tags in path_tags]
    return json.dumps({"project": "proj-1", "images": images})


def test_curate_passthrough_when_count_is_none_lists_images_excluding_duplicate_and_reject_tags():
    call_log = []
    client = _make_client({
        "images": _images_json(path_tags=[
            ("a.jpg", []),
            ("b.jpg", ["重复"]),
            ("c.jpg", ["废片"]),
            ("d.jpg", ["精选"]),
        ]),
        "tag": '{}',
    }, call_log)
    stage = CurateStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {"count": None, "apply_tag": "精选"})

    assert call_log[0] == ["/fake/pzt", "images", "proj-1", "--json"]
    assert output.ok is True
    assert output.data["selected"] == ["a.jpg", "d.jpg"]
    assert output.data["requested"] is None
    assert output.data["returned"] == 2
    assert call_log[1] == ["/fake/pzt", "tag", "clear", "proj-1", "精选", "--json"]
    assert call_log[2] == ["/fake/pzt", "tag", "apply", "proj-1", "a.jpg", "精选", "--json"]
    assert call_log[3] == ["/fake/pzt", "tag", "apply", "proj-1", "d.jpg", "精选", "--json"]
    assert len(call_log) == 4


def test_curate_passthrough_respects_exclude():
    call_log = []
    client = _make_client({
        "images": _images_json(path_tags=[("a.jpg", []), ("b.jpg", []), ("c.jpg", [])]),
        "tag": '{}',
    }, call_log)
    stage = CurateStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {"count": None, "apply_tag": "精选", "exclude": ["b.jpg"]})

    assert output.data["selected"] == ["a.jpg", "c.jpg"]
    assert call_log[1:] == [
        ["/fake/pzt", "tag", "clear", "proj-1", "精选", "--json"],
        ["/fake/pzt", "tag", "apply", "proj-1", "a.jpg", "精选", "--json"],
        ["/fake/pzt", "tag", "apply", "proj-1", "c.jpg", "精选", "--json"],
    ]


def test_curate_passthrough_ignores_ai_enabled():
    call_log = []
    client = _make_client({
        "images": _images_json(path_tags=[("a.jpg", [])]),
        "tag": '{}',
    }, call_log)
    stage = CurateStage(client=client)
    ctx = StageContext(run_id="run-1", project_id="proj-1", outputs={})

    output = stage.run(ctx, {"count": None, "apply_tag": "精选", "ai_enabled": True, "provider": "gemini"})

    # 没有比较，AI 开关在这个分支下没有意义——压根不该走到 pzt curate。
    assert call_log[0] == ["/fake/pzt", "images", "proj-1", "--json"]
    assert output.ok is True
