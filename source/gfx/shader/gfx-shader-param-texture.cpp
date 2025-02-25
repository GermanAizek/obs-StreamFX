// Modern effects for a modern Streamer
// Copyright (C) 2019 Michael Fabian Dirks
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

#include "gfx-shader-param-texture.hpp"
#include "strings.hpp"
#include <map>
#include <sstream>
#include <stdexcept>
#include "gfx-shader.hpp"
#include "gfx/gfx-debug.hpp"
#include "obs/gs/gs-helper.hpp"
#include "obs/obs-source-tracker.hpp"
#include "util/util-platform.hpp"

// TODO:
// - FFT Audio Conversion
// - FFT Variable Size...

// UI:
// Name/Key {
//   Type = File/Source/Sink
//   File = ...
//   Source = ...
// }

#define ST_I18N "Shader.Parameter.Texture"
#define ST_KEY_TYPE ".Type"
#define ST_I18N_TYPE ST_I18N ".Type"
#define ST_I18N_TYPE_FILE ST_I18N_TYPE ".File"
#define ST_I18N_TYPE_SOURCE ST_I18N_TYPE ".Source"
#define ST_KEY_FILE ".File"
#define ST_I18N_FILE ST_I18N ".File"
#define ST_KEY_SOURCE ".Source"
#define ST_I18N_SOURCE ST_I18N ".Source"

static constexpr std::string_view _annotation_field_type      = "field_type";
static constexpr std::string_view _annotation_default         = "default";
static constexpr std::string_view _annotation_enum_entry      = "enum_%zu";
static constexpr std::string_view _annotation_enum_entry_name = "enum_%zu_name";

streamfx::gfx::shader::texture_field_type streamfx::gfx::shader::get_texture_field_type_from_string(std::string v)
{
	std::map<std::string, texture_field_type> matches = {
		{"input", texture_field_type::Input},
		{"enum", texture_field_type::Enum},
		{"enumeration", texture_field_type::Enum},
	};

	auto fnd = matches.find(v);
	if (fnd != matches.end())
		return fnd->second;

	return texture_field_type::Input;
}

streamfx::gfx::shader::texture_parameter::texture_parameter(streamfx::gfx::shader::shader*      parent,
															streamfx::obs::gs::effect_parameter param,
															std::string                         prefix)
	: parameter(parent, param, prefix), _field_type(texture_field_type::Input), _keys(), _values(),
	  _type(texture_type::File), _active(false), _visible(false), _dirty(true),
	  _dirty_ts(std::chrono::high_resolution_clock::now()), _file_path(), _file_texture(), _source_name(), _source(),
	  _source_child(), _source_active(), _source_visible(), _source_rendertarget()
{
	char string_buffer[256];

	// Build keys and names.
	{
		_keys.reserve(3);
		{ // Type
			snprintf(string_buffer, sizeof(string_buffer), "%s%s", get_key().data(), ST_KEY_TYPE);
			_keys.push_back(std::string(string_buffer));
		}
		{ // File
			snprintf(string_buffer, sizeof(string_buffer), "%s%s", get_key().data(), ST_KEY_FILE);
			_keys.push_back(std::string(string_buffer));
		}
		{ // Source
			snprintf(string_buffer, sizeof(string_buffer), "%s%s", get_key().data(), ST_KEY_SOURCE);
			_keys.push_back(std::string(string_buffer));
		}
	}

	// Detect Field Types
	if (auto anno = get_parameter().get_annotation(_annotation_field_type); anno) {
		_field_type = get_texture_field_type_from_string(anno.get_default_string());
	}
	if (auto anno = get_parameter().get_annotation(_annotation_default); anno) {
		_default = std::filesystem::path(anno.get_default_string());
	}

	if (field_type() == texture_field_type::Enum) {
		for (std::size_t idx = 0; idx < std::numeric_limits<std::size_t>::max(); idx++) {
			// Build key.
			std::string key_name;
			std::string key_value;
			{
				snprintf(string_buffer, sizeof(string_buffer), _annotation_enum_entry.data(), idx);
				key_value = std::string(string_buffer);
				snprintf(string_buffer, sizeof(string_buffer), _annotation_enum_entry_name.data(), idx);
				key_name = std::string(string_buffer);
			}

			// Value must be given, name is optional.
			if (auto eanno = get_parameter().get_annotation(key_value);
				eanno
				&& (get_type_from_effect_type(eanno.get_type()) == streamfx::gfx::shader::parameter_type::String)) {
				texture_enum_data entry;

				entry.data.file = std::filesystem::path(eanno.get_default_string());

				if (auto nanno = get_parameter().get_annotation(key_name);
					nanno && (nanno.get_type() == streamfx::obs::gs::effect_parameter::type::String)) {
					entry.name = nanno.get_default_string();
				} else {
					entry.name = "Unnamed Entry";
				}

				_values.push_back(entry);
			} else {
				break;
			}
		}

		if (_values.size() == 0) {
			_field_type = texture_field_type::Input;
		} else {
			_keys[1] = get_key();
		}
	}

	if (field_type() == texture_field_type::Input) {
		// Special code for Input-only fields.
	}
}

streamfx::gfx::shader::texture_parameter::~texture_parameter() {}

void streamfx::gfx::shader::texture_parameter::defaults(obs_data_t* settings)
{
	if (field_type() == texture_field_type::Input) {
		obs_data_set_default_int(settings, _keys[0].c_str(), static_cast<long long>(texture_type::File));
		obs_data_set_default_string(settings, _keys[1].c_str(), _default.generic_u8string().c_str());
		obs_data_set_default_string(settings, _keys[2].c_str(), "");
	} else {
		obs_data_set_default_string(settings, _keys[1].c_str(), _default.generic_u8string().c_str());
	}
}

bool streamfx::gfx::shader::texture_parameter::modified_type(void* priv, obs_properties_t* props, obs_property_t*,
															 obs_data_t* settings)
{
	auto self = reinterpret_cast<streamfx::gfx::shader::texture_parameter*>(priv);
	if (self->field_type() == texture_field_type::Input) {
		auto type = static_cast<texture_type>(obs_data_get_int(settings, self->_keys[0].c_str()));
		obs_property_set_visible(obs_properties_get(props, self->_keys[1].c_str()), type == texture_type::File);
		obs_property_set_visible(obs_properties_get(props, self->_keys[2].c_str()), type == texture_type::Source);
		return true;
	}
	return false;
}

void streamfx::gfx::shader::texture_parameter::properties(obs_properties_t* props, obs_data_t* settings)
{
	if (!is_visible())
		return;

	if (field_type() == texture_field_type::Enum) {
		auto p = obs_properties_add_list(props, get_key().data(), has_name() ? get_name().data() : get_key().data(),
										 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		for (auto v : _values) {
			obs_property_list_add_string(p, v.name.c_str(), v.data.file.generic_u8string().c_str());
		}
	} else {
		obs_properties_t* pr = obs_properties_create();
		{
			auto p = obs_properties_add_group(props, get_key().data(),
											  has_name() ? get_name().data() : get_key().data(), OBS_GROUP_NORMAL, pr);
			if (has_description())
				obs_property_set_long_description(p, get_description().data());
		}

		{
			auto p = obs_properties_add_list(pr, _keys[0].c_str(), D_TRANSLATE(ST_I18N_TYPE), OBS_COMBO_TYPE_LIST,
											 OBS_COMBO_FORMAT_INT);
			obs_property_set_modified_callback2(p, modified_type, this);
			obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_TYPE_FILE), static_cast<int64_t>(texture_type::File));
			obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_TYPE_SOURCE), static_cast<int64_t>(texture_type::Source));
		}

		{
			// ToDo: Filter and Default Path.
			auto p = obs_properties_add_path(pr, _keys[1].c_str(), D_TRANSLATE(ST_I18N_FILE), OBS_PATH_FILE, "* (*.*)",
											 nullptr);
		}

		{
			auto p = obs_properties_add_list(pr, _keys[2].c_str(), D_TRANSLATE(ST_I18N_SOURCE), OBS_COMBO_TYPE_LIST,
											 OBS_COMBO_FORMAT_STRING);
			obs_property_list_add_string(p, "", "");
			obs::source_tracker::get()->enumerate(
				[&p](std::string name, obs_source_t*) {
					std::stringstream sstr;
					sstr << name << " (" << D_TRANSLATE(S_SOURCETYPE_SOURCE) << ")";
					obs_property_list_add_string(p, sstr.str().c_str(), name.c_str());
					return false;
				},
				obs::source_tracker::filter_video_sources);
			obs::source_tracker::get()->enumerate(
				[&p](std::string name, obs_source_t*) {
					std::stringstream sstr;
					sstr << name << " (" << D_TRANSLATE(S_SOURCETYPE_SCENE) << ")";
					obs_property_list_add_string(p, sstr.str().c_str(), name.c_str());
					return false;
				},
				obs::source_tracker::filter_scenes);
		}

		modified_type(this, props, nullptr, settings);
	}
}

std::filesystem::path make_absolute_to(std::filesystem::path origin, std::filesystem::path destination)
{
	auto destination_dir = std::filesystem::absolute(destination.remove_filename());
	return std::filesystem::absolute(destination / origin);
}

void streamfx::gfx::shader::texture_parameter::update(obs_data_t* settings)
{
	// Value is assigned elsewhere.
	if (is_automatic())
		return;

	if (field_type() == texture_field_type::Input) {
		_type = static_cast<texture_type>(obs_data_get_int(settings, _keys[0].c_str()));
	} else {
		_type = texture_type::File;
	}

	if (_type == texture_type::File) {
		auto file_path = std::filesystem::path(obs_data_get_string(settings, _keys[1].c_str()));
		if (file_path.is_relative()) {
			file_path = make_absolute_to(file_path, get_parent()->get_shader_file());
		}

		if (_file_path != file_path) {
			_file_path = file_path;
			_dirty     = true;
			_dirty_ts  = std::chrono::high_resolution_clock::now() - std::chrono::milliseconds(1);
		}
	} else if (_type == texture_type::Source) {
		auto source_name = obs_data_get_string(settings, _keys[2].c_str());

		if (_source_name != source_name) {
			_source_name = source_name;
			_dirty       = true;
			_dirty_ts    = std::chrono::high_resolution_clock::now() - std::chrono::milliseconds(1);
		}
	}
}

void streamfx::gfx::shader::texture_parameter::assign()
{
	if (is_automatic())
		return;

	// If the data has been marked dirty, and the future timestamp minus the now is smaller than 0ms.
	if (_dirty && ((_dirty_ts - std::chrono::high_resolution_clock::now()) < std::chrono::milliseconds(0))) {
		// Reload or Reacquire everything necessary.
		try {
			// Remove now unused references.
			_source.release();
			_source_child.reset();
			_source_active.reset();
			_source_visible.reset();
			_source_rendertarget.reset();
			_file_texture.reset();

			if (((field_type() == texture_field_type::Input) && (_type == texture_type::File))
				|| (field_type() == texture_field_type::Enum)) {
				if (!_file_path.empty()) {
					_file_texture = std::make_shared<streamfx::obs::gs::texture>(
						streamfx::util::platform::native_to_utf8(_file_path).generic_u8string().c_str());
				}
			} else if ((field_type() == texture_field_type::Input) && (_type == texture_type::Source)) {
				// Try and grab the source itself.
				auto source = ::streamfx::obs::source(_source_name);
				if (!source) {
					throw std::runtime_error("Specified Source does not exist.");
				}

				// Attach the child to our parent.
				auto child = std::make_shared<::streamfx::obs::source_active_child>(source, get_parent()->get());

				// Create necessary visible and active objects.
				decltype(_source_active)  active;
				decltype(_source_visible) visible;
				if (_active) {
					active = ::streamfx::obs::source_active_reference::add_active_reference(source);
				}
				if (_visible) {
					visible = ::streamfx::obs::source_showing_reference::add_showing_reference(source);
				}

				// Create the necessary render target to capture the source.
				auto rt = std::make_shared<streamfx::obs::gs::rendertarget>(GS_RGBA, GS_ZS_NONE);

				// Propagate all of this into the storage.
				_source_rendertarget = rt;
				_source_visible      = visible;
				_source_active       = active;
				_source_child        = child;
				_source              = source;
			}

			_dirty = false;
		} catch (const std::exception& ex) {
			_dirty_ts = std::chrono::high_resolution_clock::now() + std::chrono::milliseconds(5000);
		} catch (...) {
			_dirty_ts = std::chrono::high_resolution_clock::now() + std::chrono::milliseconds(5000);
		}
	}

	// If this is a source and active or visible, capture it.
	if ((_type == texture_type::Source) && (_active || _visible) && _source_rendertarget) {
#ifdef ENABLE_PROFILING
		::streamfx::obs::gs::debug_marker profiler1{::streamfx::obs::gs::debug_color_capture, "Parameter '%s'",
													get_key().data()};
		::streamfx::obs::gs::debug_marker profiler2{::streamfx::obs::gs::debug_color_capture, "Capture '%s'",
													obs_source_get_name(_source.get())};
#endif
		uint32_t width  = obs_source_get_width(_source.get());
		uint32_t height = obs_source_get_height(_source.get());

		auto op = _source_rendertarget->render(width, height);

		gs_matrix_push();
		gs_ortho(0, static_cast<float>(width), 0, static_cast<float>(height), 0, 1);

		// ToDo: Figure out if this breaks some sources.
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		gs_enable_blending(false);

		gs_enable_color(true, true, true, true);

		obs_source_video_render(_source.get());

		gs_blend_state_pop();
		gs_matrix_pop();
	}

	if (_type == texture_type::Source) {
		if (_source_rendertarget) {
			auto tex = _source_rendertarget->get_texture();
			if (tex) {
				get_parameter().set_texture(_source_rendertarget->get_texture(), false);
			} else {
				get_parameter().set_texture(nullptr, false);
			}
		} else {
			get_parameter().set_texture(nullptr, false);
		}
	} else if (_type == texture_type::File) {
		if (_file_texture) {
			// Loaded files are always linear.
			get_parameter().set_texture(_file_texture, false);
		} else {
			get_parameter().set_texture(nullptr, false);
		}
	}
}

void streamfx::gfx::shader::texture_parameter::visible(bool visible)
{
	_visible = visible;
	if (visible) {
		if (_source) {
			_source_visible = ::streamfx::obs::source_showing_reference::add_showing_reference(_source);
		}
	} else {
		_source_visible.reset();
	}
}

void streamfx::gfx::shader::texture_parameter::active(bool active)
{
	_active = active;
	if (active) {
		if (_source) {
			_source_active = ::streamfx::obs::source_active_reference::add_active_reference(_source);
		}
	} else {
		_source_active.reset();
	}
}
