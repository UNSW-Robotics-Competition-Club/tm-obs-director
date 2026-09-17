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

#pragma once

#include "engine/rules.hpp"

#include <string>

namespace tmd {

/// Everything the operator configures, and the only thing that persists.
///
/// The Cerberus key is deliberately absent: it is compiled into the build, not
/// typed in, because a key is issued per build. The endpoint is here only so a
/// test or a staging deployment can be pointed elsewhere without a rebuild.
struct DirectorSettings {
	/// TM's address, as an http URL with no trailing slash. Loopback is the
	/// common case - OBS usually runs on the TM machine - but a second machine on
	/// the same subnet works too.
	std::string tmAddress = "http://127.0.0.1";

	/// From TM's Tools > Options > Web Publishing. Regenerated per event, so this
	/// is the field most likely to be stale at the start of a competition.
	std::string tmApiKey;

	/// Zero until the operator picks one from the list TM reports.
	int fieldSetId = 0;

	/// Empty means the endpoint this build was compiled with.
	std::string cerberusEndpoint;

	/// Write a row per played match: number, start, end, and where it falls in
	/// the stream and the recording. Off by default - it writes a file, and a
	/// plugin that writes files nobody asked for is a plugin people stop
	/// installing.
	bool matchLogEnabled = false;

	/// Empty means the default beside the settings file.
	std::string matchLogPath;

	RuleSet rules;
};

/// Where the match CSV goes when the operator has not chosen somewhere.
std::string defaultMatchLogPath();

/// Reads the saved settings. False when there is nothing saved yet, which is the
/// ordinary first-run case and not an error.
bool loadDirectorSettings(DirectorSettings &out);

/// Writes settings to the module's config directory, creating it if needed.
/// False when the write failed, which the dock surfaces rather than swallows -
/// settings that silently did not save are discovered at the worst moment.
bool saveDirectorSettings(const DirectorSettings &settings);

/// Path of the settings file, for logging and for the dock to show.
std::string directorSettingsPath();

} // namespace tmd
