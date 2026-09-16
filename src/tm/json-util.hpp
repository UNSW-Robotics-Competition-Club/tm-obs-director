/*
obs-tm-director — VEX Tournament Manager automation for OBS Studio
Copyright (C) 2026 Sydney Robotics Academy <kevin.li@sydneyrobotics.com.au>

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

#include <nlohmann/json.hpp>

#include <ctime>
#include <string>

/// Small shared helpers for reading TM's JSON and stamping times.
///
/// These live in a header rather than in one translation unit because the API
/// client and the Cerberus client both parse the same kind of loosely-typed
/// JSON, and a second copy of "read this field, tolerate it being absent or the
/// wrong type" is a second place for the tolerance to be subtly different.
namespace tmd {

using nlohmann::json;

inline int64_t nowSeconds()
{
	return static_cast<int64_t>(std::time(nullptr));
}

/// Reads a JSON string field, tolerating absence and wrong types.
inline std::string stringField(const json &object, const char *key, const std::string &fallback = {})
{
	const auto it = object.find(key);
	if (it == object.end() || !it->is_string())
		return fallback;
	return it->get<std::string>();
}

inline int intField(const json &object, const char *key, int fallback = 0)
{
	const auto it = object.find(key);
	if (it == object.end() || !it->is_number_integer())
		return fallback;
	return it->get<int>();
}

inline bool boolField(const json &object, const char *key, bool fallback = false)
{
	const auto it = object.find(key);
	if (it == object.end() || !it->is_boolean())
		return fallback;
	return it->get<bool>();
}

/// Parses a response body, reporting why rather than throwing.
inline bool parseJson(const std::string &body, json &out, std::string &detail)
{
	out = json::parse(body, nullptr, false);
	if (out.is_discarded()) {
		detail = "response was not valid JSON";
		return false;
	}
	return true;
}

} // namespace tmd
