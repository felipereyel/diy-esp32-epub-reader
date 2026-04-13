#include <string.h>
#include <stdio.h>
#ifndef UNIT_TEST
#include <esp_log.h>
#include <esp_system.h>
#else
#define ESP_LOGI(args...)
#define ESP_LOGE(args...)
#define ESP_LOGI(args...)
#define ESP_LOGD(args...)
#endif
#include "EpubReader.h"
#include "Epub.h"
#include "../RubbishHtmlParser/RubbishHtmlParser.h"
#include "../Renderer/Renderer.h"

static const char *TAG = "EREADER";

bool EpubReader::load()
{
  ESP_LOGD(TAG, "Before epub load: %d", esp_get_free_heap_size());
  // do we need to load the epub?
  if (!epub || epub->get_path() != state.path)
  {
    renderer->show_busy();
    delete epub;
    delete parser;
    parser = nullptr;
    epub = new Epub(state.path);
    if (epub->load())
    {
      ESP_LOGD(TAG, "After epub load: %d", esp_get_free_heap_size());
      return false;
    }
  }
  return true;
}

void EpubReader::parse_and_layout_current_section()
{
  if (!parser)
  {
    renderer->show_busy();
    ESP_LOGI(TAG, "Parse and render section %d", state.current_section);
    ESP_LOGD(TAG, "Before read html: %d", esp_get_free_heap_size());

    // if spine item is not found here then it will return get_spine_item(0)
    // so it does not crashes when you want to go after last page (out of vector range)
    std::string item = epub->get_spine_item(state.current_section);
    std::string base_path = item.substr(0, item.find_last_of('/') + 1);
    char *html = reinterpret_cast<char *>(epub->get_item_contents(item));
    ESP_LOGD(TAG, "After read html: %d", esp_get_free_heap_size());
    parser = new RubbishHtmlParser(html, strlen(html), base_path);
    free(html);
    ESP_LOGD(TAG, "After parse: %d", esp_get_free_heap_size());
    parser->layout(renderer, epub);
    ESP_LOGD(TAG, "After layout: %d", esp_get_free_heap_size());
    state.pages_in_current_section = parser->get_page_count();
  }
}

void EpubReader::next()
{
  state.current_page++;
  if (state.current_page >= state.pages_in_current_section)
  {
    state.current_section++;
    state.current_page = 0;
    delete parser;
    parser = nullptr;
  }
  save_state();
}

void EpubReader::prev()
{
  if (state.current_page == 0)
  {
    if (state.current_section > 0)
    {
      delete parser;
      parser = nullptr;
      state.current_section--;
      ESP_LOGD(TAG, "Going to previous section %d", state.current_section);
      parse_and_layout_current_section();
      state.current_page = state.pages_in_current_section - 1;
      save_state();
      return;
    }
  }
  state.current_page--;
  save_state();
}

void EpubReader::render()
{
  if (!parser)
  {
    parse_and_layout_current_section();
  }
  ESP_LOGD(TAG, "rendering page %d of %d", state.current_page, parser->get_page_count());
  parser->render_page(state.current_page, renderer, epub);
  ESP_LOGD(TAG, "rendered page %d of %d", state.current_page, parser->get_page_count());
  ESP_LOGD(TAG, "after render: %d", esp_get_free_heap_size());
}

void EpubReader::set_state_section(uint16_t current_section) {
  ESP_LOGI(TAG, "go to section:%d", current_section);
  state.current_section = current_section;
}

void EpubReader::save_state()
{
  ESP_LOGI(TAG, "Saving reading state for: %s", state.path);
  FILE *fp = fopen("/fs/reading_state.bin", "wb");
  if (fp)
  {
    fwrite(&state, sizeof(EpubListItem), 1, fp);
    fflush(fp);
    fclose(fp);
    ESP_LOGI(TAG, "State saved: section=%d, page=%d, selected_toc=%d", state.current_section, state.current_page, state.selected_toc);
  }
  else
  {
    ESP_LOGE(TAG, "Failed to open reading_state.bin for write");
  }
}

bool EpubReader::has_saved_position()
{
  FILE *fp = fopen("/fs/reading_state.bin", "rb");
  if (!fp)
  {
    ESP_LOGI(TAG, "No saved state file found");
    return false;
  }
  
  EpubListItem saved_state;
  size_t read_size = fread(&saved_state, sizeof(EpubListItem), 1, fp);
  fclose(fp);
  
  if (read_size != 1)
  {
    ESP_LOGI(TAG, "Failed to read saved state");
    return false;
  }
  
  // check if the saved state matches current epub path
  ESP_LOGI(TAG, "has_saved_position: saved=%s, current=%s", saved_state.path, state.path);
  if (strcmp(saved_state.path, state.path) != 0)
  {
    ESP_LOGI(TAG, "Path mismatch!");
    return false;
  }
  
  // always check if file exists and path matches - section 0 page 0 is valid
  ESP_LOGI(TAG, "Saved pos: section=%d, page=%d", saved_state.current_section, saved_state.current_page);
  return true;
}

void EpubReader::restore_position()
{
  FILE *fp = fopen("/fs/reading_state.bin", "rb");
  if (!fp)
  {
    ESP_LOGE(TAG, "No saved state file to restore");
    return;
  }
  
  EpubListItem saved_state;
  size_t read_size = fread(&saved_state, sizeof(EpubListItem), 1, fp);
  fclose(fp);
  
  if (read_size != 1)
  {
    ESP_LOGE(TAG, "Failed to read state for restore");
    return;
  }
  
  state.current_section = saved_state.current_section;
  state.current_page = saved_state.current_page;
  state.selected_toc = saved_state.selected_toc;
  ESP_LOGI(TAG, "Restored: section=%d, page=%d, selected_toc=%d", state.current_section, state.current_page, state.selected_toc);
}