#pragma once
// World global channel controllers: the activity scripts that drive a map's weather. A
// controller (c_script_world_global_channel_controller, e.g. Outpost's wgc_weather_controller
// or the Heat Cascade's hz_emberwave) holds channel states; each state sets world global
// channels (rain_amount, ...) to a value range. The world channel registry (class 8080A002)
// gives each world channel a slot, which is the render-globals channel index the materials read
// (rain_amount = 7: wet surfaces and ripples).
#include "nim/core/package_manager.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace nim {

struct WorldChannelRegistry {
    TagHash tag;
    std::unordered_map<uint32_t, uint32_t> slots;  // fnv1(world channel name) -> global channel index
};
WorldChannelRegistry load_world_channel_registry(PackageManager& pm);

struct WeatherValue {
    uint32_t channel = 0;  // fnv1 of the world channel name
    int slot = -1;         // global channel index (registry), -1 if unregistered
    float min = 0, max = 0;
};
struct WeatherState {
    uint32_t name = 0;  // fnv1 of the state's name in the outliner
    std::vector<WeatherValue> values;
};
struct WeatherController {
    TagHash tag;
    std::string name;  // the bundle path words that resolve (e.g. "exp_outpost/hz_emberwave/wgc_controller")
    std::vector<WeatherState> states;  // distinct states over all channel groups
    bool heat_cascade() const { return name.find("emberwave") != std::string::npos; }
    // The state that sets its channels highest (the Heat Cascade's active state).
    const WeatherState* strongest_state() const;
};
struct MapWeather {
    WorldChannelRegistry registry;
    std::vector<WeatherController> controllers;
    std::unordered_map<uint32_t, std::string> names;  // fnv1 -> script string, for display
    std::string name(uint32_t hash) const;
};
// The global channel values a state sets, by channel index: the middle of each value's range
// (estimate: the engine picks within the range and blends over the state's transition time).
std::vector<std::pair<uint32_t, float>> weather_values(const WeatherState& state);

// The controllers in the packages of `bubble`'s map (the bubble package's name family, e.g.
// w64_outpost_*), with their states. Script strings of those packages name the hashes.
MapWeather load_map_weather(PackageManager& pm, TagHash bubble);

}  // namespace nim
