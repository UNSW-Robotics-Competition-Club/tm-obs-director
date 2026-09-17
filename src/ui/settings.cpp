/*
tm-obs-director — VEX Tournament Manager automation for OBS Studio
Copyright (C) 2026 UNSW Robotics Competition Club <kevin.li@sydneyrobotics.com.au>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "ui/settings.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <memory>

namespace tmd {

namespace {

/// Bumped only when an old file can no longer be read as-is. Written on every
/// save so that a future change has something to branch on rather than having to
/// guess at the shape.
constexpr int kSettingsVersion = 1;

constexpr const char *kFileName = "tm-director.json";

/// Trigger keys, indexed by Trigger. Names rather than positions, so inserting a
/// trigger later does not silently reinterpret everyone's saved overrides.
constexpr const char *kTriggerKeys[kTriggerCount] = {"queued", "activated", "started", "stopped"};

struct ObsDataDeleter {
	void operator()(obs_data_t *data) const { obs_data_release(data); }
};
using ObsDataPtr = std::unique_ptr<obs_data_t, ObsDataDeleter>;

struct ObsArrayDeleter {
	void operator()(obs_data_array_t *array) const { obs_data_array_release(array); }
};
using ObsArrayPtr = std::unique_ptr<obs_data_array_t, ObsArrayDeleter>;

std::string configDirectory()
{
	char *path = obs_module_config_path(nullptr);
	if (path == nullptr)
		return {};
	std::string directory = path;
	bfree(path);
	// obs_module_config_path appends a trailing separator for an empty file name.
	while (!directory.empty() && (directory.back() == '/' || directory.back() == '\\'))
		directory.pop_back();
	return directory;
}

} // namespace

std::string directorSettingsPath()
{
	const std::string directory = configDirectory();
	if (directory.empty())
		return {};
	return directory + "/" + kFileName;
}

std::string defaultMatchLogPath()
{
	const std::string directory = configDirectory();
	if (directory.empty())
		return {};
	return directory + "/match-log.csv";
}

bool saveDirectorSettings(const DirectorSettings &settings)
{
	const std::string directory = configDirectory();
	const std::string path = directorSettingsPath();
	if (path.empty())
		return false;

	// MKDIR_EXISTS is success: the directory is there either way.
	if (os_mkdirs(directory.c_str()) == MKDIR_ERROR)
		return false;

	ObsDataPtr root(obs_data_create());
	obs_data_set_int(root.get(), "version", kSettingsVersion);
	obs_data_set_string(root.get(), "tmAddress", settings.tmAddress.c_str());
	obs_data_set_string(root.get(), "tmApiKey", settings.tmApiKey.c_str());
	obs_data_set_int(root.get(), "fieldSetId", settings.fieldSetId);
	obs_data_set_string(root.get(), "cerberusEndpoint", settings.cerberusEndpoint.c_str());
	obs_data_set_bool(root.get(), "matchLogEnabled", settings.matchLogEnabled);
	obs_data_set_string(root.get(), "matchLogPath", settings.matchLogPath.c_str());

	obs_data_set_bool(root.get(), "enabled", settings.rules.enabled);
	obs_data_set_int(root.get(), "defaultDelayMs", settings.rules.defaultDelayMs);
	obs_data_set_int(root.get(), "defaultMinHoldMs", settings.rules.defaultMinHoldMs);

	ObsArrayPtr fields(obs_data_array_create());
	for (const FieldRule &rule : settings.rules.fields) {
		ObsDataPtr entry(obs_data_create());
		obs_data_set_int(entry.get(), "fieldId", rule.fieldId);
		obs_data_set_string(entry.get(), "name", rule.name.c_str());
		obs_data_set_string(entry.get(), "scene", rule.defaultScene.c_str());
		for (int i = 0; i < kTriggerCount; ++i)
			obs_data_set_string(entry.get(), kTriggerKeys[i], rule.sceneFor[i].c_str());
		obs_data_set_int(entry.get(), "delayMs", rule.delayMs);
		obs_data_set_int(entry.get(), "minHoldMs", rule.minHoldMs);
		obs_data_array_push_back(fields.get(), entry.get());
	}
	obs_data_set_array(root.get(), "fields", fields.get());

	return obs_data_save_json_safe(root.get(), path.c_str(), "tmp", "bak");
}

bool loadDirectorSettings(DirectorSettings &out)
{
	const std::string path = directorSettingsPath();
	if (path.empty())
		return false;

	ObsDataPtr root(obs_data_create_from_json_file_safe(path.c_str(), "bak"));
	if (!root)
		return false;

	// Defaults are declared rather than assumed, so a key absent from an older
	// file reads as the default instead of as an empty string or a zero.
	obs_data_set_default_string(root.get(), "tmAddress", "http://127.0.0.1");
	obs_data_set_default_bool(root.get(), "enabled", true);

	out.tmAddress = obs_data_get_string(root.get(), "tmAddress");
	out.tmApiKey = obs_data_get_string(root.get(), "tmApiKey");
	out.fieldSetId = static_cast<int>(obs_data_get_int(root.get(), "fieldSetId"));
	out.cerberusEndpoint = obs_data_get_string(root.get(), "cerberusEndpoint");
	out.matchLogEnabled = obs_data_get_bool(root.get(), "matchLogEnabled");
	out.matchLogPath = obs_data_get_string(root.get(), "matchLogPath");

	out.rules = RuleSet{};
	out.rules.enabled = obs_data_get_bool(root.get(), "enabled");
	out.rules.defaultDelayMs = static_cast<int>(obs_data_get_int(root.get(), "defaultDelayMs"));
	out.rules.defaultMinHoldMs = static_cast<int>(obs_data_get_int(root.get(), "defaultMinHoldMs"));

	ObsArrayPtr fields(obs_data_get_array(root.get(), "fields"));
	if (fields) {
		const size_t count = obs_data_array_count(fields.get());
		for (size_t i = 0; i < count; ++i) {
			ObsDataPtr entry(obs_data_array_item(fields.get(), i));
			if (!entry)
				continue;

			// A saved rule with no field id cannot be matched to anything TM
			// reports, so it is dropped rather than kept as a row that can never
			// fire.
			const int fieldId = static_cast<int>(obs_data_get_int(entry.get(), "fieldId"));
			if (fieldId == 0)
				continue;

			obs_data_set_default_int(entry.get(), "delayMs", -1);
			obs_data_set_default_int(entry.get(), "minHoldMs", -1);

			FieldRule rule;
			rule.fieldId = fieldId;
			rule.name = obs_data_get_string(entry.get(), "name");
			rule.defaultScene = obs_data_get_string(entry.get(), "scene");
			for (int t = 0; t < kTriggerCount; ++t)
				rule.sceneFor[t] = obs_data_get_string(entry.get(), kTriggerKeys[t]);
			rule.delayMs = static_cast<int>(obs_data_get_int(entry.get(), "delayMs"));
			rule.minHoldMs = static_cast<int>(obs_data_get_int(entry.get(), "minHoldMs"));
			out.rules.fields.push_back(rule);
		}
	}

	return true;
}

} // namespace tmd
