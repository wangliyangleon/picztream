import subprocess

from orchestrator.driver import Driver
from orchestrator.types import RunState, RunStatus, StageStatus
from pzt_client import PztClient
from run_watchfolder import build_plan, build_transport
from stages.curate import CurateStage
from stages.dedup import DedupStage
from stages.deliver import DeliverStage
from stages.evaluate import EvaluateStage
from stages.ingest import IngestStage
from store.run_store import RunStore


def test_build_plan_produces_five_stages_all_gates_off():
    plan = build_plan(
        in_folder="/tmp/in", out_folder="/tmp/out", count=9,
        provider="gemini", apply_tag="精选", auto_reject=True,
    )

    assert [s.name for s in plan.stages] == ["Ingest", "Evaluate", "Dedup", "Curate", "Deliver"]
    assert all(s.gate == "off" for s in plan.stages)
    assert plan.stages[0].params == {"folder": "/tmp/in"}
    assert plan.stages[3].params == {"count": 9, "apply_tag": "精选"}
    assert plan.stages[4].params == {"out_folder": "/tmp/out"}


def test_build_transport_reconstructs_in_and_out_dir_from_persisted_plan(tmp_path):
    plan = build_plan(
        in_folder=str(tmp_path / "in"), out_folder=str(tmp_path / "out"), count=9,
        provider="gemini", apply_tag="精选", auto_reject=True,
    )
    run = RunState(run_id="run-1", project_id="run-1", plan=plan,
                    stage_states={s.name: StageStatus.PENDING for s in plan.stages}, status=RunStatus.RUNNING)

    transport = build_transport(run)

    assert transport.in_dir == tmp_path / "in"
    assert transport.out_dir == tmp_path / "out"


def _make_fake_client(responses):
    # responses: dict[subcommand -> stdout_json_str]，用第一个位置参数
    # (子命令名)分发,不校验其余参数——D3 已经逐条测过参数拼装，这里只
    # 关心整条链路串不串得起来 + Deliver 幂等/续跑。
    def fake_runner(argv):
        subcommand = argv[1]
        return subprocess.CompletedProcess(argv, 0, stdout=responses[subcommand], stderr="")

    return PztClient(pzt_bin="/fake/pzt", runner=fake_runner)


class FakeTransport:
    def __init__(self, in_dir, out_dir):
        self.in_dir = in_dir
        self.out_dir = out_dir
        self.sent_files = []
        self.sent_texts = []

    def receive(self):
        return []

    def send_file(self, chat_id, path):
        self.sent_files.append(path)

    def send_photo(self, chat_id, path):
        self.sent_files.append(path)

    def send_text(self, chat_id, text):
        self.sent_texts.append(text)


def _make_pipeline(tmp_path, client, transport):
    marker_dir = tmp_path / "delivered"
    return {
        "Ingest": IngestStage(client=client),
        "Evaluate": EvaluateStage(client=client),
        "Dedup": DedupStage(client=client),
        "Curate": CurateStage(client=client),
        "Deliver": DeliverStage(client=client, transport=transport, marker_dir=marker_dir),
    }


def test_full_pipeline_runs_to_awaiting_review_and_delivers_selected_files(tmp_path):
    client = _make_fake_client({
        "new": '{"project": "run-1", "image_count": 3}',
        "eval": '{"submitted": 3, "evaluated": [], "failed": []}',
        "dedup": '{"groups": 3, "tagged": 0, "skipped_no_capture_time": 0}',
        "curate": '{"requested": 2, "returned": 2, "selected": ["a.jpg", "b.jpg"]}',
        "export-images": '{"exported": 2, "skipped": [], "created_dir": true}',
    })
    transport = FakeTransport(in_dir=tmp_path / "in", out_dir=tmp_path / "out")
    stages = _make_pipeline(tmp_path, client, transport)
    plan = build_plan(in_folder=str(tmp_path / "in"), out_folder=str(tmp_path / "out"),
                       count=2, provider="gemini", apply_tag="精选", auto_reject=True)
    run = RunState(run_id="run-1", project_id="run-1", plan=plan,
                    stage_states={s.name: StageStatus.PENDING for s in plan.stages}, status=RunStatus.RUNNING)
    driver = Driver(stages=stages, store=RunStore(tmp_path / "runs"))

    while run.status == RunStatus.RUNNING:
        driver.advance(run)
    if run.status == RunStatus.AWAITING_REVIEW:
        driver.approve(run)

    assert run.status == RunStatus.DONE
    assert len(transport.sent_files) == 2


def test_crash_before_checkpoint_persists_does_not_resend_via_deliver_marker(tmp_path):
    # 模拟"Deliver 已经真正发送、但 Driver 那次 store.save(run) 落盘之
    # 前进程崩了"：完整跑一遍到 Deliver 完成后，手动把 Deliver 的
    # stage_states 拨回 PENDING(模拟丢失的检查点)，再 advance 一次，
    # 断言 transport 没有收到第二次发送——DeliverStage 自己的 marker
    # 挡住了重发，这是 PRD"幂等交付"验收标准的直接体现。
    client = _make_fake_client({
        "new": '{"project": "run-1", "image_count": 1}',
        "eval": '{"submitted": 1, "evaluated": [], "failed": []}',
        "dedup": '{"groups": 1, "tagged": 0, "skipped_no_capture_time": 0}',
        "curate": '{"requested": 1, "returned": 1, "selected": ["a.jpg"]}',
        "export-images": '{"exported": 1, "skipped": [], "created_dir": true}',
    })
    transport = FakeTransport(in_dir=tmp_path / "in", out_dir=tmp_path / "out")
    stages = _make_pipeline(tmp_path, client, transport)
    plan = build_plan(in_folder=str(tmp_path / "in"), out_folder=str(tmp_path / "out"),
                       count=1, provider="gemini", apply_tag="精选", auto_reject=True)
    run = RunState(run_id="run-1", project_id="run-1", plan=plan,
                    stage_states={s.name: StageStatus.PENDING for s in plan.stages}, status=RunStatus.RUNNING)
    driver = Driver(stages=stages, store=RunStore(tmp_path / "runs"))

    while run.status == RunStatus.RUNNING:
        driver.advance(run)
    assert len(transport.sent_files) == 1

    run.stage_states["Deliver"] = StageStatus.PENDING  # 模拟丢失的检查点
    run.status = RunStatus.RUNNING
    driver.advance(run)

    assert len(transport.sent_files) == 1  # 没有翻倍


def test_crash_after_dedup_resumes_from_curate_without_rerunning_earlier_stages(tmp_path):
    call_log = []

    def fake_runner(argv):
        call_log.append(argv[1])
        responses = {
            "new": '{"project": "run-1", "image_count": 1}',
            "eval": '{"submitted": 1, "evaluated": [], "failed": []}',
            "dedup": '{"groups": 1, "tagged": 0, "skipped_no_capture_time": 0}',
            "curate": '{"requested": 1, "returned": 1, "selected": ["a.jpg"]}',
            "export-images": '{"exported": 1, "skipped": [], "created_dir": true}',
        }
        return subprocess.CompletedProcess(argv, 0, stdout=responses[argv[1]], stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    transport = FakeTransport(in_dir=tmp_path / "in", out_dir=tmp_path / "out")
    stages = _make_pipeline(tmp_path, client, transport)
    plan = build_plan(in_folder=str(tmp_path / "in"), out_folder=str(tmp_path / "out"),
                       count=1, provider="gemini", apply_tag="精选", auto_reject=True)
    run = RunState(run_id="run-1", project_id="run-1", plan=plan,
                    stage_states={s.name: StageStatus.PENDING for s in plan.stages}, status=RunStatus.RUNNING)
    store = RunStore(tmp_path / "runs")
    driver_a = Driver(stages=stages, store=store)

    driver_a.advance(run)  # Ingest
    driver_a.advance(run)  # Evaluate
    driver_a.advance(run)  # Dedup  -- 之后模拟进程崩溃

    assert call_log == ["new", "eval", "dedup"]

    # 模拟重启：全新 Driver/Stage(client 走全新 fake_runner 计数)，从磁
    # 盘重新加载 run_id="run-1"。
    call_log_after_restart = []

    def fake_runner_after_restart(argv):
        call_log_after_restart.append(argv[1])
        responses = {
            "curate": '{"requested": 1, "returned": 1, "selected": ["a.jpg"]}',
            "export-images": '{"exported": 1, "skipped": [], "created_dir": true}',
        }
        return subprocess.CompletedProcess(argv, 0, stdout=responses[argv[1]], stderr="")

    client_b = PztClient(pzt_bin="/fake/pzt", runner=fake_runner_after_restart)
    transport_b = FakeTransport(in_dir=tmp_path / "in", out_dir=tmp_path / "out")
    stages_b = _make_pipeline(tmp_path, client_b, transport_b)
    store_b = RunStore(tmp_path / "runs")
    driver_b = Driver(stages=stages_b, store=store_b)
    reloaded = store_b.load("run-1")

    while reloaded.status == RunStatus.RUNNING:
        driver_b.advance(reloaded)
    if reloaded.status == RunStatus.AWAITING_REVIEW:
        driver_b.approve(reloaded)

    assert call_log_after_restart == ["curate", "export-images"]  # 没有重跑 new/eval/dedup
    assert reloaded.status == RunStatus.DONE
