#!/usr/bin/env python3
"""
build_dictionary.py — Convert dictionary.csv to binary index + data files for CrossPoint Reader.

Input:  dictionary.csv  (word,wordtype,definition — multi-line quoted CSV)
Output: dictionary.idx  (fixed-width sorted index for binary search)
        dictionary.dat  (variable-length definition records)

Copy dictionary.idx and dictionary.dat to the root of your SD card.

Index file format (dictionary.idx):
  Header: magic[4] + version[1] + entry_count[4 LE] + word_field_size[1] = 10 bytes
  Each entry (40 bytes): word_lower[32, null-padded] + dat_offset[4 LE] + dat_length[4 LE]
  Entries are sorted alphabetically.

Data file format (dictionary.dat):
  Per word: def_count[1] + { wordtype_len[1] + wordtype[N] + def_len[2 LE] + def_text[N] } * def_count
"""

import csv
import os
import struct
import sys
from collections import defaultdict

MAGIC = b'DCTI'
VERSION = 1
WORD_FIELD_SIZE = 32  # bytes per word in the index (null-padded)
IDX_ENTRY_SIZE = WORD_FIELD_SIZE + 4 + 4  # word + dat_offset + dat_length = 40 bytes
IDX_HEADER_SIZE = len(MAGIC) + 1 + 4 + 1  # magic + version + entry_count + word_field_size = 10

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
DEFAULT_INPUT = os.path.join(REPO_ROOT, 'dictionary.csv')
DEFAULT_IDX = os.path.join(REPO_ROOT, 'dictionary.idx')
DEFAULT_DAT = os.path.join(REPO_ROOT, 'dictionary.dat')


def clean_definition(text: str) -> str:
    """Normalize whitespace in a definition (collapse runs of spaces/newlines)."""
    return ' '.join(text.split())


def encode_word_field(word: str) -> bytes:
    """Encode a word into a null-padded WORD_FIELD_SIZE byte field (UTF-8, truncated)."""
    encoded = word.lower().encode('utf-8', errors='replace')[:WORD_FIELD_SIZE]
    return encoded.ljust(WORD_FIELD_SIZE, b'\x00')


def parse_csv(path: str) -> dict:
    """
    Parse dictionary.csv and return a dict mapping lowercase word -> list of (wordtype, definition).
    Handles multi-line quoted CSV values correctly via csv.reader.
    """
    words = defaultdict(list)
    skipped = 0

    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        reader = csv.reader(f)
        header = next(reader, None)  # skip header row
        if header is None:
            print("ERROR: empty file", file=sys.stderr)
            sys.exit(1)

        for i, row in enumerate(reader, start=2):
            if len(row) < 1:
                continue
            if len(row) < 3:
                # Rows with fewer than 3 columns: skip silently
                skipped += 1
                continue

            raw_word = row[0].strip()
            wordtype = row[1].strip() if row[1] else ''
            definition = clean_definition(row[2]) if row[2] else ''

            if not raw_word or not definition:
                skipped += 1
                continue

            key = raw_word.lower()
            if len(key.encode('utf-8')) > WORD_FIELD_SIZE:
                # Word too long for index field — skip
                skipped += 1
                continue

            words[key].append((wordtype, definition))

    print(f"Parsed {len(words)} unique words ({skipped} rows skipped).")
    return words


def write_binary_files(words: dict, idx_path: str, dat_path: str):
    """Write dictionary.idx and dictionary.dat from the parsed word dict."""
    sorted_words = sorted(words.keys())
    entry_count = len(sorted_words)

    print(f"Writing {entry_count} entries to {os.path.basename(dat_path)} and {os.path.basename(idx_path)}...")

    with open(dat_path, 'wb') as dat_f, open(idx_path, 'wb') as idx_f:
        # Write index header
        idx_f.write(MAGIC)
        idx_f.write(struct.pack('<B', VERSION))
        idx_f.write(struct.pack('<I', entry_count))
        idx_f.write(struct.pack('<B', WORD_FIELD_SIZE))

        dat_offset = 0

        for word_key in sorted_words:
            defs = words[word_key]

            # --- Build dat record ---
            record = bytearray()
            def_count = min(len(defs), 255)
            record += struct.pack('<B', def_count)

            for wordtype, definition in defs[:def_count]:
                wt_bytes = wordtype.encode('utf-8', errors='replace')[:255]
                record += struct.pack('<B', len(wt_bytes))
                record += wt_bytes

                def_bytes = definition.encode('utf-8', errors='replace')
                # Cap individual definition at 800 bytes to protect device RAM
                if len(def_bytes) > 800:
                    def_bytes = def_bytes[:797] + b'...'
                record += struct.pack('<H', len(def_bytes))
                record += def_bytes

            dat_length = len(record)
            dat_f.write(record)

            # --- Write index entry ---
            idx_f.write(encode_word_field(word_key))
            idx_f.write(struct.pack('<I', dat_offset))
            idx_f.write(struct.pack('<I', dat_length))

            dat_offset += dat_length

    idx_size = IDX_HEADER_SIZE + entry_count * IDX_ENTRY_SIZE
    print(f"  {os.path.basename(idx_path)}: {idx_size / 1024 / 1024:.1f} MB  ({entry_count} entries)")
    print(f"  {os.path.basename(dat_path)}: {dat_offset / 1024 / 1024:.1f} MB")
    print("Done. Copy both files to the root of your SD card.")


def main():
    input_path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_INPUT
    idx_path = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_IDX
    dat_path = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_DAT

    if not os.path.exists(input_path):
        print(f"ERROR: Input file not found: {input_path}", file=sys.stderr)
        print(f"Usage: python build_dictionary.py [input.csv] [output.idx] [output.dat]", file=sys.stderr)
        sys.exit(1)

    print(f"Reading: {input_path}")
    words = parse_csv(input_path)
    write_binary_files(words, idx_path, dat_path)


if __name__ == '__main__':
    main()
