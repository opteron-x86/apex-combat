"""Check the native data contract of the packaged companion plugin."""
import hashlib
from pathlib import Path
import struct
import unittest


PLUGIN = Path(__file__).resolve().parents[1] / 'package' / 'ApexCombat.esp'


def records(data, start=0, end=None):
    """Read the uncompressed major records and nested groups in the companion."""
    end = len(data) if end is None else end
    while start < end:
        if start + 24 > end:
            raise ValueError('Truncated record header')
        tag, size, flags, form_id = struct.unpack_from('<4sIII', data, start)
        if tag == b'GRUP':
            if size < 24 or start + size > end:
                raise ValueError('Invalid group size')
            yield from records(data, start + 24, start + size)
            start += size
        else:
            stop = start + 24 + size
            if stop > end or flags & 0x40000:
                raise ValueError('Truncated or unsupported compressed record')
            yield tag, form_id, flags, data[start:stop]
            start = stop


def subrecords(record):
    position = 24
    while position < len(record):
        if position + 6 > len(record):
            raise ValueError('Truncated subrecord header')
        tag, size = struct.unpack_from('<4sH', record, position)
        stop = position + 6 + size
        if stop > len(record) or tag == b'XXXX':
            raise ValueError('Truncated or unsupported extended subrecord')
        yield tag, record[position + 6:stop]
        position = stop


class CompanionDataTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.records = list(records(PLUGIN.read_bytes()))
        cls.by_id = {form_id & 0xFFFFFF: (tag, flags, raw)
                     for tag, form_id, flags, raw in cls.records}

    def test_plugin_identity_and_form_allocation(self):
        self.assertEqual(self.records[0][0], b'TES4')
        self.assertTrue(self.records[0][2] & 0x200, 'Companion must stay ESL flagged')
        header = list(subrecords(self.records[0][3]))
        self.assertEqual([value for tag, value in header if tag == b'MAST'], [b'Skyrim.esm\0'])
        stats = next(value for tag, value in header if tag == b'HEDR')
        self.assertGreaterEqual(struct.unpack('<fII', stats)[2], 0x803)
        self.assertEqual(len(self.by_id), len(self.records), 'Duplicate local form IDs')
        self.assertEqual({key: value[0] for key, value in self.by_id.items()},
                         {0: b'TES4', 0x800: b'SNDR', 0x801: b'SNDR', 0x802: b'PERK'})
        for tag, form_id, _, _ in self.records[1:]:
            self.assertEqual(form_id >> 24, 1, f'{tag}: expected companion-owned record')

    def test_existing_sound_descriptors_unchanged(self):
        expected = {
            0x800: '53bd122407011bb8ba3ccfd789c1ab301a09494d2d611488175b5d1162c39229',
            0x801: '7f56b40c3d95546f2483fa163642b76de572da3082a7eb392ee6b4b437059676',
        }
        for form_id, digest in expected.items():
            self.assertEqual(hashlib.sha256(self.by_id[form_id][2]).hexdigest(), digest)

    def test_hidden_single_rank_perk_and_neutral_entry(self):
        tag, flags, raw = self.by_id[0x802]
        self.assertEqual(tag, b'PERK')
        self.assertFalse(flags & 0x20, 'Perk must not be deleted')
        parts = list(subrecords(raw))
        self.assertEqual([value for tag, value in parts if tag == b'EDID'], [b'ApexPowerAttackCost\0'])
        self.assertEqual([value for tag, value in parts if tag == b'FULL'], [b'ApexPowerAttackCost\0'])
        boundary = [index for index, (tag, _) in enumerate(parts) if tag == b'PRKE']
        self.assertEqual(len(boundary), 1, 'Expected exactly one entry-point effect')
        before, entry = parts[:boundary[0]], parts[boundary[0]:]
        # Trait, level, number of ranks, playable, hidden.
        self.assertEqual([value for tag, value in before if tag == b'DATA'], [bytes((0, 0, 1, 0, 1))])
        # Entry-point effect, zero-based first rank, priority zero. The entry
        # layout matches Skyrim's ChampionsStance and FightingStance records.
        self.assertEqual(entry, [
            (b'PRKE', bytes((2, 0, 0))),
            (b'DATA', bytes((27, 3, 2))),  # ModPowerAttackStamina, MultiplyValue, two argument tabs
            (b'EPFT', b'\x01'),           # one floating-point parameter
            (b'EPFD', struct.pack('<f', 1.0)),
            (b'PRKF', b''),
        ])
        self.assertFalse(any(tag in (b'CTDA', b'PRKC', b'VMAD', b'NNAM') for tag, _ in parts),
                         'Native entry must have no conditions, script, or next perk')

    def test_malformed_record_boundaries_fail(self):
        with self.assertRaises(ValueError):
            list(records(b'PERK'))
        with self.assertRaises(ValueError):
            list(records(struct.pack('<4sIIIII', b'GRUP', 0, 0, 0, 0, 0)))
        with self.assertRaises(ValueError):
            list(subrecords(bytes(24) + b'EDID\x05\x00x'))


if __name__ == '__main__':
    unittest.main()
