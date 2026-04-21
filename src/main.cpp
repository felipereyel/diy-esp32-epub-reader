#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <esp_sleep.h>
#include "config.h"
#include "EpubList/Epub.h"
#include "EpubList/EpubList.h"
#include "EpubList/EpubReader.h"
#include "EpubList/EpubToc.h"
#include "EpubList/State.h"
#include <RubbishHtmlParser/RubbishHtmlParser.h>
#include "boards/Board.h"

#ifdef LOG_ENABLED
// Reference: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/log.html
#define LOG_LEVEL ESP_LOG_INFO
#else
#define LOG_LEVEL ESP_LOG_NONE
#endif
#include <esp_log.h>

extern "C"
{
  void app_main();
}

const char *TAG = "main";

typedef enum
{
  SELECTING_EPUB,
  SELECTING_TABLE_CONTENTS,
  READING_EPUB
} UIState;

// default to showing the list of epubs to the user
RTC_NOINIT_ATTR UIState ui_state = SELECTING_EPUB;
// the state data for the epub list and reader
RTC_DATA_ATTR EpubListState epub_list_state;
// the state data for the epub index list
RTC_DATA_ATTR EpubTocState epub_index_state;

void handleEpub(Renderer *renderer, UIAction action);
void handleEpubList(Renderer *renderer, UIAction action, bool needs_redraw);
void draw_count(Renderer *renderer, const char *text);

static EpubList *epub_list = nullptr;
static EpubReader *reader = nullptr;
static EpubToc *contents = nullptr;

void handleEpub(Renderer *renderer, UIAction action)
{
  if (!reader)
  {
    reader = new EpubReader(epub_list_state.epub_list[epub_list_state.selected_item], renderer);
    reader->load();
  }
  switch (action)
  {
  case UP:
    reader->prev();
    break;
  case DOWN:
    reader->next();
    break;
  case SELECT:
    // switch back to main screen
    ui_state = SELECTING_EPUB;
    renderer->clear_screen();
    // clear the epub reader away
    delete reader;
    reader = nullptr;
    // force a redraw
    if (!epub_list)
    {
      epub_list = new EpubList(renderer, epub_list_state);
    }
    handleEpubList(renderer, NONE, true);
    return;
  case NONE:
  default:
    break;
  }
  reader->render();
  EpubListItem &book_state = epub_list_state.epub_list[epub_list_state.selected_item];
  char buf[16];
  snprintf(buf, sizeof(buf), "%d/%dp", book_state.current_page + 1, book_state.pages_in_current_section);
  renderer->set_margin_top(0);
  renderer->draw_text(5, -10, buf, false, false);
  renderer->set_margin_top(35);
}

void handleEpubTableContents(Renderer *renderer, UIAction action, bool needs_redraw)
{
  if (!contents)
  {
    // restore saved chapter selection if exists
    uint16_t saved_toc = epub_list_state.epub_list[epub_list_state.selected_item].selected_toc;
    ESP_LOGI("main", "Creating TOC for book at index %d, saved_toc=%d", epub_list_state.selected_item, saved_toc);
    if (saved_toc > 0)
    {
      epub_index_state.selected_item = saved_toc;
      ESP_LOGI("main", "Restoring TOC to saved index: %d", saved_toc);
    }
    else
    {
      epub_index_state.selected_item = 0;
    }
    contents = new EpubToc(epub_list_state.epub_list[epub_list_state.selected_item], epub_index_state, renderer);
    contents->set_needs_redraw();
    contents->load();
  }
  switch (action)
  {
  case UP:
    contents->prev();
    break;
  case DOWN:
    contents->next();
    break;
  case SELECT:
    // save the selected chapter to state
    epub_list_state.epub_list[epub_list_state.selected_item].selected_toc = epub_index_state.selected_item;
    ESP_LOGI("main", "Saving selected_toc=%d", epub_index_state.selected_item);
    // setup the reader state
    ui_state = READING_EPUB;
    // create the reader and load the book
    reader = new EpubReader(epub_list_state.epub_list[epub_list_state.selected_item], renderer);
    // only override section if no saved position (new book selection)
    if (reader->has_saved_position())
    {
      ESP_LOGI("main", "Restoring saved position from file");
      reader->restore_position();
    }
    else
    {
      reader->set_state_section(contents->get_selected_toc());
    }
    reader->load();
    //switch to reading the epub
    delete contents;
    handleEpub(renderer, NONE);
    return;
  case NONE:
  default:
    break;
  }
  contents->render();
  char buf[20];
  snprintf(buf, sizeof(buf), "%d chapters", contents->get_toc_count());
  draw_count(renderer, buf);
}

void handleEpubList(Renderer *renderer, UIAction action, bool needs_redraw)
{
  // load up the epub list from the filesystem
  if (!epub_list)
  {
    ESP_LOGI("main", "Creating epub list");
    epub_list = new EpubList(renderer, epub_list_state);
    if (epub_list->load("/fs/"))
    {
      ESP_LOGI("main", "Epub files loaded");
      epub_list->load_state();
    }
  }
  if (needs_redraw)
  {
    epub_list->set_needs_redraw();
  }
  // work out what the user wants us to do
  switch (action)
  {
  case UP:
    epub_list->prev();
    break;
  case DOWN:
    epub_list->next();
    break;
  case SELECT:
    // switch to reading the epub
    // setup the reader state
    ui_state = SELECTING_TABLE_CONTENTS;
    // restore saved chapter selection BEFORE creating EpubToc
    {
      uint16_t saved_toc = epub_list_state.epub_list[epub_list_state.selected_item].selected_toc;
      if (saved_toc > 0)
      {
        epub_index_state.selected_item = saved_toc;
        ESP_LOGI("main", "Restoring saved TOC before creating: %d", saved_toc);
      }
    }
    // create the reader and load the book
    contents = new EpubToc(epub_list_state.epub_list[epub_list_state.selected_item], epub_index_state, renderer);
    contents->load();
    contents->set_needs_redraw();
    handleEpubTableContents(renderer, NONE, true);
    return;
  case NONE:
default:
    // nothing to do
    break;
  }

  epub_list->render();
  char buf[16];
  snprintf(buf, sizeof(buf), "%d books", epub_list_state.num_epubs);
  draw_count(renderer, buf);
}

void handleUserInteraction(Renderer *renderer, UIAction ui_action, bool needs_redraw)
{
  switch (ui_state)
  {
  case READING_EPUB:
    handleEpub(renderer, ui_action);
    break;
  case SELECTING_TABLE_CONTENTS:
    handleEpubTableContents(renderer, ui_action, needs_redraw);
    break;
  case SELECTING_EPUB:
  default:
    handleEpubList(renderer, ui_action, needs_redraw);
    break;
  }
}

// TODO - add the battery level
void draw_battery_level(Renderer *renderer, float voltage, float percentage)
{
  renderer->set_margin_top(0);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", (int)percentage);
  int text_width = renderer->get_text_width(buf);
  int xpos = renderer->get_page_width() - text_width - 5;
  int ypos = -10;
  renderer->draw_text(xpos, ypos, buf, false, false);
  renderer->set_margin_top(35);
}

void draw_count(Renderer *renderer, const char *text)
{
  renderer->set_margin_top(0);
  renderer->draw_text(5, -10, text, false, false);
  renderer->set_margin_top(35);
}

void main_task(void *param)
{
  // start the board up
  ESP_LOGI("main", "Powering up the board");
  Board *board = Board::factory();
  board->power_up();
  // create the renderer for the board
  ESP_LOGI("main", "Creating renderer");
  Renderer *renderer = board->get_renderer();
  // bring the file system up - SPIFFS or SDCard depending on the defines in platformio.ini
  ESP_LOGI("main", "Starting file system");
  board->start_filesystem();

  // battery details
  ESP_LOGI("main", "Starting battery monitor");
  Battery *battery = board->get_battery();
  if (battery)
  {
    battery->setup();
  }

  // make space for the battery display
  renderer->set_margin_top(35);
  // page margins
  renderer->set_margin_left(10);
  renderer->set_margin_right(10);

  // create a message queue for UI events
  xQueueHandle ui_queue = xQueueCreate(10, sizeof(UIAction));

  // set the controls up
  ESP_LOGI("main", "Setting up controls");
  ButtonControls *button_controls = board->get_button_controls(ui_queue);
  TouchControls *touch_controls = board->get_touch_controls(renderer, ui_queue);

  ESP_LOGI("main", "Controls configured");
  // work out if we were woken from deep sleep
  if (button_controls->did_wake_from_deep_sleep())
  {
    // restore the renderer state - it should have been saved when we went to sleep...
    bool hydrate_success = renderer->hydrate();
    UIAction ui_action = button_controls->get_deep_sleep_action();
    handleUserInteraction(renderer, ui_action, !hydrate_success);
  }
  else
  {
    // reset the screen
    renderer->reset();
    // make sure the UI is in the right state
    handleUserInteraction(renderer, NONE, true);
  }

  // draw the battery level before flushing the screen
  if (battery)
  {
    draw_battery_level(renderer, battery->get_voltage(), battery->get_percentage());
  }
  touch_controls->render(renderer);
  renderer->flush_display();

  // keep track of when the user last interacted and go to sleep after N seconds
  int64_t last_user_interaction = esp_timer_get_time();
  while (esp_timer_get_time() - last_user_interaction < 120 * 1000 * 1000)
  {
    UIAction ui_action = NONE;
    // wait for something to happen for 60 seconds
    if (xQueueReceive(ui_queue, &ui_action, pdMS_TO_TICKS(60000)) == pdTRUE)
    {
      if (ui_action != NONE)
      {
        // something happened!
        last_user_interaction = esp_timer_get_time();
        // show feedback on the touch controls
        touch_controls->renderPressedState(renderer, ui_action);
        handleUserInteraction(renderer, ui_action, false);

        // make sure to clear the feedback on the touch controls
        touch_controls->render(renderer);
      }
    }
    // update the battery level - do this even if there is no interaction so we
    // show the battery level even if the user is idle
    if (battery)
    {
      ESP_LOGI("main", "Battery Level %f, percent %d", battery->get_voltage(), battery->get_percentage());
      draw_battery_level(renderer, battery->get_voltage(), battery->get_percentage());
    }
    renderer->flush_display();
  }
  ESP_LOGI("main", "Saving state");
  // save the state of the renderer
  renderer->dehydrate();
  // turn off the filesystem
  board->stop_filesystem();
  // get ready to go to sleep
  board->prepare_to_sleep();
  ESP_ERROR_CHECK(esp_sleep_enable_ulp_wakeup());
  ESP_LOGI("main", "Entering deep sleep");
  // configure deep sleep options
  button_controls->setup_deep_sleep();
  vTaskDelay(pdMS_TO_TICKS(500));
  // go to sleep
  esp_deep_sleep_start();
}

void app_main()
{
  // Logging control
  esp_log_level_set("main", LOG_LEVEL);
  esp_log_level_set("EPUB", LOG_LEVEL);
  esp_log_level_set("PUBLIST", LOG_LEVEL);
  esp_log_level_set("ZIP", LOG_LEVEL);
  esp_log_level_set("JPG", LOG_LEVEL);
  esp_log_level_set("TOUCH", LOG_LEVEL);

  // dump out the epub list state
  ESP_LOGI("main", "epub list state num_epubs=%d", epub_list_state.num_epubs);
  ESP_LOGI("main", "epub list state is_loaded=%d", epub_list_state.is_loaded);
  ESP_LOGI("main", "epub list state selected_item=%d", epub_list_state.selected_item);

  ESP_LOGI("main", "Memory before main task start %d", esp_get_free_heap_size());
  xTaskCreatePinnedToCore(main_task, "main_task", 32768, NULL, 1, NULL, 1);
}