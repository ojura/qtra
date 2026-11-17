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
        current = {'uri': URI, 'uid': 'same-occurrence', 'playing': True,
                   'progressMs': 1800, 'observedAt': 1000}
        karaoke.control(sheet(), current, 1000)
        relinked = {**current, 'uri': OTHER}
        self.assertEqual(karaoke.snapshot(relinked, True, 1000)['status'], 'playing')
        self.assertEqual(karaoke.snapshot({**relinked, 'uid': 'new-occurrence'}, True, 1000)['status'], 'waiting')

    def test_requested_uri_matches_a_sheet_loaded_before_its_relinked_song_starts(self):
        karaoke = Karaoke()
        karaoke.control(sheet(), {}, 0)
        current = {'uri': OTHER, 'requestedUri': URI, 'playing': True,
                   'progressMs': 2000, 'observedAt': 0}
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
        state = karaoke.snapshot({'uri': URI, 'playing': True, 'progressMs': 10000,
                                  'observedAt': 0}, True, 0)
        self.assertEqual(state['lines'][0]['text'], 'Roboti pjevaju.')
        self.assertEqual(state['lines'][0]['startMs'], 1000)
        self.assertEqual(state['lines'][0]['endMs'], 3800)
        self.assertEqual(state['positionMs'], 9000)
        self.assertEqual(state['status'], 'finished')

    def test_first_accepted_occurrence_binds_once_and_survives_restart(self):
        karaoke = Karaoke()
        karaoke.control(sheet(), {}, 1000)
        current = {'uri': URI, 'uid': 'first', 'playing': True,
                   'progressMs': 0, 'observedAt': 1000}
        self.assertTrue(karaoke.observe(current, True, 1000))
        self.assertEqual(karaoke.saved()['trackUid'], 'first')
        self.assertFalse(karaoke.observe(current, True, 1100))
        karaoke = Karaoke(karaoke.saved())
        relinked = {**current, 'uri': OTHER, 'observedAt': 2000}
        self.assertEqual(karaoke.snapshot(relinked, True, 2100)['status'], 'playing')
        repeated = {**current, 'uid': 'second', 'observedAt': 3000}
        self.assertFalse(karaoke.observe(repeated, True, 3000))
        self.assertEqual(karaoke.snapshot(repeated, True, 3100)['status'], 'waiting')
        self.assertEqual(karaoke.saved()['trackUid'], 'first')

    def test_binding_requires_connected_matching_occurrence_and_snapshot_is_pure(self):
        karaoke = Karaoke()
        karaoke.control(sheet(), {}, 0)
        original = karaoke.saved()
        current = {'uri': URI, 'uid': 'first', 'playing': True,
                   'progressMs': 0, 'observedAt': 0}
        karaoke.snapshot(current, True, 0)
        self.assertEqual(karaoke.saved(), original)
        self.assertFalse(karaoke.observe(current, False, 0))
        self.assertFalse(karaoke.observe({**current, 'uri': OTHER}, True, 0))
        self.assertFalse(karaoke.observe({**current, 'uid': None}, True, 0))
        self.assertEqual(karaoke.saved(), original)
        self.assertTrue(karaoke.observe({**current, 'uri': OTHER, 'requestedUri': URI}, True, 0))

    def test_explicit_pause_and_buffering_hold_exact_zero_despite_playing_flag(self):
        karaoke = Karaoke()
        karaoke.control(sheet(), {}, 0)
        current = {'uri': URI, 'playing': True, 'progressMs': 0, 'observedAt': 0}
        for field, clock_state in [('paused', 'paused'), ('buffering', 'buffering')]:
            with self.subTest(field=field):
                result = karaoke.snapshot({**current, field: True}, True, 2500)
                self.assertEqual(result['status'], 'paused')
                self.assertEqual(result['clockState'], clock_state)
                self.assertEqual(result['positionMs'], 0)

    def test_explicit_zero_and_backward_forward_seeks_use_the_observed_anchor(self):
        karaoke = Karaoke()
        karaoke.control(sheet(), {}, 0)
        for position, observed, stamp, expected in [(0, 0, 250, 250),
                                                     (4000, 1000, 1100, 4100),
                                                     (0, 2000, 2100, 100),
                                                     (6500, 3000, 3100, 6600)]:
            with self.subTest(position=position, observed=observed):
                current = {'uri': URI, 'playing': True, 'progressMs': position, 'observedAt': observed}
                result = karaoke.snapshot(current, True, stamp)
                self.assertEqual(result['status'], 'playing')
                self.assertEqual(result['positionMs'], expected)
                self.assertEqual(result['clockValidUntil'], observed + 10000)

    def test_explicit_nontrue_source_clock_holds_without_caching_numeric_fallback(self):
        karaoke = Karaoke()
        current = {'uri': URI, 'uid': 'first', 'playing': True, 'clockValid': True,
                   'progressMs': 200, 'observedAt': 1000}
        karaoke.control(sheet(), current, 1000)
        for value in (False, None, 0, 1, 'true', 'false'):
            with self.subTest(clock_valid=value):
                invalid = {**current, 'clockValid': value, 'progressMs': 0, 'observedAt': 2000}
                self.assertFalse(karaoke.observe(invalid, True, 2000))
                result = karaoke.snapshot(invalid, True, 2500)
                self.assertEqual(result['status'], 'held')
                self.assertEqual(result['clockState'], 'invalid')
                self.assertEqual(result['clockValidUntil'], 0)
                self.assertEqual(result['positionMs'], 200)

    def test_true_source_clock_preserves_zero_and_missing_flag_remains_compatible(self):
        karaoke = Karaoke()
        karaoke.control(sheet(), {}, 1000)
        current = {'uri': URI, 'playing': True, 'progressMs': 0, 'observedAt': 1000}
        for extra, expected_status, expected_position in [({}, 'playing', 250),
                ({'clockValid': True}, 'playing', 250),
                ({'clockValid': True, 'paused': True}, 'paused', 0),
                ({'clockValid': True, 'buffering': True}, 'paused', 0)]:
            with self.subTest(extra=extra):
                result = karaoke.snapshot({**current, **extra}, True, 1250)
                self.assertEqual(result['status'], expected_status)
                self.assertEqual(result['positionMs'], expected_position)
                self.assertEqual(result['clockValidUntil'], 11000)

    def test_missing_or_invalid_progress_holds_last_measured_position(self):
        karaoke = Karaoke()
        current = {'uri': URI, 'uid': 'first', 'playing': True,
                   'progressMs': 200, 'observedAt': 1000}
        karaoke.control(sheet(), current, 1000)
        karaoke.observe(current, True, 1000)
        for value in (None, True, False, -1, float('nan'), float('inf'), '0'):
            with self.subTest(value=value):
                invalid = {**current, 'observedAt': 2000}
                if value is None:
                    invalid.pop('progressMs')
                else:
                    invalid['progressMs'] = value
                self.assertFalse(karaoke.observe(invalid, True, 2000))
                result = karaoke.snapshot(invalid, True, 2500)
                self.assertEqual(result['status'], 'held')
                self.assertEqual(result['clockState'], 'invalid')
                self.assertEqual(result['clockValidUntil'], 0)
                self.assertEqual(result['positionMs'], 200)

    def test_missing_invalid_or_future_observation_time_holds_reported_position(self):
        karaoke = Karaoke()
        karaoke.control(sheet(), {}, 0)
        for value in (None, True, False, -1, float('nan'), float('inf'), '1000', 5000):
            with self.subTest(value=value):
                current = {'uri': URI, 'playing': True, 'progressMs': 600}
                if value is not None:
                    current['observedAt'] = value
                result = karaoke.snapshot(current, True, 2500)
                self.assertEqual(result['status'], 'held')
                self.assertEqual(result['clockState'], 'invalid')
                self.assertEqual(result['clockValidUntil'], 0)
                self.assertEqual(result['positionMs'], 600)

    def test_freshness_deadline_comes_from_observation_and_does_not_slide(self):
        karaoke = Karaoke()
        karaoke.control({**sheet(), 'durationMs': 40000}, {}, 100000)
        current = {'uri': URI, 'playing': True, 'progressMs': 500, 'observedAt': 100000}
        for stamp in (100250, 104000, 109999):
            with self.subTest(stamp=stamp):
                result = karaoke.snapshot(current, True, stamp)
                self.assertEqual(result['status'], 'playing')
                self.assertEqual(result['clockValidUntil'], 110000)
                self.assertEqual(result['positionMs'], 500 + stamp - 100000)
        for stamp in (110000, 140000):
            with self.subTest(stamp=stamp):
                result = karaoke.snapshot(current, True, stamp)
                self.assertEqual(result['status'], 'held')
                self.assertEqual(result['clockState'], 'stale')
                self.assertEqual(result['positionMs'], 10500)
                self.assertEqual(result['clockValidUntil'], 110000)

    def test_stale_paused_or_buffering_observation_never_advances(self):
        karaoke = Karaoke()
        karaoke.control(sheet(), {}, 0)
        for field in ('paused', 'buffering'):
            with self.subTest(field=field):
                result = karaoke.snapshot({'uri': URI, 'playing': True, field: True,
                                           'progressMs': 0, 'observedAt': 0}, True, 20000)
                self.assertEqual(result['status'], 'held')
                self.assertEqual(result['clockState'], 'stale')
                self.assertEqual(result['positionMs'], 0)

    def test_waiting_and_manual_clocks_keep_separate_freshness_semantics(self):
        karaoke = Karaoke()
        karaoke.control(sheet(), {}, 0)
        current = {'uri': URI, 'playing': True, 'progressMs': 0, 'observedAt': 0}
        result = karaoke.snapshot(current, False, 1000)
        self.assertEqual(result['status'], 'waiting')
        self.assertEqual(result['clockState'], 'disconnected')
        self.assertEqual(result['clockValidUntil'], 0)
        karaoke.control({**sheet('manual'), 'durationMs': 40000, 'autoplay': True}, {}, 0)
        result = karaoke.snapshot({}, False, 15000)
        self.assertEqual(result['status'], 'playing')
        self.assertEqual(result['positionMs'], 15000)
        self.assertNotIn('clockValidUntil', result)
        self.assertFalse(karaoke.observe(current, True, 15000))


if __name__ == '__main__':
    unittest.main()
