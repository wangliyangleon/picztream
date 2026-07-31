import subprocess

from orchestrator.stage import StageContext
from orchestrator.types import StageOutput
from pzt_client import PztClient
from stages.style_apply_all import StyleApplyAllStage


def _make_client(call_log, fail_paths=None):
    fail_paths = fail_paths or set()

    def fake_runner(argv):
        call_log.append(argv)
        assert argv[1] == "recipe" and argv[2] == "apply"
        path = argv[4]
        if path in fail_paths:
            return subprocess.CompletedProcess(
                argv, 1, stdout="", stderr='{"error": "recipe_not_found", "message": "no such recipe"}\n')
        return subprocess.CompletedProcess(
            argv, 0, stdout='{"applied": true, "recipe_name": "' + argv[5] + '"}', stderr="")

    return PztClient(pzt_bin="/fake/pzt", runner=fake_runner)


def _ctx(selected, chosen_recipe, preview_photo):
    return StageContext(run_id="run-1", project_id="proj-1", outputs={
        "Curate": StageOutput(ok=True, data={"selected": selected}),
        "Style": StageOutput(ok=True, data={"chosen_recipe": chosen_recipe, "preview_photo": preview_photo}),
    })


def test_applies_the_chosen_recipe_to_every_remaining_selected_photo():
    call_log = []
    client = _make_client(call_log)
    stage = StyleApplyAllStage(client=client)

    output = stage.run(_ctx(["a.jpg", "b.jpg", "c.jpg"], "Havana 1959", "a.jpg"), {})

    # a.jpg 是代表图，Style 已经套过了，这里不该重复调用
    assert len(call_log) == 2
    called_paths = {argv[4] for argv in call_log}
    assert called_paths == {"b.jpg", "c.jpg"}
    assert output.ok is True
    assert output.data["applied"] == {"a.jpg": "Havana 1959", "b.jpg": "Havana 1959", "c.jpg": "Havana 1959"}
    assert output.skipped == []


def test_no_chosen_recipe_is_a_no_op():
    call_log = []
    client = _make_client(call_log)
    stage = StyleApplyAllStage(client=client)

    output = stage.run(_ctx(["a.jpg", "b.jpg"], None, None), {})

    assert output.ok is True
    assert output.data == {"applied": {}}
    assert call_log == []


def test_partial_apply_failure_keeps_successes_and_reports_skipped():
    call_log = []
    client = _make_client(call_log, fail_paths={"b.jpg"})
    stage = StyleApplyAllStage(client=client)

    output = stage.run(_ctx(["a.jpg", "b.jpg", "c.jpg"], "Havana 1959", "a.jpg"), {})

    assert output.ok is True
    assert output.data["applied"] == {"a.jpg": "Havana 1959", "c.jpg": "Havana 1959"}
    assert len(output.skipped) == 1
    assert output.skipped[0]["path"] == "b.jpg"
    assert "recipe_not_found" in output.skipped[0]["error"]


def test_total_apply_failure_of_remaining_photos_reports_stage_failure():
    # 代表图 a.jpg 在 Style 阶段已经套用成功了，即便剩下的照片全部套用
    # 失败，applied 里也不会是空的——"全部失败"这里指的是"剩余（除代表
    # 图外）的照片全部失败"，不是整批 applied 为空。
    call_log = []
    client = _make_client(call_log, fail_paths={"b.jpg", "c.jpg"})
    stage = StyleApplyAllStage(client=client)

    output = stage.run(_ctx(["a.jpg", "b.jpg", "c.jpg"], "Havana 1959", "a.jpg"), {})

    assert output.ok is False
    assert output.data["applied"] == {"a.jpg": "Havana 1959"}
    assert len(output.skipped) == 2


# -- 进度上报（T-8 G2）--


def _ctx_with_progress(selected, chosen_recipe, preview_photo, seen):
    ctx = _ctx(selected, chosen_recipe, preview_photo)
    ctx.on_progress = lambda done, total, kind: seen.append((done, total, kind))
    return ctx


def test_reports_progress_over_all_selected_photos():
    # 分母是用户选中的总张数，不是这个 stage 还要跑的张数：代表图已经在
    # Style 阶段套好了，报 "2/2" 而实际有 3 张会让用户以为漏了一张。
    seen = []
    stage = StyleApplyAllStage(client=_make_client([]))

    stage.run(_ctx_with_progress(["a.jpg", "b.jpg", "c.jpg"], "Havana 1959", "a.jpg", seen), {})

    assert seen == [(2, 3, "photos"), (3, 3, "photos")]


def test_progress_counts_from_zero_when_there_is_no_preview_photo():
    seen = []
    stage = StyleApplyAllStage(client=_make_client([]))

    stage.run(_ctx_with_progress(["a.jpg", "b.jpg"], "Havana 1959", None, seen), {})

    assert seen == [(1, 2, "photos"), (2, 2, "photos")]


def test_failed_photos_still_advance_the_counter():
    # 单张失败是软失败（criticality="optional"，skipped 里记一笔就继续）。
    # 不推进计数的话进度会卡住不动，用户看到的是"卡死"而不是"跳过了一张"。
    seen = []
    stage = StyleApplyAllStage(client=_make_client([], fail_paths={"b.jpg"}))

    output = stage.run(_ctx_with_progress(["a.jpg", "b.jpg", "c.jpg"], "Havana 1959", "a.jpg", seen), {})

    assert seen == [(2, 3, "photos"), (3, 3, "photos")]
    assert len(output.skipped) == 1


def test_no_chosen_recipe_reports_nothing():
    seen = []
    stage = StyleApplyAllStage(client=_make_client([]))

    stage.run(_ctx_with_progress(["a.jpg"], None, None, seen), {})

    assert seen == []
