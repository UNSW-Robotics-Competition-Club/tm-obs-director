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

#include "ui/director.hpp"
#include "ui/dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr const char *kDockId = "tm-director";

tmd::Director *g_director = nullptr;
tmd::TmDirectorDock *g_dock = nullptr;

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		// Not in obs_module_load: scenes do not exist yet there, so a switch
		// arriving early would find nothing to switch to.
		if (g_dock != nullptr)
			g_dock->refreshScenes();
		if (g_director != nullptr)
			g_director->start();
		break;

	case OBS_FRONTEND_EVENT_SCENE_LIST_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		// A rule naming a scene that has since been renamed is kept and marked
		// rather than dropped, which is handled inside the dock.
		if (g_dock != nullptr)
			g_dock->refreshScenes();
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		// The socket owns a thread that calls back into the director. It has to
		// be stopped while Qt and libobs are both still standing.
		if (g_director != nullptr)
			g_director->stop();
		break;

	default:
		break;
	}
}

} // namespace

bool obs_module_load(void)
{
	g_director = new tmd::Director();
	g_dock = new tmd::TmDirectorDock(g_director);

	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("TMDirector.Dock"), g_dock)) {
		obs_log(LOG_ERROR, "could not add the TM Director dock");
		delete g_dock;
		g_dock = nullptr;
		delete g_director;
		g_director = nullptr;
		return false;
	}

	obs_frontend_add_event_callback(onFrontendEvent, nullptr);

	obs_log(LOG_INFO, "TM Director %s loaded", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);

	// Stop before the dock goes: the director emits into it.
	if (g_director != nullptr)
		g_director->stop();

	// OBS owns the dock wrapper and deletes the widget with it.
	obs_frontend_remove_dock(kDockId);
	g_dock = nullptr;

	delete g_director;
	g_director = nullptr;

	obs_log(LOG_INFO, "TM Director unloaded");
}
