import pytest

from molty_sim.align import cue_errors, match, mms_word, norm
from molty_sim.metrics import TurnFacts, foreign_script, pct, stats, summarize, turn_metrics, wer


def test_wer_ignores_case_punctuation_fillers_and_numbers():
    assert wer("I measured it, like at 2 meters from the window.", "i measured it like at two meters from the window") == 0
    assert wer("um so the thing is", "So the thing is.") == 0
    assert wer("yes please", "oh yes please") == pytest.approx(0.5)  # one insertion over two words
    assert wer("a b c d", "a x c") == pytest.approx(0.5)  # one substitution + one deletion
    assert wer("", "") is None
    assert wer("", "hello") == 1.0


def facts(**kw):
    base = dict(expect="turn_end", reason="turn_end", speech_end_s=2.0, speech_after_end_s=0.0, stop_mic_s=2.6, end_mic_s=2.7,
                mic_open_at=100.0, end_at=102.75, next_play_at=104.0, ack="chirp")
    base.update(kw)
    return TurnFacts(**base)


def test_turn_metrics_clean_turn():
    m = turn_metrics(facts())
    assert m["eos_to_turn_end_ms"] == 600
    assert m["eos_to_end_frame_ms"] == 700
    assert m["gap_ms"] == 2000  # 104.0 - (100 + 2.0)
    assert m["ack_after_eos_ms"] == 750
    assert not m["false_cutoff"] and m["ok"]


def test_turn_end_without_listen_stop_uses_end_frame():
    assert turn_metrics(facts(stop_mic_s=None))["eos_to_turn_end_ms"] == 700


def test_false_cutoff_needs_real_speech_left():
    assert turn_metrics(facts(speech_after_end_s=0.1))["false_cutoff"] is False  # a clipped tail
    m = turn_metrics(facts(speech_after_end_s=1.4, speech_end_s=4.0, stop_mic_s=2.6))
    assert m["false_cutoff"] is True and m["speech_lost_ms"] == 1400 and not m["ok"]
    assert m["eos_to_turn_end_ms"] < 0  # ended before the human did


def test_no_speech_cases():
    m = turn_metrics(facts(reason="no_speech", ack="off"))
    assert m["false_no_speech"] and "ack_after_eos_ms" not in m
    silent = facts(expect="no_speech", reason="no_speech", speech_end_s=None, end_mic_s=8.1, ack="off", next_play_at=None)
    m = turn_metrics(silent)
    assert m["ok"] and not m["false_no_speech"] and not m["missed_no_speech"] and m["no_speech_after_ms"] == 8100
    assert "gap_ms" not in m and "eos_to_turn_end_ms" not in m
    assert turn_metrics(facts(expect="no_speech", speech_end_s=None))["missed_no_speech"]


def test_stats_and_percentiles():
    assert pct([1, 2, 3, 4, 5], 50) == 3
    assert pct([10, 20], 90) == pytest.approx(19)
    assert pct([], 50) is None
    s = stats([None, 100, 300, 200])
    assert s["n"] == 3 and s["median"] == 200 and s["min"] == 100
    assert stats([None]) is None


def test_summarize_counts():
    rows = [turn_metrics(facts()), turn_metrics(facts(speech_after_end_s=1.0)), turn_metrics(facts(reason="no_speech", ack="off"))]
    rows[0]["say_underruns"] = 2
    s = summarize(rows, [10, -30, 50])
    assert s["false_cutoffs"] == 1 and s["false_no_speech"] == 1 and s["reason_ok"] == 1
    assert s["underruns"] == 2 and s["acks"] == {"chirp": 2, "thinking": 0, "off": 1}
    assert s["cue_err_ms"]["median"] == 10 and s["cue_abs_err_ms"]["max"] == 50


def test_word_normalisation_and_matching():
    assert norm("Meters,") == "meters" and norm("2") == "two" and norm("don’t") == "don't"
    assert mms_word("2") == "two" and mms_word("1990") == "" and mms_word("it's") == "it's"
    assert match(["Hi", "Molty,", "how", "are", "you?"], ["hi", "multi", "how", "are", "you"]) == [(0, 0), (2, 2), (3, 3), (4, 4)]


def test_cue_errors_signs_and_skips():
    onsets = [[0.10, 0.3], None, [0.80, 1.0]]
    e = cue_errors("one two three", [(0, 1760), (1, 8000), (2, 12000), (7, 99)], onsets)
    assert [(x["i"], x["err_ms"]) for x in e] == [(0, 10), (2, -50)]  # 0.11 - 0.10 late; 0.75 - 0.80 early


def test_perceived_silence_counts_molty_filler():
    # a slow agent (answer 6 s after the human stopped) with Molty's own filler at 2.6 s
    m = turn_metrics(facts(next_play_at=108.0, first_voice_at=104.6))
    assert m["gap_ms"] == 6000  # the answer itself
    assert m["silence_ms"] == 2600  # what the human hears as silence
    # no filler: the silence is the gap
    m = turn_metrics(facts(next_play_at=108.0))
    assert m["silence_ms"] == m["gap_ms"] == 6000
    # a "first voice" after the answer is not earlier than the answer
    assert turn_metrics(facts(next_play_at=104.0, first_voice_at=105.0))["silence_ms"] == 2000
    assert "silence_ms" not in turn_metrics(facts(speech_end_s=None, next_play_at=None))


def test_summary_counts_fillers():
    s = summarize([{"silence_ms": 2600, "gap_ms": 6000, "filler": "Hmm…"}, {"silence_ms": 900, "gap_ms": 900}], [])
    assert s["fillers"] == 1
    assert s["silence_ms"]["median"] == 1750.0 and s["gap_ms"]["median"] == 3450.0


class _Say:
    def __init__(self, text, play_start):
        self.text, self.play_start = text, play_start


def test_first_voice_and_agent_say_pick_the_filler_apart():
    from molty_sim.run import agent_say, first_voice

    filler, answer, old = _Say("Hmm…", 104.6), _Say("Sunny all day.", 108.0), _Say("Earlier.", 99.0)
    assert first_voice([old, filler, answer], after=102.0, until=108.0) is filler
    assert first_voice([old, answer], after=102.0, until=108.0) is answer
    assert first_voice([old], after=102.0, until=108.0) is None

    class Dev:
        says = {"a": old, "b": filler, "c": answer}

    assert agent_say(Dev, {"a"}, "Sunny all day.") is answer  # not the filler that started meanwhile
    assert agent_say(Dev, {"a", "b", "c"}, "x") is None


def test_foreign_script_flags_english_spelled_in_another_script():
    # live turns of 2026-10-31: "can you do the one where you're eating" heard as Persian / Chinese
    assert foreign_script("کن یه دود اون و یه ایتنگ")
    assert foreign_script("你要 跟 一 度 的 忘 地 越 一定")
    assert foreign_script("Nhà cán nhiêu đủ được quán đã giờ ý tính.")  # Vietnamese diacritics
    assert not foreign_script("Can you do the one where you are eating?")
    assert not foreign_script("Ça va, déjà vu — 2 meters, 100%!")
    assert not foreign_script("")
    s = summarize([{"foreign_script": True}, {"foreign_script": False}, {}], [])
    assert s["foreign_script"] == 1
