#ifndef DISPLAY_UTILS_H
#define DISPLAY_UTILS_H

#include <string>
#include <vector>
#include <lvgl.h>

namespace display_utils {
    void LoadIdleInfoSettings(std::string* idle_info_texts, std::vector<int>& valid_idle_indices, int& refresh_interval);
    void SetLabelScrollSpeed(lv_obj_t* label, int px_per_sec, int refresh_interval);
}

#endif