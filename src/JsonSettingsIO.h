#pragma once

class CrossPointSettings;
class CrossPointState;
class Print;

namespace JsonSettingsIO {

// CrossPointSettings
bool saveSettings(const CrossPointSettings& s, const char* path);
bool loadSettings(CrossPointSettings& s, const char* json, bool* needsResave = nullptr);

// CrossPointState
bool saveState(const CrossPointState& s, const char* path);
bool writeState(const CrossPointState& s, Print& output);
bool loadState(CrossPointState& s, const char* json);

}  // namespace JsonSettingsIO
