import subprocess

from orchestrator.stage import StageContext
from orchestrator.types import StageOutput
from pzt_client import PztClient
from stages.deliver import DeliverStage


class FakeTransport:
    def __init__(self):
        self.sent_files = []
        self.sent_texts = []

    def send_file(self, chat_id, path):
        self.sent_files.append((chat_id, path))

    def send_photo(self, chat_id, path):
        self.sent_files.append((chat_id, path))

    def send_text(self, chat_id, text):
        self.sent_texts.append((chat_id, text))


def make_curate_output_ctx(selected):
    return StageContext(
        run_id="run-1", project_id="proj-1",
        outputs={"Curate": StageOutput(ok=True, data={"selected": selected})},
    )


def make_curate_and_style_output_ctx(selected, applied):
    return StageContext(
        run_id="run-1", project_id="proj-1",
        outputs={
            "Curate": StageOutput(ok=True, data={"selected": selected}),
            "StyleApplyAll": StageOutput(ok=True, data={"applied": applied}),
        },
    )


def test_deliver_inputs_default_to_curate():
    # 现在只有 run_telegram.py 的 Plan 才含 Style/StyleApplyAll，
    # run_watchfolder.py/run_intent.py 完全不含这两个 stage，默认值不能
    # 假设它们存在——需要 Style/StyleApplyAll 的调用方自己显式覆盖。
    stage = DeliverStage(client=None, transport=None, marker_dir=None, staging_dir=None)
    assert stage.inputs == ["Curate"]


def test_deliver_is_critical():
    # AG-06 拍板：交付是用户点"满意"后的明确诉求，导出失败必须让 run FAILED，
    # 不能被 optional 吞成 SKIPPED 让 run 误报"这批就处理完啦"。
    stage = DeliverStage(client=None, transport=None, marker_dir=None, staging_dir=None)
    assert stage.criticality == "critical"


def test_deliver_exports_to_its_own_staging_dir_then_sends_each_selected_file(tmp_path):
    captured = {}

    def fake_runner(argv):
        captured["argv"] = argv
        return subprocess.CompletedProcess(argv, 0, stdout='{"exported": 2, "skipped": [], "created_dir": true}\n', stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    transport = FakeTransport()
    staging_dir = tmp_path / "staging"
    stage = DeliverStage(client=client, transport=transport, marker_dir=tmp_path / "delivered", staging_dir=staging_dir)
    ctx = make_curate_output_ctx(["a.jpg", "b.jpg"])

    output = stage.run(ctx, {})

    run_staging_dir = staging_dir / "run-1"
    assert captured["argv"] == ["/fake/pzt", "export-images", "proj-1", "a.jpg", "b.jpg", str(run_staging_dir), "--json"]
    assert output.ok is True
    assert transport.sent_files == [
        ("watchfolder", str(run_staging_dir / "a.jpg")),
        ("watchfolder", str(run_staging_dir / "b.jpg")),
    ]
    assert len(transport.sent_texts) == 1


def test_deliver_stages_into_a_directory_independent_of_any_caller_supplied_out_folder(tmp_path):
    # 真机复现过的 bug：export-images 的落地目录和 transport 最终交付
    # 的目的地如果是同一个文件夹，WatchFolderTransport.send_file() 会
    # 把已经导出的文件"拷贝"回它自己所在的路径，shutil 直接
    # SameFileError。DeliverStage 自己的暂存目录必须独立于 transport
    # 的 out_dir，哪怕调用方把某个跟 out_dir 撞在一起的路径传进
    # params 也不该影响——本来 params 里也不该有这个概念了，这条测试
    # 顺便锁住"DeliverStage 不读 params 里的 out_folder"这件事。
    def fake_runner(argv):
        return subprocess.CompletedProcess(argv, 0, stdout='{"exported": 1, "skipped": [], "created_dir": true}\n', stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    transport = FakeTransport()
    staging_dir = tmp_path / "staging"
    stage = DeliverStage(client=client, transport=transport, marker_dir=tmp_path / "delivered", staging_dir=staging_dir)
    ctx = make_curate_output_ctx(["a.jpg"])

    stage.run(ctx, {"out_folder": str(tmp_path / "out")})  # 就算传了同名参数也不该被读

    sent_path = transport.sent_files[0][1]
    assert not sent_path.startswith(str(tmp_path / "out"))
    assert sent_path.startswith(str(staging_dir))


def test_deliver_writes_marker_after_successful_send(tmp_path):
    def fake_runner(argv):
        return subprocess.CompletedProcess(argv, 0, stdout='{"exported": 1, "skipped": [], "created_dir": true}\n', stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    transport = FakeTransport()
    marker_dir = tmp_path / "delivered"
    stage = DeliverStage(client=client, transport=transport, marker_dir=marker_dir, staging_dir=tmp_path / "staging")
    ctx = make_curate_output_ctx(["a.jpg"])

    stage.run(ctx, {})

    markers = list(marker_dir.glob("run-1-*.json"))
    assert len(markers) == 1


def test_deliver_skips_resend_when_marker_already_exists(tmp_path):
    calls = []

    def fake_runner(argv):
        calls.append(argv)
        return subprocess.CompletedProcess(argv, 0, stdout='{"exported": 1, "skipped": [], "created_dir": true}\n', stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    transport = FakeTransport()
    marker_dir = tmp_path / "delivered"
    stage = DeliverStage(client=client, transport=transport, marker_dir=marker_dir, staging_dir=tmp_path / "staging")
    ctx = make_curate_output_ctx(["a.jpg"])

    stage.run(ctx, {})  # 第一次真正发送
    output = stage.run(ctx, {})  # 第二次:模拟重跑

    assert len(calls) == 1  # export-images 只被真正调用了一次
    assert len(transport.sent_files) == 1  # 只发送了一次，没有重发
    assert output.ok is True
    assert output.data.get("already_delivered") is True


def test_deliver_maps_export_failure_to_stage_failure(tmp_path):
    def fake_runner(argv):
        return subprocess.CompletedProcess(argv, 1, stdout="", stderr='{"error": "io_error", "message": "x"}\n')

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    transport = FakeTransport()
    stage = DeliverStage(client=client, transport=transport, marker_dir=tmp_path / "delivered", staging_dir=tmp_path / "staging")
    ctx = make_curate_output_ctx(["a.jpg"])

    output = stage.run(ctx, {})

    assert output.ok is False
    assert len(transport.sent_files) == 0


def test_deliver_resends_when_curate_selection_changes_after_adjustment(tmp_path):
    calls = []

    def fake_runner(argv):
        calls.append(argv)
        return subprocess.CompletedProcess(argv, 0, stdout='{"exported": 1, "skipped": [], "created_dir": true}\n', stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    transport = FakeTransport()
    marker_dir = tmp_path / "delivered"
    stage = DeliverStage(client=client, transport=transport, marker_dir=marker_dir, staging_dir=tmp_path / "staging")

    stage.run(make_curate_output_ctx(["a.jpg"]), {})  # 第一次交付选片 a
    output = stage.run(make_curate_output_ctx(["b.jpg"]), {})  # 调整后换成选片 b，同一个 run_id

    assert len(calls) == 2  # export-images 对两次不同的选片都真的调用了
    assert len(transport.sent_files) == 2
    assert transport.sent_files[1][1].endswith("b.jpg")
    assert output.data.get("already_delivered") is not True


def test_deliver_resends_when_style_changed_for_the_same_selection(tmp_path):
    # StyleApplyAll 只调颜色不改文件路径，同样的 selected 列表配合不同
    # 的风格必须被当成"不同的一次交付"，不能被去重标记误判成已经交付
    # 过——见 docs/W2026-07-15_AgentStyle_Eng_Design.md 第七节。
    calls = []

    def fake_runner(argv):
        calls.append(argv)
        return subprocess.CompletedProcess(argv, 0, stdout='{"exported": 1, "skipped": [], "created_dir": true}\n', stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    transport = FakeTransport()
    marker_dir = tmp_path / "delivered"
    stage = DeliverStage(client=client, transport=transport, marker_dir=marker_dir, staging_dir=tmp_path / "staging")

    stage.run(make_curate_and_style_output_ctx(["a.jpg"], {"a.jpg": "Havana 1959"}), {})
    output = stage.run(make_curate_and_style_output_ctx(["a.jpg"], {"a.jpg": "Tokyo 1966"}), {})

    assert len(calls) == 2  # export-images 对两次不同的风格都真的调用了
    assert len(transport.sent_files) == 2
    assert output.data.get("already_delivered") is not True


def test_deliver_resumes_only_unsent_items_after_a_crash_mid_batch(tmp_path):
    calls = []

    def fake_runner(argv):
        calls.append(argv)
        return subprocess.CompletedProcess(
            argv, 0, stdout='{"exported": 3, "skipped": [], "created_dir": true}\n', stderr="")

    class CrashingTransport(FakeTransport):
        def __init__(self):
            super().__init__()

        def send_file(self, chat_id, path):
            if len(self.sent_files) >= 1:
                raise RuntimeError("simulated crash mid-delivery")
            super().send_file(chat_id, path)

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    crashing_transport = CrashingTransport()
    marker_dir = tmp_path / "delivered"
    stage = DeliverStage(client=client, transport=crashing_transport, marker_dir=marker_dir,
                          staging_dir=tmp_path / "staging")
    ctx = make_curate_output_ctx(["a.jpg", "b.jpg", "c.jpg"])

    try:
        stage.run(ctx, {})
    except RuntimeError:
        pass  # 模拟"发送中途整个进程崩了"

    assert len(crashing_transport.sent_files) == 1  # 只有第一张真的发出去了

    # "重启"：全新一个 DeliverStage 实例（同一个 marker_dir），用不崩溃
    # 的 transport 继续跑，模拟进程重启后重新调用同一个 Stage。
    resumed_transport = FakeTransport()
    resumed_stage = DeliverStage(client=client, transport=resumed_transport, marker_dir=marker_dir,
                                  staging_dir=tmp_path / "staging")
    output = resumed_stage.run(ctx, {})

    assert len(resumed_transport.sent_files) == 2  # 只补发没发过的两张
    assert resumed_transport.sent_files[0][1].endswith("b.jpg")
    assert resumed_transport.sent_files[1][1].endswith("c.jpg")
    assert output.ok is True
