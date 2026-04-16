#include "DictionaryLookupActivity.h"

#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cctype>
#include <cstring>

#include "DictionaryStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

DictionaryLookupActivity::DictionaryLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::unique_ptr<Page> page, int fontId, int marginLeft,
                                                   int marginTop)
    : Activity("DictLookup", renderer, mappedInput),
      page(std::move(page)),
      fontId(fontId),
      marginLeft(marginLeft),
      marginTop(marginTop) {}

void DictionaryLookupActivity::onEnter() {
  Activity::onEnter();

  // Build filtered list of text lines that have at least one word.
  textLines.clear();
  if (page) {
    for (const auto& el : page->elements) {
      if (el->getTag() == TAG_PageLine) {
        auto* line = static_cast<PageLine*>(el.get());
        if (line->getBlock() && line->getBlock()->wordCount() > 0) {
          textLines.push_back(line);
        }
      }
    }
  }

  lineIndex = 0;
  wordIndex = 0;
  viewMode = ViewMode::WORD_SELECTION;
  requestUpdate();
}

void DictionaryLookupActivity::onExit() {
  Activity::onExit();
  textLines.clear();
  page.reset();
}

void DictionaryLookupActivity::clampWordIndex() {
  if (textLines.empty()) return;
  const int maxWord = static_cast<int>(textLines[lineIndex]->getBlock()->wordCount()) - 1;
  if (wordIndex > maxWord) wordIndex = maxWord;
  if (wordIndex < 0) wordIndex = 0;
}

void DictionaryLookupActivity::loop() {
  if (textLines.empty()) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (viewMode == ViewMode::DEFINITION) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      viewMode = ViewMode::WORD_SELECTION;
      requestUpdate();
    }
    return;
  }

  // --- WORD_SELECTION mode ---

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    doLookup();
    return;
  }

  const int lineCount = static_cast<int>(textLines.size());

  // Left — previous word, wrap to end of previous line
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    if (wordIndex > 0) {
      wordIndex--;
    } else if (lineIndex > 0) {
      lineIndex--;
      wordIndex = static_cast<int>(textLines[lineIndex]->getBlock()->wordCount()) - 1;
    }
    requestUpdate();
    return;
  }

  // Right — next word, wrap to start of next line
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    const int wordCount = static_cast<int>(textLines[lineIndex]->getBlock()->wordCount());
    if (wordIndex < wordCount - 1) {
      wordIndex++;
    } else if (lineIndex < lineCount - 1) {
      lineIndex++;
      wordIndex = 0;
    }
    requestUpdate();
    return;
  }

  // Up — previous line
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (lineIndex > 0) {
      lineIndex--;
      clampWordIndex();
      requestUpdate();
    }
    return;
  }

  // Down — next line
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (lineIndex < lineCount - 1) {
      lineIndex++;
      clampWordIndex();
      requestUpdate();
    }
    return;
  }
}

// ---- Position helpers ----

int DictionaryLookupActivity::currentWordScreenX() const {
  const PageLine* line = textLines[lineIndex];
  const TextBlock& block = *line->getBlock();
  return static_cast<int>(block.getWordXpos()[wordIndex]) + marginLeft + static_cast<int>(line->xPos);
}

int DictionaryLookupActivity::currentWordScreenY() const {
  return marginTop + static_cast<int>(textLines[lineIndex]->yPos);
}

int DictionaryLookupActivity::currentWordWidth() const {
  const TextBlock& block = *textLines[lineIndex]->getBlock();
  const EpdFontFamily::Style style = block.getWordStyles()[wordIndex];
  return renderer.getTextWidth(fontId, block.getWords()[wordIndex].c_str(), style);
}

// ---- Lookup ----

void DictionaryLookupActivity::doLookup() {
  const TextBlock& block = *textLines[lineIndex]->getBlock();
  const std::string& raw = block.getWords()[wordIndex];

  // Strip leading/trailing non-alphanumeric chars for a clean display word.
  // e.g. "word,"→"word", "(example)"→"example". DictionaryStore normalises
  // the lookup key internally, so passing the stripped word is fine.
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
  currentWord = raw.substr(start, end - start);

  memset(defBuf, 0, sizeof(defBuf));
  if (!DictionaryStore::isAvailable()) {
    strncpy(defBuf, tr(STR_DICT_NO_FILE), sizeof(defBuf) - 1);
  } else {
    // matchedKey will be filled with the actual index entry found — which may be a
    // stemmed form (e.g. "negotiator" when the page word was "negotiators").
    char matchedKey[33] = {};
    if (DictionaryStore::lookup(currentWord, defBuf, sizeof(defBuf), matchedKey, sizeof(matchedKey))) {
      // Update display word to whatever the dictionary actually matched.
      // Preserve the original capitalisation of the first letter if it was upper-case.
      if (matchedKey[0] != '\0') {
        const bool wasCapital = !currentWord.empty() && std::isupper(static_cast<unsigned char>(currentWord[0]));
        currentWord = matchedKey;
        if (wasCapital && !currentWord.empty()) {
          currentWord[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(currentWord[0])));
        }
      }
    } else {
      strncpy(defBuf, tr(STR_DICT_NOT_FOUND), sizeof(defBuf) - 1);
    }
  }

  viewMode = ViewMode::DEFINITION;
  requestUpdate();
}

// ---- Rendering ----

void DictionaryLookupActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (textLines.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_DICT_NOT_FOUND));
    renderer.displayBuffer();
    return;
  }

  if (viewMode == ViewMode::DEFINITION) {
    renderDefinition();
  } else {
    renderWordSelection();
  }

  renderer.displayBuffer();
}

void DictionaryLookupActivity::renderWordSelection() {
  // Re-render the full book page at the reader's original margins.
  // The page layout is pre-cached for these exact margins, so no shifting is applied —
  // any shift would push right-edge words off screen without reflowing the layout.
  page->render(renderer, fontId, marginLeft, marginTop);

  // Compute highlight bounding box for current word
  const int sx = currentWordScreenX();
  const int sy = currentWordScreenY();
  const int sw = currentWordWidth();
  const int sh = renderer.getLineHeight(fontId);

  // Draw black highlight rectangle and white word text over it
  if (sw > 0) {
    renderer.fillRect(sx - 2, sy, sw + 4, sh);
    const TextBlock& block = *textLines[lineIndex]->getBlock();
    const EpdFontFamily::Style style = block.getWordStyles()[wordIndex];
    renderer.drawText(fontId, sx, sy, block.getWords()[wordIndex].c_str(), false, style);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_LOOK_UP), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DictionaryLookupActivity::renderDefinition() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int btnH = metrics.buttonHintsHeight;

  // Button hints are drawn in portrait-bottom coordinates. Determine which
  // screen edge that strip occupies for the current orientation, and reserve
  // space on that edge so the definition content is never obscured.
  int safeLeft = 0;
  int safeTop = 0;
  int safeRight = 0;
  int safeBottom = 0;
  switch (renderer.getOrientation()) {
    case GfxRenderer::LandscapeClockwise:
      safeLeft = btnH;
      break;
    case GfxRenderer::LandscapeCounterClockwise:
      safeRight = btnH;
      break;
    case GfxRenderer::PortraitInverted:
      safeTop = btnH;
      break;
    default:  // Portrait
      safeBottom = btnH;
      break;
  }

  // Header with the looked-up word
  const Rect headerRect{safeLeft, metrics.topPadding + safeTop,
                        pageWidth - safeLeft - safeRight, metrics.headerHeight};
  GUI.drawHeader(renderer, headerRect, currentWord.c_str());

  // Content area — kept inside the safe zone on all four sides
  const int contentX = safeLeft + metrics.contentSidePadding;
  const int contentY = metrics.topPadding + safeTop + metrics.headerHeight + metrics.verticalSpacing;
  const int contentWidth = pageWidth - safeLeft - safeRight - metrics.contentSidePadding * 2;
  const int contentBottom = pageHeight - safeBottom - metrics.verticalSpacing;

  int y = contentY;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);

  // Render definition text, splitting on '\n' for paragraph breaks between definitions
  const char* src = defBuf;
  const char* end = defBuf + strlen(defBuf);

  while (src < end && y < contentBottom - lineH) {
    // Find next newline (paragraph break) or end of string
    const char* nl = src;
    while (nl < end && *nl != '\n') ++nl;

    // Wrap this paragraph
    const std::string para(src, nl);
    if (!para.empty()) {
      const int maxLines = (contentBottom - y) / lineH;
      if (maxLines <= 0) break;
      const auto lines = renderer.wrappedText(UI_10_FONT_ID, para.c_str(), contentWidth, maxLines);
      for (const auto& line : lines) {
        if (y + lineH > contentBottom) break;
        renderer.drawText(UI_10_FONT_ID, contentX, y, line.c_str(), true);
        y += lineH;
      }
    }

    // Skip past newline(s) — add a small gap for paragraph breaks
    if (nl < end && *nl == '\n') {
      ++nl;
      if (nl < end && *nl == '\n') {
        ++nl;
        y += lineH / 2;  // extra half-line gap between definitions
      }
    }
    src = nl;
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
