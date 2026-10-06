#pragma once

#include <Epub.h>

#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class EpubReaderClippingListActivity final : public UiListActivity {
 public:
  explicit EpubReaderClippingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                          const std::shared_ptr<Epub>& epub);

  void onEnter() override;
  void render(RenderLock&&) override;

 private:
  std::shared_ptr<Epub> epub;
  std::vector<std::string> clippingPreviews;
  std::vector<std::string> clippingSubtitles;
  std::vector<freeink::ui::ListItem> clippingRowItems;
  bool confirmingDelete = false;
  OptionPopup confirmPopup{true};

  void rebuildClippingRowItems();
  int listCount() const override { return static_cast<int>(clippingRowItems.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;

  void openSelectedClipping();
  void showClippingActions();
  void showDeleteConfirmation();
  void deleteSelectedClipping();
};
