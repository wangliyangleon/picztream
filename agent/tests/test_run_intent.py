import subprocess

from compose.validate import validate_plan
from orchestrator.driver import Driver
from orchestrator.types import Plan, PlanDelta, RunState, RunStatus, StageSpec, StageStatus
from pzt_client import PztClient
from stages.curate import CurateStage
from stages.dedup import DedupStage
from stages.deliver import DeliverStage
from stages.evaluate import EvaluateStage
from stages.ingest import IngestStage
from store.run_store import RunStore


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

    def send_photo(self, chat_id, path, caption=None):
        self.sent_files.append(path)

    def send_text(self, chat_id, text):
        self.sent_texts.append(text)


def _make_pipeline(tmp_path, client, transport):
    marker_dir = tmp_path / "delivered"
    staging_dir = tmp_path / "staging"
    return {
        "Ingest": IngestStage(client=client),
        "Evaluate": EvaluateStage(client=client),
        "Dedup": DedupStage(client=client),
        "Curate": CurateStage(client=client),
        "Deliver": DeliverStage(client=client, transport=transport, marker_dir=marker_dir, staging_dir=staging_dir),
    }


def _fake_parse_adjustment(msg, run):
    # 站在 parse_adjustment 真实实现的调用方视角：这里直接给"换掉第1
    # 张"该产出的 PlanDelta，只为了验证驱动器接上一次调整之后的链路
    # ("子图重跑、上游不重跑、Deliver 真的把新选片送出去")，
    # parse_adjustment 自己的 LLM 解析逻辑由 test_adjustment_parser.py
    # 单独锁住，不在这里重复测。
    del msg
    selected = run.outputs["Curate"].data["selected"]
    return PlanDelta(stage_name="Curate", params={"exclude": [selected[0]]})


def test_intent_run_adjustment_reruns_only_curate_and_deliver(tmp_path):
    # 跟 run_intent.py::main() 的真实代码路径一致：先 validate_plan 一份
    # 含 Style/StyleApplyAll 的完整 Plan，再把这两个 stage 过滤掉——子增
    # 量 E 这个入口完全没有处理 AWAITING_GATE 的代码，Style 现在是必选
    # 闸门，硬塞进这个入口工作量明显更大，范围上明确只给 run_telegram.py
    # 用。
    plan = validate_plan(Plan(stages=[
        StageSpec(name="Ingest", params={"folder": str(tmp_path / "in")}),
        StageSpec(name="Evaluate", params={"provider": "gemini", "auto_reject": True}),
        StageSpec(name="Dedup"),
        StageSpec(name="Curate", params={"count": 1, "apply_tag": "精选"}),
        StageSpec(name="Style", params={"provider": "gemini"}, gate="required"),
        StageSpec(name="StyleApplyAll", gate="required"),
        StageSpec(name="Deliver", params={"out_folder": str(tmp_path / "out")}),
    ]))
    plan.stages = [s for s in plan.stages if s.name not in ("Style", "StyleApplyAll")]
    run = RunState(run_id="run-1", project_id="run-1", plan=plan,
                    stage_states={s.name: StageStatus.PENDING for s in plan.stages}, status=RunStatus.RUNNING)

    call_log = []
    curate_call_count = {"n": 0}
    fixed_responses = {
        "new": '{"project": "run-1", "image_count": 2}',
        "eval": '{"submitted": 2, "evaluated": [], "failed": []}',
        "dedup": '{"groups": 2, "tagged": 0, "skipped_no_capture_time": 0}',
        "tag": '{}',
        "export-images": '{"exported": 1, "skipped": [], "created_dir": true}',
    }

    def fake_runner(argv):
        subcommand = argv[1]
        call_log.append(subcommand)
        if subcommand == "curate":
            curate_call_count["n"] += 1
            if curate_call_count["n"] == 1:
                stdout = '{"requested": 1, "returned": 1, "selected": ["a.jpg"]}'
            else:
                stdout = '{"requested": 2, "returned": 2, "selected": ["a.jpg", "b.jpg"]}'
            return subprocess.CompletedProcess(argv, 0, stdout=stdout, stderr="")
        return subprocess.CompletedProcess(argv, 0, stdout=fixed_responses[subcommand], stderr="")

    client = PztClient(pzt_bin="/fake/pzt", runner=fake_runner)
    transport = FakeTransport(in_dir=tmp_path / "in", out_dir=tmp_path / "out")
    stages = _make_pipeline(tmp_path, client, transport)
    driver = Driver(stages=stages, store=RunStore(tmp_path / "runs"))

    while run.status == RunStatus.RUNNING:
        driver.advance(run)
    assert run.status == RunStatus.AWAITING_REVIEW
    assert len(transport.sent_files) == 1
    assert transport.sent_files[0].endswith("a.jpg")

    delta = _fake_parse_adjustment("换掉第1张", run)
    driver.apply_adjustment(run, delta)
    while run.status == RunStatus.RUNNING:
        driver.advance(run)
    driver.approve(run)

    assert run.status == RunStatus.DONE
    assert call_log.count("new") == 1
    assert call_log.count("eval") == 1
    assert call_log.count("dedup") == 1
    assert call_log.count("curate") == 2
    assert call_log.count("export-images") == 2
    assert len(transport.sent_files) == 2
    assert transport.sent_files[1].endswith("b.jpg")
