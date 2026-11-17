"""Karaoke clocks use an original short cue fixture, without a music service."""
import unittest

from karaoke import Karaoke


URI = 'spotify:track:' + 'a' * 22
OTHER = 'spotify:track:' + 'b' * 22


def sheet(source='spotify'):
    return {'title': 'Roboti pjevaju', 'source': source, 'trackUri': URI, 'durationMs': 9000,
            'lines': [{'syllables': [
                {'text': 'Ro', 'startMs': 1000, 'endMs': 1400},
                {'text': 'bo', 'startMs': 1400, 'endMs': 1800},
                {'text': 'ti ', 'startMs': 1800, 'endMs': 2200},
                {'text': 'pje', 'startMs': 2400, 'endMs': 2800},
                {'text': 'va', 'startMs': 2800, 'endMs': 3200},
                {'text': 'ju.', 'startMs': 3200, 'endMs': 3800}]}]}


class KaraokeTests(unittest.TestCase):
    def test_spotify_waits_for_selected_track_then_follows_pause_seek_and_skip(self):
        karaoke = Karaoke()
        karaoke.control(sheet(), {}, 100000)
        self.assertEqual(karaoke.snapshot({'uri': OTHER}, True, 100000)['status'], 'waiting')
        playing = {'uri': URI, 'playing': True, 'progressMs': 1500, 'observedAt': 100000}
        self.assertEqual(karaoke.snapshot(playing, True, 100250)['positionMs'], 1750)
        paused = {**playing, 'playing': False}
        self.assertEqual(karaoke.snapshot(paused, True, 105000)['positionMs'], 1500)
        sought = {**playing, 'progressMs': 400, 'observedAt': 105000}
        self.assertEqual(karaoke.snapshot(sought, True, 105100)['positionMs'], 500)
        self.assertEqual(karaoke.snapshot({**playing, 'uri': OTHER}, True, 105000)['status'], 'waiting')
        self.assertEqual(karaoke.snapshot(playing, False, 105000)['status'], 'waiting')

    def test_current_queue_uid_survives_catalogue_relink_but_not_a_new_occurrence(self):
        karaoke = Karaoke()
        current = {'uri': URI, 'uid': 'same-occurrence', 'playing': True, 'progressMs': 1800}
        karaoke.control(sheet(), current, 1000)
        relinked = {**current, 'uri': OTHER}
        self.assertEqual(karaoke.snapshot(relinked, True, 1000)['status'], 'playing')
        self.assertEqual(karaoke.snapshot({**relinked, 'uid': 'new-occurrence'}, True, 1000)['status'], 'waiting')

    def test_requested_uri_matches_a_sheet_loaded_before_its_relinked_song_starts(self):
        karaoke = Karaoke()
        karaoke.control(sheet(), {}, 0)
        current = {'uri': OTHER, 'requestedUri': URI, 'playing': True, 'progressMs': 2000}
        self.assertEqual(karaoke.snapshot(current, True, 0)['status'], 'playing')

    def test_manual_clock_pause_resume_seek_offset_and_persistence(self):
        karaoke = Karaoke()
        karaoke.control(sheet('manual'), {}, 1000)
        self.assertEqual(karaoke.snapshot({}, False, 2000)['status'], 'paused')
        karaoke.control({'command': 'start'}, {}, 2000)
        self.assertEqual(karaoke.snapshot({}, False, 3000)['positionMs'], 1000)
        karaoke.control({'command': 'pause'}, {}, 3500)
        self.assertEqual(karaoke.snapshot({}, False, 9000)['positionMs'], 1500)
        karaoke = Karaoke(karaoke.saved())
        karaoke.control({'command': 'resume'}, {}, 10000)
        karaoke.control({'command': 'offset', 'offsetMs': 200}, {}, 10000)
        self.assertEqual(karaoke.snapshot({}, False, 10500)['positionMs'], 2200)
        karaoke.control({'command': 'seek', 'positionMs': 500}, {}, 11000)
        self.assertEqual(karaoke.snapshot({}, False, 11100)['positionMs'], 800)
        self.assertEqual(karaoke.snapshot({}, False, 50000)['status'], 'finished')
        karaoke.control({'command': 'stop'}, {}, 50000)
        self.assertIsNone(karaoke.snapshot({}, False, 50000))

    def test_invalid_new_sheet_and_controls_leave_the_old_sheet_intact(self):
        karaoke = Karaoke()
        karaoke.control(sheet(), {}, 0)
        original = karaoke.saved()
        for value in (-1, float('nan'), True, '100'):
            bad = sheet()
            bad['lines'][0]['syllables'][0]['startMs'] = value
            with self.assertRaises(ValueError):
                karaoke.control(bad, {}, 0)
            self.assertEqual(karaoke.saved(), original)
        bad = sheet()
        bad['lines'][0]['syllables'][1]['startMs'] = 1100
        with self.assertRaises(ValueError):
            karaoke.control(bad, {}, 0)
        with self.assertRaises(ValueError):
            karaoke.control({'command': 'seek', 'positionMs': 500}, {}, 0)
        self.assertEqual(karaoke.saved(), original)

    def test_negative_offset_preserves_the_future_start_anchor(self):
        karaoke = Karaoke()
        karaoke.control({**sheet('manual'), 'offsetMs': -5000, 'autoplay': True}, {}, 0)
        state = karaoke.snapshot({}, False, 1000)
        self.assertEqual(state['positionMs'], -4000)
        interpolated = max(0, state['positionMs'] + 2000 - state['sampledAt'])
        self.assertEqual(interpolated, 0)
        self.assertEqual(max(0, karaoke.snapshot({}, False, 2000)['positionMs']), interpolated)
        self.assertEqual(karaoke.snapshot({}, False, 5500)['positionMs'], 500)

    def test_negative_manual_offset_can_reach_the_last_cue_and_finish(self):
        karaoke = Karaoke()
        karaoke.control({**sheet('manual'), 'offsetMs': -2000, 'autoplay': True}, {}, 0)
        self.assertEqual(karaoke.snapshot({}, False, 10000)['positionMs'], 8000)
        state = karaoke.snapshot({}, False, 12000)
        self.assertEqual(state['positionMs'], 9000)
        self.assertEqual(state['status'], 'finished')

    def test_text_spacing_line_bounds_and_end_are_preserved(self):
        karaoke = Karaoke()
        karaoke.control(sheet(), {}, 0)
        state = karaoke.snapshot({'uri': URI, 'playing': True, 'progressMs': 10000}, True, 0)
        self.assertEqual(state['lines'][0]['text'], 'Roboti pjevaju.')
        self.assertEqual(state['lines'][0]['startMs'], 1000)
        self.assertEqual(state['lines'][0]['endMs'], 3800)
        self.assertEqual(state['positionMs'], 9000)
        self.assertEqual(state['status'], 'finished')


if __name__ == '__main__':
    unittest.main()
