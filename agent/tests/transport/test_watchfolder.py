from pathlib import Path

from transport.watchfolder import WatchFolderTransport


def test_receive_lists_only_image_files_in_in_dir(tmp_path):
    in_dir = tmp_path / "in"
    in_dir.mkdir()
    (in_dir / "a.jpg").write_bytes(b"x")
    (in_dir / "b.jpeg").write_bytes(b"x")
    (in_dir / "notes.txt").write_text("hi")
    transport = WatchFolderTransport(in_dir=in_dir, out_dir=tmp_path / "out")

    messages = list(transport.receive())

    assert {m.file_path for m in messages} == {str(in_dir / "a.jpg"), str(in_dir / "b.jpeg")}
    assert all(m.kind == "file" and m.chat_id == WatchFolderTransport.CHAT_ID for m in messages)


def test_receive_is_empty_when_in_dir_has_no_images(tmp_path):
    in_dir = tmp_path / "in"
    in_dir.mkdir()
    transport = WatchFolderTransport(in_dir=in_dir, out_dir=tmp_path / "out")

    assert list(transport.receive()) == []


def test_send_file_copies_into_out_dir(tmp_path):
    src = tmp_path / "keeper.jpg"
    src.write_bytes(b"photo-bytes")
    transport = WatchFolderTransport(in_dir=tmp_path / "in", out_dir=tmp_path / "out")

    transport.send_file("chat", str(src))

    assert (tmp_path / "out" / "keeper.jpg").read_bytes() == b"photo-bytes"


def test_send_photo_also_copies_into_out_dir(tmp_path):
    src = tmp_path / "preview.jpg"
    src.write_bytes(b"preview-bytes")
    transport = WatchFolderTransport(in_dir=tmp_path / "in", out_dir=tmp_path / "out")

    transport.send_photo("chat", str(src))

    assert (tmp_path / "out" / "preview.jpg").read_bytes() == b"preview-bytes"


def test_send_text_appends_to_messages_log(tmp_path):
    transport = WatchFolderTransport(in_dir=tmp_path / "in", out_dir=tmp_path / "out")

    transport.send_text("chat", "选好了 9 张")
    transport.send_text("chat", "第二条消息")

    log = (tmp_path / "out" / "messages.log").read_text()
    assert "选好了 9 张" in log
    assert "第二条消息" in log
