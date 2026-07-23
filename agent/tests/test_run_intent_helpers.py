from orchestrator.types import Plan, StageSpec
from run_intent import _fill_transport_params


def test_fill_transport_params_sets_ingest_folder_and_deliver_out_folder():
    plan = Plan(stages=[
        StageSpec(name="Ingest"),
        StageSpec(name="Dedup"),
        StageSpec(name="Curate", params={"count": 9, "apply_tag": "精选"}),
        StageSpec(name="Deliver"),
    ])

    _fill_transport_params(plan, "/tmp/in", "/tmp/out")

    ingest_spec = next(s for s in plan.stages if s.name == "Ingest")
    deliver_spec = next(s for s in plan.stages if s.name == "Deliver")
    assert ingest_spec.params == {"folder": "/tmp/in"}
    assert deliver_spec.params == {"out_folder": "/tmp/out"}
