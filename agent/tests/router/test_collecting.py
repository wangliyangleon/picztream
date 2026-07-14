import re

from orchestrator.types import RunStatus
from router.collecting import (
    incoming_dir_for,
    new_collecting_run,
    new_run_id,
    stage_incoming_photo,
)
from store.run_store import RunStore


def test_new_collecting_run_round_trips_through_run_store(tmp_path):
    store = RunStore(tmp_path)
    run = new_collecting_run("tg-abcd1234")

    store.save(run)
    loaded = store.load("tg-abcd1234")

    assert loaded.run_id == "tg-abcd1234"
    assert loaded.project_id == "tg-abcd1234"
    assert loaded.status == RunStatus.COLLECTING
    assert loaded.plan.stages == []
    assert loaded.stage_states == {}


def test_new_collecting_run_shows_up_in_list_active(tmp_path):
    store = RunStore(tmp_path)
    run = new_collecting_run("tg-abcd1234")
    store.save(run)

    active_ids = {r.run_id for r in store.list_active()}

    assert "tg-abcd1234" in active_ids


def test_new_run_id_produces_distinct_ids_matching_expected_shape():
    id1 = new_run_id()
    id2 = new_run_id()

    assert id1 != id2
    assert re.fullmatch(r"tg-[0-9a-f]{8}", id1)
    assert re.fullmatch(r"tg-[0-9a-f]{8}", id2)


def test_incoming_dir_for_creates_directory(tmp_path):
    incoming_root = tmp_path / "incoming"

    result = incoming_dir_for(incoming_root, "tg-abcd1234")

    assert result == incoming_root / "tg-abcd1234"
    assert result.is_dir()


def test_incoming_dir_for_is_idempotent_on_second_call(tmp_path):
    incoming_root = tmp_path / "incoming"
    first = incoming_dir_for(incoming_root, "tg-abcd1234")

    second = incoming_dir_for(incoming_root, "tg-abcd1234")

    assert first == second
    assert second.is_dir()


def test_stage_incoming_photo_copies_bytes_into_run_subdir(tmp_path):
    incoming_root = tmp_path / "incoming"
    src = tmp_path / "source" / "photo.jpg"
    src.parent.mkdir()
    src.write_bytes(b"real-photo-bytes")

    dest = stage_incoming_photo(incoming_root, "tg-abcd1234", str(src))

    assert dest == incoming_root / "tg-abcd1234" / "photo.jpg"
    assert dest.read_bytes() == b"real-photo-bytes"
