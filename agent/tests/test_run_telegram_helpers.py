from pzt_client import PztClient
from run_telegram import build_router


class FakeTransport:
    def receive(self):
        return []

    def send_text(self, chat_id, text):
        pass

    def send_photo(self, chat_id, path):
        pass

    def send_file(self, chat_id, path):
        pass


def test_build_router_wires_stages_store_and_deliver_chat_id(tmp_path):
    client = PztClient(pzt_bin="/fake/pzt")
    transport = FakeTransport()

    router = build_router(state_dir=tmp_path, client=client, transport=transport, chat_id="42")

    assert router.chat_id == "42"
    assert router.incoming_root == tmp_path / "incoming"
    assert router.preview_root == tmp_path / "preview"
    assert router.deliver_out_folder == tmp_path / "deliver-out"
    assert router.driver.stages["Deliver"].chat_id == "42"
    assert (tmp_path / "incoming").is_dir()
    assert (tmp_path / "preview").is_dir()
    assert (tmp_path / "deliver-out").is_dir()


def test_build_router_uses_the_real_compose_plan_and_classify_gate_reply_functions(tmp_path):
    from compose.adjustment_parser import classify_gate_reply
    from compose.plan_composer import compose_plan

    client = PztClient(pzt_bin="/fake/pzt")
    transport = FakeTransport()

    router = build_router(state_dir=tmp_path, client=client, transport=transport, chat_id="42")

    assert router.compose_plan_fn is compose_plan
    assert router.classify_gate_reply_fn is classify_gate_reply


def test_build_router_defaults_and_passes_through_idle_reminder_seconds(tmp_path):
    client = PztClient(pzt_bin="/fake/pzt")
    transport = FakeTransport()

    default_router = build_router(state_dir=tmp_path, client=client, transport=transport, chat_id="42")
    assert default_router.idle_reminder_seconds == 300.0

    custom_router = build_router(state_dir=tmp_path, client=client, transport=transport, chat_id="42",
                                  idle_reminder_seconds=20.0)
    assert custom_router.idle_reminder_seconds == 20.0


def test_build_router_uses_the_real_refine_plan_confirmation_function(tmp_path):
    from compose.adjustment_parser import refine_plan_confirmation

    client = PztClient(pzt_bin="/fake/pzt")
    transport = FakeTransport()

    router = build_router(state_dir=tmp_path, client=client, transport=transport, chat_id="42")

    assert router.refine_plan_confirmation_fn is refine_plan_confirmation
