/*
 * Modern effects for a modern Streamer
 * Copyright (C) 2018 Michael Fabian Dirks
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */

#pragma once
#include "common.hpp"
#include "obs-source.hpp"

namespace streamfx::obs {
	template<class _factory, typename _instance>
	class source_factory {
		protected:
		obs_source_info                                         _info = {};
		std::map<std::string, std::shared_ptr<obs_source_info>> _proxies;
		std::set<std::string>                                   _proxy_names;

		public:
		source_factory(obs_source_type type = OBS_SOURCE_TYPE_INPUT)
		{
			_info.type_data = this;
			_info.type      = type;

			_info.get_name        = _get_name;
			_info.create          = _create;
			_info.destroy         = _destroy;
			_info.get_defaults2   = _get_defaults2;
			_info.get_properties2 = _get_properties2;
			_info.load            = _load;
			_info.update          = _update;
			_info.save            = _save;
			_info.filter_remove   = _filter_remove;
		}
		virtual ~source_factory() {}

		protected:
		void finish_setup()
		{
			// Adjust options by type:
			// - Transitions
			if (_info.type == OBS_SOURCE_TYPE_TRANSITION) {
				// Transitions don't have size.
				support_size(false);

				// Transitions are also composite, video and custom draw, but never async.
				_info.output_flags |= OBS_SOURCE_COMPOSITE | OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
				_info.output_flags &= ~(OBS_SOURCE_ASYNC);

				// Transition controls.
				_info.transition_start = _transition_start;
				_info.transition_stop  = _transition_stop;

				// Transitions are always synchronous video.
				_info.video_tick   = _video_tick;
				_info.video_render = _video_render;
			}
			// - Filter
			if (_info.type == OBS_SOURCE_TYPE_FILTER) {
				support_interaction(false);

				// If this is a video filter, require one of two code paths.
				uint32_t av = (_info.output_flags & OBS_SOURCE_ASYNC_VIDEO);
				if (av == OBS_SOURCE_VIDEO) {
					_info.video_tick   = _video_tick;
					_info.video_render = _video_render_filter;
				} else if (av == OBS_SOURCE_ASYNC_VIDEO) {
					_info.video_tick   = _video_tick;
					_info.filter_video = _filter_video;
				}

				// If this is an audio filter
				if ((_info.output_flags & OBS_SOURCE_AUDIO) == OBS_SOURCE_AUDIO) {
					_info.filter_audio = _filter_audio;
				}
			}
			// - Input
			if (_info.type == OBS_SOURCE_TYPE_INPUT) {
				uint32_t av = (_info.output_flags & OBS_SOURCE_ASYNC_VIDEO);
				if (av == OBS_SOURCE_VIDEO) {
					_info.video_tick   = _video_tick;
					_info.video_render = _video_render;
					support_size(true);
				} else if (av == OBS_SOURCE_ASYNC_VIDEO) {
					_info.video_tick = _video_tick;
					support_size(false);
				}
			}

			// Does the source have the composite audio flag?
			if ((_info.output_flags & OBS_SOURCE_COMPOSITE) == OBS_SOURCE_COMPOSITE) {
				// Does it also have some invalid flags?
				if ((_info.output_flags & OBS_SOURCE_AUDIO) == OBS_SOURCE_AUDIO) {
					throw std::runtime_error("Composite sources may not be audio sources.");
				}
				if ((_info.output_flags & OBS_SOURCE_ASYNC) == OBS_SOURCE_ASYNC) {
					throw std::runtime_error("Composite sources may not be asynchronous.");
				}
				// If not allow the assignment of audio_render.
				_info.audio_render = _audio_render;
			}

			// Does the source have the submix audio flag? (Internal to libOBS?)
			if ((_info.output_flags & OBS_SOURCE_SUBMIX) == OBS_SOURCE_SUBMIX) {
				if ((_info.output_flags & OBS_SOURCE_AUDIO) == OBS_SOURCE_AUDIO) {
					throw std::runtime_error("Composite sources may not be audio sources.");
				}
				if ((_info.output_flags & OBS_SOURCE_COMPOSITE) == OBS_SOURCE_COMPOSITE) {
					throw std::runtime_error("Submix sources may not be composite.");
				}
				_info.audio_mix = _audio_mix;
			}

			obs_register_source(&_info);
		}

		void register_proxy(std::string_view name)
		{
			auto iter = _proxy_names.emplace(name);

			// Create proxy.
			std::shared_ptr<obs_source_info> proxy = std::make_shared<obs_source_info>();
			memcpy(proxy.get(), &_info, sizeof(obs_source_info));
			proxy->id = iter.first->c_str();
			proxy->output_flags |= OBS_SOURCE_DEPRECATED;
			obs_register_source(proxy.get());

			_proxies.emplace(name, proxy);
		}

		private /* Factory */:
		static const char* _get_name(void* type_data) noexcept
		{
			try {
				if (type_data)
					return reinterpret_cast<_factory*>(type_data)->get_name();
				return nullptr;
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				return nullptr;
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				return nullptr;
			}
		}

		static void* _create(obs_data_t* settings, obs_source_t* source) noexcept
		{
			try {
				return reinterpret_cast<_factory*>(obs_source_get_type_data(source))->create(settings, source);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				return nullptr;
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				return nullptr;
			}
		}

		static void _get_defaults2(void* type_data, obs_data_t* settings) noexcept
		{
			try {
				if (type_data)
					reinterpret_cast<_factory*>(type_data)->get_defaults2(settings);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static obs_properties_t* _get_properties2(void* data, void* type_data) noexcept
		{
			try {
				if (type_data)
					return reinterpret_cast<_factory*>(type_data)->get_properties2(reinterpret_cast<_instance*>(data));
				return nullptr;
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				return nullptr;
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				return nullptr;
			}
		}

		private /* Instance */:
		static void _destroy(void* data) noexcept
		{
			try {
				if (data)
					delete reinterpret_cast<_instance*>(data);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _filter_remove(void* data, obs_source_t* source) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->filter_remove(source);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		public /* Instance > Video */:
		static void _video_tick(void* data, float seconds) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->video_tick(seconds);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _video_render(void* data, gs_effect_t* effect) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->video_render(effect);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _video_render_filter(void* data, gs_effect_t* effect) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->video_render(effect);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				obs_source_skip_video_filter(reinterpret_cast<_instance*>(data)->get());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				obs_source_skip_video_filter(reinterpret_cast<_instance*>(data)->get());
			}
		}

		static struct obs_source_frame* _filter_video(void* data, struct obs_source_frame* frame) noexcept
		{
			try {
				if (data)
					return reinterpret_cast<_instance*>(data)->filter_video(frame);
				return frame;
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				return frame;
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				return frame;
			}
		}

		public /* Instance > Audio */:
		static struct obs_audio_data* _filter_audio(void* data, struct obs_audio_data* frame) noexcept
		{
			try {
				if (data)
					return reinterpret_cast<_instance*>(data)->filter_audio(frame);
				return frame;
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				return frame;
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				return frame;
			}
		}

		static bool _audio_render(void* data, uint64_t* ts_out, struct obs_source_audio_mix* audio_output,
								  uint32_t mixers, std::size_t channels, std::size_t sample_rate) noexcept
		{
			try {
				if (data)
					return reinterpret_cast<_instance*>(data)->audio_render(ts_out, audio_output, mixers, channels,
																			sample_rate);
				return false;
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				return false;
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				return false;
			}
		}

		static bool _audio_mix(void* data, uint64_t* ts_out, struct audio_output_data* audio_output,
							   std::size_t channels, std::size_t sample_rate) noexcept
		{
			try {
				if (data)
					return reinterpret_cast<_instance*>(data)->audio_mix(ts_out, audio_output, channels, sample_rate);
				return false;
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				return false;
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				return false;
			}
		}

		public /* Instance > Transition */:
		static void _transition_start(void* data) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->transition_start();
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _transition_stop(void* data) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->transition_stop();
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		public /* Instance > Resolution */:
		void support_size(bool v)
		{
			if (v) {
				_info.get_width  = _get_width;
				_info.get_height = _get_height;
			} else {
				_info.get_width  = nullptr;
				_info.get_height = nullptr;
			}
		}

		static uint32_t _get_width(void* data) noexcept
		{
			try {
				if (data)
					return reinterpret_cast<_instance*>(data)->get_width();
				return 0;
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				return 0;
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				return 0;
			}
		}

		static uint32_t _get_height(void* data) noexcept
		{
			try {
				if (data)
					return reinterpret_cast<_instance*>(data)->get_height();
				return 0;
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				return 0;
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				return 0;
			}
		}

		public /* Instance > Children */:
		void support_active_child_sources(bool v)
		{
			if (v) {
				_info.enum_active_sources = _enum_active_sources;
			} else {
				_info.enum_active_sources = nullptr;
			}
		}

		static void _enum_active_sources(void* data, obs_source_enum_proc_t enum_callback, void* param) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->enum_active_sources(enum_callback, param);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		void support_child_sources(bool v)
		{
			if (v) {
				_info.enum_all_sources = _enum_all_sources;
			} else {
				_info.enum_all_sources = nullptr;
			}
		}

		static void _enum_all_sources(void* data, obs_source_enum_proc_t enum_callback, void* param) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->enum_all_sources(enum_callback, param);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		public /* Instance > Configuration */:
		static void _load(void* data, obs_data_t* settings) noexcept
		{
			try {
				auto priv = reinterpret_cast<_instance*>(data);
				if (priv) {
					uint64_t version = static_cast<uint64_t>(obs_data_get_int(settings, S_VERSION));
					priv->migrate(settings, version);
					obs_data_set_int(settings, S_VERSION, static_cast<int64_t>(STREAMFX_VERSION));
					obs_data_set_string(settings, S_COMMIT, STREAMFX_COMMIT);
					priv->load(settings);
				}
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _update(void* data, obs_data_t* settings) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->update(settings);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _save(void* data, obs_data_t* settings) noexcept
		{
			try {
				if (data) {
					reinterpret_cast<_instance*>(data)->save(settings);
					obs_data_set_int(settings, S_VERSION, static_cast<int64_t>(STREAMFX_VERSION));
					obs_data_set_string(settings, S_COMMIT, STREAMFX_COMMIT);
				}
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		public /* Instance > Activity Tracking */:
		void support_activity_tracking(bool v)
		{
			if (v) {
				_info.activate   = _activate;
				_info.deactivate = _deactivate;
			} else {
				_info.activate   = nullptr;
				_info.deactivate = nullptr;
			}
		}

		static void _activate(void* data) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->activate();
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _deactivate(void* data) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->deactivate();
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		public /* Instance > Visibility Tracking */:
		void support_visibility_tracking(bool v)
		{
			if (v) {
				_info.show = _show;
				_info.hide = _hide;
			} else {
				_info.show = nullptr;
				_info.hide = nullptr;
			}
		}

		static void _show(void* data) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->show();
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _hide(void* data) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->hide();
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		public /* Instance > Interaction */:
		void support_interaction(bool v)
		{
			if (v) {
				_info.output_flags |= (OBS_SOURCE_INTERACTION);
				_info.mouse_click = _mouse_click;
				_info.mouse_move  = _mouse_move;
				_info.mouse_wheel = _mouse_wheel;
				_info.focus       = _focus;
				_info.key_click   = _key_click;
			} else {
				_info.output_flags &= ~(OBS_SOURCE_INTERACTION);
				_info.mouse_click = nullptr;
				_info.mouse_move  = nullptr;
				_info.mouse_wheel = nullptr;
				_info.focus       = nullptr;
				_info.key_click   = nullptr;
			}
		}

		static void _mouse_click(void* data, const obs_mouse_event* event, int32_t type, bool mouse_up,
								 uint32_t click_count) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->mouse_click(event, type, mouse_up, click_count);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _mouse_move(void* data, const obs_mouse_event* event, bool mouse_leave) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->mouse_move(event, mouse_leave);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _mouse_wheel(void* data, const obs_mouse_event* event, int x_delta, int y_delta) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->mouse_wheel(event, x_delta, y_delta);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _focus(void* data, bool focus) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->focus(focus);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _key_click(void* data, const obs_key_event* event, bool key_up) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->key_click(event, key_up);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		public /* Instance > Media Controls */:
		void support_media_controls(bool v)
		{
			if (v) {
				_info.output_flags |= (OBS_SOURCE_CONTROLLABLE_MEDIA);
				_info.media_play_pause   = _media_play_pause;
				_info.media_restart      = _media_restart;
				_info.media_stop         = _media_stop;
				_info.media_next         = _media_next;
				_info.media_previous     = _media_previous;
				_info.media_get_duration = _media_get_duration;
				_info.media_get_time     = _media_get_time;
				_info.media_set_time     = _media_set_time;
				_info.media_get_state    = _media_get_state;
			} else {
				_info.output_flags &= ~(OBS_SOURCE_CONTROLLABLE_MEDIA);
				_info.media_play_pause   = nullptr;
				_info.media_restart      = nullptr;
				_info.media_stop         = nullptr;
				_info.media_next         = nullptr;
				_info.media_previous     = nullptr;
				_info.media_get_duration = nullptr;
				_info.media_get_time     = nullptr;
				_info.media_set_time     = nullptr;
				_info.media_get_state    = nullptr;
			}
		}

		static void _media_play_pause(void* data, bool pause) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->media_play_pause(pause);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _media_restart(void* data) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->media_restart();
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _media_stop(void* data) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->media_stop();
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _media_next(void* data) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->media_next();
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static void _media_previous(void* data) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->media_previous();
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static int64_t _media_get_duration(void* data) noexcept
		{
			try {
				if (data)
					return reinterpret_cast<_instance*>(data)->media_get_duration();
				return 0;
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				return 0;
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				return 0;
			}
		}

		static int64_t _media_get_time(void* data) noexcept
		{
			try {
				if (data)
					return reinterpret_cast<_instance*>(data)->media_get_time();
				return 0;
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				return 0;
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				return 0;
			}
		}

		static void _media_set_time(void* data, int64_t milliseconds) noexcept
		{
			try {
				if (data)
					reinterpret_cast<_instance*>(data)->media_set_time(milliseconds);
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
			}
		}

		static obs_media_state _media_get_state(void* data) noexcept
		{
			try {
				if (data)
					return reinterpret_cast<_instance*>(data)->media_get_state();
				return OBS_MEDIA_STATE_NONE;
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				return OBS_MEDIA_STATE_NONE;
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				return OBS_MEDIA_STATE_NONE;
			}
		}

		public /* Instance > Missing Files */:
		void support_missing_files(bool v)
		{
			if (v) {
				_info.missing_files = _missing_files;
			} else {
				_info.missing_files = nullptr;
			}
		}

		static obs_missing_files_t* _missing_files(void* data)
		{
			try {
				if (data)
					return reinterpret_cast<_instance*>(data)->missing_files();
				return nullptr;
			} catch (const std::exception& ex) {
				DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
				return nullptr;
			} catch (...) {
				DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
				return nullptr;
			}
		}

		public:
		virtual const char* get_name()
		{
			return "Not Yet Implemented";
		}

		virtual void* create(obs_data_t* settings, obs_source_t* source)
		{
			return reinterpret_cast<void*>(new _instance(settings, source));
		}

		virtual void get_defaults2(obs_data_t* data) {}

		virtual obs_properties_t* get_properties2(_instance* data)
		{
			return nullptr;
		}
	};

	class source_instance {
		protected:
		::streamfx::obs::source _self;

		public:
		source_instance(obs_data_t* settings, obs_source_t* source) : _self(source, false, false) {}
		virtual ~source_instance(){};

		virtual ::streamfx::obs::source get()
		{
			return _self;
		}

		virtual void filter_remove(obs_source_t* source) {}

		public /* Instance > Video */:
		virtual void video_tick(float_t seconds) {}

		virtual void video_render(gs_effect_t* effect) {}

		virtual struct obs_source_frame* filter_video(struct obs_source_frame* frame)
		{
			return frame;
		}

		public /* Instance > Audio */:
		virtual struct obs_audio_data* filter_audio(struct obs_audio_data* audio)
		{
			return audio;
		}

		virtual bool audio_render(uint64_t* ts_out, struct obs_source_audio_mix* audio_output, uint32_t mixers,
								  std::size_t channels, std::size_t sample_rate)
		{
			return false;
		}

		virtual bool audio_mix(uint64_t* ts_out, struct audio_output_data* audio_output, std::size_t channels,
							   std::size_t sample_rate)
		{
			return false;
		}

		public /* Instance > Transition */:
		virtual void transition_start() {}

		virtual void transition_stop() {}

		public /* Instance > Resolution */:
		virtual uint32_t get_width()
		{
			return 0;
		}

		virtual uint32_t get_height()
		{
			return 0;
		}

		public /* Instance > Children */:
		virtual void enum_active_sources(obs_source_enum_proc_t enum_callback, void* param) {}

		virtual void enum_all_sources(obs_source_enum_proc_t enum_callback, void* param) {}

		public /* Instance > Configuration */:
		virtual void load(obs_data_t* settings) {}

		virtual void migrate(obs_data_t* settings, uint64_t version) {}

		virtual void update(obs_data_t* settings) {}

		virtual void save(obs_data_t* settings) {}

		public /* Instance > Activity Tracking */:
		virtual void activate() {}

		virtual void deactivate() {}

		public /* Instance > Visibility Tracking */:
		virtual void show() {}

		virtual void hide() {}

		public /* Instance > Interaction */:
		virtual void mouse_click(const obs_mouse_event* event, int32_t type, bool mouse_up, uint32_t click_count) {}

		virtual void mouse_move(const obs_mouse_event* event, bool mouse_leave) {}

		virtual void mouse_wheel(const obs_mouse_event* event, int32_t x_delta, int32_t y_delta) {}

		virtual void focus(bool focus) {}

		virtual void key_click(const obs_key_event* event, bool key_up) {}

		public /* Media Controls */:
		virtual void media_play_pause(bool pause){};

		virtual void media_restart(){};

		virtual void media_stop(){};

		virtual void media_next(){};

		virtual void media_previous(){};

		virtual int64_t media_get_duration()
		{
			return 0;
		}

		virtual int64_t media_get_time()
		{
			return 0;
		}

		virtual void media_set_time(int64_t milliseconds) {}

		virtual obs_media_state media_get_state()
		{
			return OBS_MEDIA_STATE_NONE;
		}

		public /* Missing Files */:
		virtual obs_missing_files_t* missing_files()
		{
			return nullptr;
		};
	};

} // namespace streamfx::obs
