#pragma once

#include <Epub/Page.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <memory>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/Activity.h"

// DictionaryLookupActivity — in-reader word selector and dictionary definition viewer.
//
// Launched from the reader options menu. Receives the current rendered Page so it can re-draw
// the book content with a highlight cursor over the selected word.
//
// Two modes:
//   WORD_SELECTION — page is re-rendered with a black highlight box around the current word;
//                    Left/Right moves between words, Up/Down moves between lines,
//                    Confirm looks up the word, Back exits.
//   DEFINITION     — shows the formatted definition; Back or Confirm returns to WORD_SELECTION.

class DictionaryLookupActivity final : public Activity {
 public:
  DictionaryLookupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Page> page,
                            int fontId, int marginLeft, int marginTop);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class ViewMode { WORD_SELECTION, DEFINITION };

  std::unique_ptr<Page> page;
  const int fontId;
  const int marginLeft;
  const int marginTop;

  // Filtered list of PageLine pointers with at least one word (no images, no empty lines).
  // Pointers into page->elements — valid for the lifetime of this activity.
  std::vector<PageLine*> textLines;

  int lineIndex = 0;
  int wordIndex = 0;
  ViewMode viewMode = ViewMode::WORD_SELECTION;

  // Definition buffer — member allocation avoids putting 1 KB on the stack.
  char defBuf[1024] = {};
  std::string currentWord;

  void renderWordSelection();
  void renderDefinition();
  void doLookup();

  // Returns the screen x position of the current word.
  int currentWordScreenX() const;
  // Returns the screen y position of the current line.
  int currentWordScreenY() const;
  // Returns the width of the current word.
  int currentWordWidth() const;
  // Clamps wordIndex to be valid for the current line.
  void clampWordIndex();
};
