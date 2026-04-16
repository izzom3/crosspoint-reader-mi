#include "DictionaryStore.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstring>

namespace DictionaryStore {

// --- Index constants (must match build_dictionary.py) ---
static constexpr uint8_t IDX_VERSION = 1;
static constexpr size_t WORD_FIELD = 32;
static constexpr size_t IDX_ENTRY_SIZE = WORD_FIELD + 4 + 4;  // word + dat_offset + dat_length
static constexpr size_t IDX_HEADER_SIZE = 4 + 1 + 4 + 1;     // magic + version + count + field_size

// Strip leading/trailing ASCII punctuation and lowercase the result.
// e.g. "word," -> "word", '"hello"' -> "hello"
static std::string normalizeWord(const std::string& raw) {
  size_t start = 0;
  size_t end = raw.size();
  while (start < end && !std::isalpha(static_cast<unsigned char>(raw[start])) &&
         !std::isdigit(static_cast<unsigned char>(raw[start]))) {
    ++start;
  }
  while (end > start && !std::isalpha(static_cast<unsigned char>(raw[end - 1])) &&
         !std::isdigit(static_cast<unsigned char>(raw[end - 1]))) {
    --end;
  }
  std::string result = raw.substr(start, end - start);
  for (char& c : result) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return result;
}

bool isAvailable() { return Storage.exists(IDX_PATH); }

// Binary search the open idx file for 'key' (lowercase, no punctuation, <= WORD_FIELD chars).
// Returns true and fills datOffset/datLength if found.
static bool binarySearch(FsFile& idxFile, uint32_t entryCount, const char* key,
                          uint32_t& datOffset, uint32_t& datLength) {
  char wordBuf[WORD_FIELD + 1] = {};
  int32_t lo = 0;
  int32_t hi = static_cast<int32_t>(entryCount) - 1;

  while (lo <= hi) {
    const int32_t mid = lo + (hi - lo) / 2;
    const uint32_t seekPos = static_cast<uint32_t>(IDX_HEADER_SIZE) + static_cast<uint32_t>(mid) * IDX_ENTRY_SIZE;

    idxFile.seek(seekPos);
    if (idxFile.read(wordBuf, WORD_FIELD) != static_cast<int>(WORD_FIELD)) {
      break;
    }
    wordBuf[WORD_FIELD] = '\0';

    const int cmp = strncmp(key, wordBuf, WORD_FIELD);
    if (cmp == 0) {
      uint8_t offBuf[8];
      if (idxFile.read(offBuf, 8) == 8) {
        datOffset = static_cast<uint32_t>(offBuf[0]) | (static_cast<uint32_t>(offBuf[1]) << 8) |
                    (static_cast<uint32_t>(offBuf[2]) << 16) | (static_cast<uint32_t>(offBuf[3]) << 24);
        datLength = static_cast<uint32_t>(offBuf[4]) | (static_cast<uint32_t>(offBuf[5]) << 8) |
                    (static_cast<uint32_t>(offBuf[6]) << 16) | (static_cast<uint32_t>(offBuf[7]) << 24);
        return true;
      }
      break;
    } else if (cmp < 0) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  }
  return false;
}

bool lookup(const std::string& word, char* outBuf, size_t outBufSize,
            char* outFoundWord, size_t outFoundWordSize) {
  if (outBufSize == 0) return false;
  const std::string key = normalizeWord(word);
  if (key.empty()) return false;

  // --- Open index file and read header ---
  FsFile idxFile;
  if (!Storage.openFileForRead("DICT", IDX_PATH, idxFile)) {
    LOG_ERR("DICT", "dictionary.idx not found");
    return false;
  }

  uint8_t header[IDX_HEADER_SIZE];
  if (idxFile.read(header, IDX_HEADER_SIZE) != static_cast<int>(IDX_HEADER_SIZE)) {
    LOG_ERR("DICT", "Failed to read idx header");
    idxFile.close();
    return false;
  }

  if (header[0] != 'D' || header[1] != 'C' || header[2] != 'T' || header[3] != 'I') {
    LOG_ERR("DICT", "Bad magic in dictionary.idx");
    idxFile.close();
    return false;
  }
  if (header[4] != IDX_VERSION) {
    LOG_ERR("DICT", "Unsupported idx version %d", header[4]);
    idxFile.close();
    return false;
  }

  const uint32_t entryCount = static_cast<uint32_t>(header[5]) | (static_cast<uint32_t>(header[6]) << 8) |
                               (static_cast<uint32_t>(header[7]) << 16) | (static_cast<uint32_t>(header[8]) << 24);

  if (entryCount == 0) {
    idxFile.close();
    return false;
  }

  // --- Binary search: try exact key first, then suffix-stripped stems ---
  uint32_t datOffset = 0;
  uint32_t datLength = 0;
  bool found = binarySearch(idxFile, entryCount, key.c_str(), datOffset, datLength);

  if (!found) {
    // Suffix-stripping stemmer: try common English inflection endings.
    // Candidates are tried in order; we stop at the first match.
    // All stems are written into a small stack buffer to avoid heap allocation.
    const size_t klen = key.size();
    char stem[WORD_FIELD + 1];

    // Macro: try a stem built with snprintf, stop if found.
    // Inline the binarySearch call directly to keep control flow clear.
#define TRY_STEM(fmt, ...)                                                       \
  if (!found) {                                                                  \
    snprintf(stem, sizeof(stem), fmt, ##__VA_ARGS__);                           \
    if (stem[0] != '\0') found = binarySearch(idxFile, entryCount, stem, datOffset, datLength); \
  }

    // -ing  (running→run, moving→move, walking→walk)
    if (!found && klen > 4 && strncmp(key.c_str() + klen - 3, "ing", 3) == 0) {
      const size_t s = klen - 3;
      // Doubled-consonant: "runn" → "run"
      if (s >= 2 && key[s - 1] == key[s - 2]) {
        TRY_STEM("%.*s", static_cast<int>(s - 1), key.c_str());
      }
      TRY_STEM("%.*se", static_cast<int>(s), key.c_str());  // moving → move
      TRY_STEM("%.*s", static_cast<int>(s), key.c_str());   // walking → walk
    }

    // -ed  (stepped→step, loved→love, walked→walk)
    if (!found && klen > 3 && strncmp(key.c_str() + klen - 2, "ed", 2) == 0) {
      const size_t s = klen - 2;
      // Doubled-consonant: "stepp" → "step"
      if (s >= 2 && key[s - 1] == key[s - 2]) {
        TRY_STEM("%.*s", static_cast<int>(s - 1), key.c_str());
      }
      TRY_STEM("%.*se", static_cast<int>(s), key.c_str());  // loved → love
      TRY_STEM("%.*s", static_cast<int>(s), key.c_str());   // walked → walk
    }

    // -ies → -y  (cities→city, bodies→body)
    if (!found && klen > 3 && strncmp(key.c_str() + klen - 3, "ies", 3) == 0) {
      TRY_STEM("%.*sy", static_cast<int>(klen - 3), key.c_str());
    }

    // -es  (boxes→box, classes→class)
    if (!found && klen > 3 && strncmp(key.c_str() + klen - 2, "es", 2) == 0) {
      TRY_STEM("%.*s", static_cast<int>(klen - 2), key.c_str());
    }

    // -s  (cats→cat)
    if (!found && klen > 2 && key[klen - 1] == 's') {
      TRY_STEM("%.*s", static_cast<int>(klen - 1), key.c_str());
    }

    // -ly  (quickly→quick, happily→happy)
    if (!found && klen > 4 && strncmp(key.c_str() + klen - 2, "ly", 2) == 0) {
      // -ily → -y  (happily→happy)
      if (klen > 5 && key[klen - 3] == 'i') {
        TRY_STEM("%.*sy", static_cast<int>(klen - 3), key.c_str());
      }
      TRY_STEM("%.*s", static_cast<int>(klen - 2), key.c_str());
    }

    // -er  (bigger→big, nicer→nice, runner→run)
    if (!found && klen > 4 && strncmp(key.c_str() + klen - 2, "er", 2) == 0) {
      const size_t s = klen - 2;
      if (s >= 2 && key[s - 1] == key[s - 2]) {
        TRY_STEM("%.*s", static_cast<int>(s - 1), key.c_str());
      }
      TRY_STEM("%.*se", static_cast<int>(s), key.c_str());
      TRY_STEM("%.*s", static_cast<int>(s), key.c_str());
    }

    // -est  (biggest→big, nicest→nice)
    if (!found && klen > 5 && strncmp(key.c_str() + klen - 3, "est", 3) == 0) {
      const size_t s = klen - 3;
      if (s >= 2 && key[s - 1] == key[s - 2]) {
        TRY_STEM("%.*s", static_cast<int>(s - 1), key.c_str());
      }
      TRY_STEM("%.*se", static_cast<int>(s), key.c_str());
      TRY_STEM("%.*s", static_cast<int>(s), key.c_str());
    }

    // -ness  (darkness→dark, happiness→happy via -iness)
    if (!found && klen > 5 && strncmp(key.c_str() + klen - 4, "ness", 4) == 0) {
      if (klen > 6 && key[klen - 5] == 'i') {
        TRY_STEM("%.*sy", static_cast<int>(klen - 5), key.c_str());  // happiness→happy
      }
      TRY_STEM("%.*s", static_cast<int>(klen - 4), key.c_str());
    }

    // -ful / -less  (helpful→help, useless→use)
    if (!found && klen > 5 && strncmp(key.c_str() + klen - 3, "ful", 3) == 0) {
      TRY_STEM("%.*s", static_cast<int>(klen - 3), key.c_str());
    }
    if (!found && klen > 5 && strncmp(key.c_str() + klen - 4, "less", 4) == 0) {
      TRY_STEM("%.*s", static_cast<int>(klen - 4), key.c_str());
    }

#undef TRY_STEM

    if (found) {
      LOG_DBG("DICT", "Found '%s' via stem '%s'", key.c_str(), stem);
      // Report the matched stem so callers can display it instead of the original
      if (outFoundWord != nullptr && outFoundWordSize > 0) {
        strncpy(outFoundWord, stem, outFoundWordSize - 1);
        outFoundWord[outFoundWordSize - 1] = '\0';
      }
    }
  }

  // For an exact match, report the normalised key as the found word
  if (found && outFoundWord != nullptr && outFoundWordSize > 0 && outFoundWord[0] == '\0') {
    strncpy(outFoundWord, key.c_str(), outFoundWordSize - 1);
    outFoundWord[outFoundWordSize - 1] = '\0';
  }

  idxFile.close();

  if (!found) {
    LOG_DBG("DICT", "Word not found: %s", key.c_str());
    return false;
  }

  // --- Read definition record from dat file ---
  if (datLength == 0) return false;

  // Cap read size to protect RAM (dat record should already be capped by build script)
  const size_t readSize = (datLength < 2048) ? datLength : 2048;
  auto* datBuf = static_cast<uint8_t*>(malloc(readSize));
  if (!datBuf) {
    LOG_ERR("DICT", "malloc failed for dat read (%u bytes)", static_cast<unsigned>(readSize));
    return false;
  }

  FsFile datFile;
  bool readOk = false;
  if (Storage.openFileForRead("DICT", DAT_PATH, datFile)) {
    datFile.seek(datOffset);
    readOk = (datFile.read(datBuf, readSize) == static_cast<int>(readSize));
    datFile.close();
  }

  if (!readOk) {
    LOG_ERR("DICT", "Failed to read dat record for '%s'", key.c_str());
    free(datBuf);
    return false;
  }

  // --- Parse and format definitions into outBuf ---
  size_t pos = 0;
  size_t outPos = 0;

  auto appendStr = [&](const char* s, size_t len) {
    const size_t space = outBufSize - 1 - outPos;
    const size_t toCopy = (len < space) ? len : space;
    memcpy(outBuf + outPos, s, toCopy);
    outPos += toCopy;
  };

  if (pos >= readSize) {
    free(datBuf);
    return false;
  }

  const uint8_t defCount = datBuf[pos++];

  for (uint8_t i = 0; i < defCount && outPos < outBufSize - 1; ++i) {
    if (pos >= readSize) break;

    // Numbering for multiple definitions
    if (defCount > 1) {
      char numBuf[8];
      const int n = snprintf(numBuf, sizeof(numBuf), "%d. ", static_cast<int>(i + 1));
      if (n > 0) appendStr(numBuf, static_cast<size_t>(n));
    }

    // Word type (e.g. "n.", "v.", "adj.")
    const uint8_t wtLen = datBuf[pos++];
    if (pos + wtLen > readSize) break;
    if (wtLen > 0) {
      appendStr("(", 1);
      appendStr(reinterpret_cast<const char*>(datBuf + pos), wtLen);
      appendStr(") ", 2);
    }
    pos += wtLen;

    // Definition text
    if (pos + 2 > readSize) break;
    const uint16_t defLen = static_cast<uint16_t>(datBuf[pos]) | (static_cast<uint16_t>(datBuf[pos + 1]) << 8);
    pos += 2;
    if (pos + defLen > readSize) break;
    appendStr(reinterpret_cast<const char*>(datBuf + pos), defLen);
    pos += defLen;

    // Separator between definitions
    if (i + 1 < defCount && outPos < outBufSize - 1) {
      appendStr("\n\n", 2);
    }
  }

  outBuf[outPos] = '\0';
  free(datBuf);

  LOG_DBG("DICT", "Lookup '%s': %d def(s), %u chars", key.c_str(), static_cast<int>(defCount),
          static_cast<unsigned>(outPos));
  return outPos > 0;
}

}  // namespace DictionaryStore
