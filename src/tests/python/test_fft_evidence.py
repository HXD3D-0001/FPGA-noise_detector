"""Reject corrupted evidence rather than turning plausible numbers into PASS."""
import unittest
from check_fft_pipeline import validate_timing

ROWS=['0 4101 4105 8203 56363 4099 14505',
      '1 58417 58421 62519 110677 4099 14505',
      '2 112731 112735 116833 164991 4099 14505',
      '3 224089 224093 228191 276353 4099 14505']

class EvidenceTests(unittest.TestCase):
    def test_actual_timing_trace(self):
        timing=validate_timing(ROWS)
        self.assertEqual([x['seq'] for x in timing],[0,1,2,4])
        self.assertTrue(all(x['bank_release_cycles']==4102 for x in timing))

    def test_review_reproducer_rejected(self):
        with self.assertRaises(AssertionError):
            validate_timing(['0 100 999999 200 0 50 100']*4)

    def test_corrupted_timing_fields_rejected(self):
        for field,value in ((0,1),(1,-1),(1,5000),(2,9000),(2,4112),
                            (3,9000),(4,4000),(5,4098),(6,4098),(6,9999999)):
            with self.subTest(field=field,value=value):
                changed=[x.split() for x in ROWS];changed[0][field]=str(value)
                with self.assertRaises(AssertionError):validate_timing([' '.join(x) for x in changed])

    def test_missing_extra_duplicate_reordered_records(self):
        for rows in (ROWS[:3],ROWS+ROWS[:1],[ROWS[0],ROWS[0],*ROWS[2:]],
                     [ROWS[1],ROWS[0],*ROWS[2:]],['0 4101 4105 8203 56363 4099']+ROWS[1:]):
            with self.subTest(rows=rows):
                with self.assertRaises(AssertionError):validate_timing(rows)

    def test_ingest_deadline_is_strict(self):
        rows=list(ROWS);rows[0]='0 0 4 49993 56363 49990 50000'
        with self.assertRaises(AssertionError):validate_timing(rows)

if __name__=='__main__':unittest.main()
