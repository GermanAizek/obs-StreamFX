// FFMPEG Video Encoder Integration for OBS Studio
// Copyright (c) 2019 Michael Fabian Dirks <info@xaymar.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "encoder-ffmpeg.hpp"
#include "strings.hpp"
#include <sstream>
#include "codecs/hevc.hpp"
#include "ffmpeg/tools.hpp"
#include "handlers/debug_handler.hpp"
#include "obs/gs/gs-helper.hpp"
#include "plugin.hpp"

#ifdef ENABLE_ENCODER_FFMPEG_AMF
#include "handlers/amf_h264_handler.hpp"
#include "handlers/amf_hevc_handler.hpp"
#endif

#ifdef ENABLE_ENCODER_FFMPEG_NVENC
#include "handlers/nvenc_h264_handler.hpp"
#include "handlers/nvenc_hevc_handler.hpp"
#endif

#ifdef ENABLE_ENCODER_FFMPEG_PRORES
#include "handlers/prores_aw_handler.hpp"
#endif

#ifdef ENABLE_ENCODER_FFMPEG_DNXHR
#include "handlers/dnxhd_handler.hpp"
#endif

extern "C" {
#pragma warning(push)
#pragma warning(disable : 4244)
#include <obs-avc.h>
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#pragma warning(pop)
}

#ifdef WIN32
#include "ffmpeg/hwapi/d3d11.hpp"
#endif

// FFmpeg
#define ST_I18N_FFMPEG "Encoder.FFmpeg"
#define ST_I18N_FFMPEG_SUFFIX ST_I18N_FFMPEG ".Suffix"
#define ST_I18N_FFMPEG_CUSTOMSETTINGS ST_I18N_FFMPEG ".CustomSettings"
#define ST_KEY_FFMPEG_CUSTOMSETTINGS "FFmpeg.CustomSettings"
#define ST_I18N_FFMPEG_THREADS ST_I18N_FFMPEG ".Threads"
#define ST_KEY_FFMPEG_THREADS "FFmpeg.Threads"
#define ST_I18N_FFMPEG_GPU ST_I18N_FFMPEG ".GPU"
#define ST_KEY_FFMPEG_GPU "FFmpeg.GPU"

#define ST_I18N_KEYFRAMES ST_I18N_FFMPEG ".KeyFrames"
#define ST_I18N_KEYFRAMES_INTERVALTYPE ST_I18N_KEYFRAMES ".IntervalType"
#define ST_I18N_KEYFRAMES_INTERVALTYPE_(x) ST_I18N_KEYFRAMES_INTERVALTYPE "." x
#define ST_KEY_KEYFRAMES_INTERVALTYPE "KeyFrames.IntervalType"
#define ST_I18N_KEYFRAMES_INTERVAL ST_I18N_KEYFRAMES ".Interval"
#define ST_KEY_KEYFRAMES_INTERVAL_SECONDS "KeyFrames.Interval.Seconds"
#define ST_KEY_KEYFRAMES_INTERVAL_FRAMES "KeyFrames.Interval.Frames"

using namespace streamfx::encoder::ffmpeg;
using namespace streamfx::encoder::codec;

enum class keyframe_type { SECONDS, FRAMES };

ffmpeg_instance::ffmpeg_instance(obs_data_t* settings, obs_encoder_t* self, bool is_hw)
	: encoder_instance(settings, self, is_hw),

	  _factory(reinterpret_cast<ffmpeg_factory*>(obs_encoder_get_type_data(self))),

	  _codec(_factory->get_avcodec()), _context(nullptr), _handler(ffmpeg_manager::get()->get_handler(_codec->name)),

	  _scaler(), _packet(),

	  _hwapi(), _hwinst(),

	  _lag_in_frames(0), _sent_frames(0), _have_first_frame(false), _extra_data(), _sei_data(),

	  _free_frames(), _used_frames(), _free_frames_last_used()
{
	// Initialize GPU Stuff
	if (is_hw) {
		// Abort if user specified manual override.
		if ((obs_data_get_int(settings, ST_KEY_FFMPEG_GPU) != -1) || (obs_encoder_scaling_enabled(_self))
			|| (video_output_get_info(obs_encoder_video(_self))->format != VIDEO_FORMAT_NV12)) {
			throw std::runtime_error(
				"Selected settings prevent the use of hardware encoding, falling back to software.");
		}

#ifdef WIN32
		auto gctx = streamfx::obs::gs::context();
		if (gs_get_device_type() == GS_DEVICE_DIRECT3D_11) {
			_hwapi = std::make_shared<::streamfx::ffmpeg::hwapi::d3d11>();
		}
#endif
		if (!_hwapi) {
			throw std::runtime_error("Failed to create acceleration context.");
		}

		_hwinst = _hwapi->create_from_obs();
	}

	// Initialize context.
	_context = avcodec_alloc_context3(_codec);
	if (!_context) {
		DLOG_ERROR("Failed to create context for encoder '%s'.", _codec->name);
		throw std::runtime_error("Failed to create encoder context.");
	}

	// Create 8MB of precached Packet data for use later on.
	av_init_packet(&_packet);
	av_new_packet(&_packet, 8 * 1024 * 1024); // 8 MB precached Packet size.

	// Initialize
	if (is_hw) {
		initialize_hw(settings);
	} else {
		initialize_sw(settings);
	}

	// Update settings
	update(settings);

	// Initialize Encoder
	auto gctx = streamfx::obs::gs::context();
	int  res  = avcodec_open2(_context, _codec, NULL);
	if (res < 0) {
		throw std::runtime_error(::streamfx::ffmpeg::tools::get_error_description(res));
	}
}

ffmpeg_instance::~ffmpeg_instance()
{
	auto gctx = streamfx::obs::gs::context();
	if (_context) {
		// Flush encoders that require it.
		if ((_codec->capabilities & AV_CODEC_CAP_DELAY) != 0) {
			avcodec_send_frame(_context, nullptr);
			while (avcodec_receive_packet(_context, &_packet) >= 0) {
				avcodec_send_frame(_context, nullptr);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		// Close and free context.
		avcodec_close(_context);
		avcodec_free_context(&_context);
	}

	av_packet_unref(&_packet);

	_scaler.finalize();
}

void ffmpeg_instance::get_properties(obs_properties_t* props)
{
	if (_handler)
		_handler->get_properties(props, _codec, _context, _handler->is_hardware_encoder(_factory));

	obs_property_set_enabled(obs_properties_get(props, ST_KEY_KEYFRAMES_INTERVALTYPE), false);
	obs_property_set_enabled(obs_properties_get(props, ST_KEY_KEYFRAMES_INTERVAL_SECONDS), false);
	obs_property_set_enabled(obs_properties_get(props, ST_KEY_KEYFRAMES_INTERVAL_FRAMES), false);

	obs_property_set_enabled(obs_properties_get(props, ST_KEY_FFMPEG_THREADS), false);
	obs_property_set_enabled(obs_properties_get(props, ST_KEY_FFMPEG_GPU), false);
}

void ffmpeg_instance::migrate(obs_data_t* settings, uint64_t version)
{
	if (_handler)
		_handler->migrate(settings, version, _codec, _context);
}

bool ffmpeg_instance::update(obs_data_t* settings)
{
	bool support_reconfig           = false;
	bool support_reconfig_threads   = false;
	bool support_reconfig_gpu       = false;
	bool support_reconfig_keyframes = false;
	if (_handler) {
		support_reconfig = _handler->supports_reconfigure(_factory, support_reconfig_threads, support_reconfig_gpu,
														  support_reconfig_keyframes);
	}

	if (!_context->internal) {
		// FFmpeg Options
		_context->debug                 = 0;
		_context->strict_std_compliance = FF_COMPLIANCE_NORMAL;
	}

	if (!_context->internal || (support_reconfig && support_reconfig_threads)) {
		/// Threading
		if (!_hwinst) {
			_context->thread_type = 0;
			if (_codec->capabilities & AV_CODEC_CAP_FRAME_THREADS) {
				_context->thread_type |= FF_THREAD_FRAME;
			}
			if (_codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
				_context->thread_type |= FF_THREAD_SLICE;
			}
			if (_context->thread_type != 0) {
				int64_t threads = obs_data_get_int(settings, ST_I18N_FFMPEG_THREADS);
				if (threads > 0) {
					_context->thread_count = static_cast<int>(threads);
				} else {
					_context->thread_count = static_cast<int>(std::thread::hardware_concurrency());
				}
			} else {
				_context->thread_count = 1;
			}

			// Frame Delay (Lag In Frames)
			_context->delay = _context->thread_count;
		} else {
			_context->delay = 0;
		}
	}

	if (!_context->internal || (support_reconfig && support_reconfig_gpu)) {
		// Apply GPU Selection
		if (!_hwinst && ::streamfx::ffmpeg::tools::can_hardware_encode(_codec)) {
			av_opt_set_int(_context, "gpu", (int)obs_data_get_int(settings, ST_KEY_FFMPEG_GPU), AV_OPT_SEARCH_CHILDREN);
		}
	}

	if (!_context->internal || (support_reconfig && support_reconfig_keyframes)) {
		// Keyframes
		if (_handler && _handler->has_keyframe_support(_factory)) {
			// Key-Frame Options
			obs_video_info ovi;
			if (!obs_get_video_info(&ovi)) {
				throw std::runtime_error("obs_get_video_info failed, restart OBS Studio to fix it (hopefully).");
			}

			int64_t kf_type    = obs_data_get_int(settings, ST_KEY_KEYFRAMES_INTERVALTYPE);
			bool    is_seconds = (kf_type == 0);

			if (is_seconds) {
				_context->gop_size = static_cast<int>(obs_data_get_double(settings, ST_KEY_KEYFRAMES_INTERVAL_SECONDS)
													  * (ovi.fps_num / ovi.fps_den));
			} else {
				_context->gop_size = static_cast<int>(obs_data_get_int(settings, ST_KEY_KEYFRAMES_INTERVAL_FRAMES));
			}
			_context->keyint_min = _context->gop_size;
		}
	}

	if (!_context->internal || support_reconfig) {
		// Handler Options
		if (_handler)
			_handler->update(settings, _codec, _context);

		{ // FFmpeg Custom Options
			const char* opts     = obs_data_get_string(settings, ST_KEY_FFMPEG_CUSTOMSETTINGS);
			std::size_t opts_len = strnlen(opts, 65535);

			parse_ffmpeg_commandline(std::string{opts, opts + opts_len});
		}

		// Handler Overrides
		if (_handler)
			_handler->override_update(this, settings);
	}

	// Handler Logging
	if (!_context->internal || support_reconfig) {
		DLOG_INFO("[%s] Configuration:", _codec->name);
		DLOG_INFO("[%s]   FFmpeg:", _codec->name);
		DLOG_INFO("[%s]     Custom Settings: %s", _codec->name,
				  obs_data_get_string(settings, ST_KEY_FFMPEG_CUSTOMSETTINGS));
		DLOG_INFO("[%s]     Standard Compliance: %s", _codec->name,
				  ::streamfx::ffmpeg::tools::get_std_compliance_name(_context->strict_std_compliance));
		DLOG_INFO("[%s]     Threading: %s (with %i threads)", _codec->name,
				  ::streamfx::ffmpeg::tools::get_thread_type_name(_context->thread_type), _context->thread_count);

		DLOG_INFO("[%s]   Video:", _codec->name);
		if (_hwinst) {
			DLOG_INFO("[%s]     Texture: %" PRId32 "x%" PRId32 " %s %s %s", _codec->name, _context->width,
					  _context->height, ::streamfx::ffmpeg::tools::get_pixel_format_name(_context->sw_pix_fmt),
					  ::streamfx::ffmpeg::tools::get_color_space_name(_context->colorspace),
					  av_color_range_name(_context->color_range));
		} else {
			DLOG_INFO("[%s]     Input: %" PRId32 "x%" PRId32 " %s %s %s", _codec->name, _scaler.get_source_width(),
					  _scaler.get_source_height(),
					  ::streamfx::ffmpeg::tools::get_pixel_format_name(_scaler.get_source_format()),
					  ::streamfx::ffmpeg::tools::get_color_space_name(_scaler.get_source_colorspace()),
					  _scaler.is_source_full_range() ? "Full" : "Partial");
			DLOG_INFO("[%s]     Output: %" PRId32 "x%" PRId32 " %s %s %s", _codec->name, _scaler.get_target_width(),
					  _scaler.get_target_height(),
					  ::streamfx::ffmpeg::tools::get_pixel_format_name(_scaler.get_target_format()),
					  ::streamfx::ffmpeg::tools::get_color_space_name(_scaler.get_target_colorspace()),
					  _scaler.is_target_full_range() ? "Full" : "Partial");
			if (!_hwinst)
				DLOG_INFO("[%s]     On GPU Index: %lli", _codec->name, obs_data_get_int(settings, ST_KEY_FFMPEG_GPU));
		}
		DLOG_INFO("[%s]     Framerate: %" PRId32 "/%" PRId32 " (%f FPS)", _codec->name, _context->time_base.den,
				  _context->time_base.num,
				  static_cast<double_t>(_context->time_base.den) / static_cast<double_t>(_context->time_base.num));

		DLOG_INFO("[%s]   Keyframes: ", _codec->name);
		if (_context->keyint_min != _context->gop_size) {
			DLOG_INFO("[%s]     Minimum: %i frames", _codec->name, _context->keyint_min);
			DLOG_INFO("[%s]     Maximum: %i frames", _codec->name, _context->gop_size);
		} else {
			DLOG_INFO("[%s]     Distance: %i frames", _codec->name, _context->gop_size);
		}

		if (_handler) {
			_handler->log_options(settings, _codec, _context);
		}
	}

	return true;
}

static inline void copy_data(encoder_frame* frame, AVFrame* vframe)
{
	int h_chroma_shift, v_chroma_shift;
	av_pix_fmt_get_chroma_sub_sample(static_cast<AVPixelFormat>(vframe->format), &h_chroma_shift, &v_chroma_shift);

	for (std::size_t idx = 0; idx < MAX_AV_PLANES; idx++) {
		if (!frame->data[idx] || !vframe->data[idx])
			continue;

		std::size_t plane_height = static_cast<size_t>(vframe->height) >> (idx ? v_chroma_shift : 0);

		if (static_cast<uint32_t>(vframe->linesize[idx]) == frame->linesize[idx]) {
			std::memcpy(vframe->data[idx], frame->data[idx], frame->linesize[idx] * plane_height);
		} else {
			std::size_t ls_in  = static_cast<size_t>(frame->linesize[idx]);
			std::size_t ls_out = static_cast<size_t>(vframe->linesize[idx]);
			std::size_t bytes  = ls_in < ls_out ? ls_in : ls_out;

			uint8_t* to   = vframe->data[idx];
			uint8_t* from = frame->data[idx];

			for (std::size_t y = 0; y < plane_height; y++) {
				std::memcpy(to, from, bytes);
				to += ls_out;
				from += ls_in;
			}
		}
	}
}

bool ffmpeg_instance::encode_audio(struct encoder_frame* frame, struct encoder_packet* packet, bool* received_packet)
{
	throw std::logic_error("The method or operation is not implemented.");
}

bool ffmpeg_instance::encode_video(struct encoder_frame* frame, struct encoder_packet* packet, bool* received_packet)
{
	std::shared_ptr<AVFrame> vframe = pop_free_frame(); // Retrieve an empty frame.

	// Convert frame.
	{
		vframe->height          = _context->height;
		vframe->format          = _context->pix_fmt;
		vframe->color_range     = _context->color_range;
		vframe->colorspace      = _context->colorspace;
		vframe->color_primaries = _context->color_primaries;
		vframe->color_trc       = _context->color_trc;
		vframe->pts             = frame->pts;

		if ((_scaler.is_source_full_range() == _scaler.is_target_full_range())
			&& (_scaler.get_source_colorspace() == _scaler.get_target_colorspace())
			&& (_scaler.get_source_format() == _scaler.get_target_format())) {
			copy_data(frame, vframe.get());
		} else {
			int res = _scaler.convert(reinterpret_cast<uint8_t**>(frame->data), reinterpret_cast<int*>(frame->linesize),
									  0, _context->height, vframe->data, vframe->linesize);
			if (res <= 0) {
				DLOG_ERROR("Failed to convert frame: %s (%" PRId32 ").",
						   ::streamfx::ffmpeg::tools::get_error_description(res), res);
				return false;
			}
		}
	}

	if (!encode_avframe(vframe, packet, received_packet))
		return false;

	return true;
}

bool ffmpeg_instance::encode_video(uint32_t handle, int64_t pts, uint64_t lock_key, uint64_t* next_key,
								   struct encoder_packet* packet, bool* received_packet)
{
#ifdef D_PLATFORM_WINDOWS
	if (handle == GS_INVALID_HANDLE) {
		DLOG_ERROR("Received invalid handle.");
		*next_key = lock_key;
		return false;
	}

	std::shared_ptr<AVFrame> vframe = pop_free_frame();
	_hwinst->copy_from_obs(_context->hw_frames_ctx, handle, lock_key, next_key, vframe);

	vframe->color_range     = _context->color_range;
	vframe->colorspace      = _context->colorspace;
	vframe->color_primaries = _context->color_primaries;
	vframe->color_trc       = _context->color_trc;
	vframe->pts             = pts;

	if (!encode_avframe(vframe, packet, received_packet))
		return false;

	*next_key = lock_key;

	return true;
#else
	return false;
#endif
}

void ffmpeg_instance::initialize_sw(obs_data_t* settings)
{
	if (_codec->type == AVMEDIA_TYPE_VIDEO) {
		// Initialize Video Encoding
		auto voi = video_output_get_info(obs_encoder_video(_self));

		// Figure out a suitable pixel format to convert to if necessary.
		AVPixelFormat pix_fmt_source = ::streamfx::ffmpeg::tools::obs_videoformat_to_avpixelformat(voi->format);
		AVPixelFormat pix_fmt_target = AV_PIX_FMT_NONE;
		{
			if (_codec->pix_fmts) {
				pix_fmt_target = ::streamfx::ffmpeg::tools::get_least_lossy_format(_codec->pix_fmts, pix_fmt_source);
			} else { // If there are no supported formats, just pass in the current one.
				pix_fmt_target = pix_fmt_source;
			}

			if (_handler) // Allow Handler to override the automatic color format for sanity reasons.
				_handler->override_colorformat(pix_fmt_target, settings, _codec, _context);
		}

		// Setup from OBS information.
		::streamfx::ffmpeg::tools::context_setup_from_obs(voi, _context);

		// Override with other information.
		_context->width   = static_cast<int>(obs_encoder_get_width(_self));
		_context->height  = static_cast<int>(obs_encoder_get_height(_self));
		_context->pix_fmt = pix_fmt_target;

		_scaler.set_source_size(static_cast<uint32_t>(_context->width), static_cast<uint32_t>(_context->height));
		_scaler.set_source_color(_context->color_range == AVCOL_RANGE_JPEG, _context->colorspace);
		_scaler.set_source_format(pix_fmt_source);

		_scaler.set_target_size(static_cast<uint32_t>(_context->width), static_cast<uint32_t>(_context->height));
		_scaler.set_target_color(_context->color_range == AVCOL_RANGE_JPEG, _context->colorspace);
		_scaler.set_target_format(pix_fmt_target);

		// Create Scaler
		if (!_scaler.initialize(SWS_POINT)) {
			std::stringstream sstr;
			sstr << "Initializing scaler failed for conversion from '"
				 << ::streamfx::ffmpeg::tools::get_pixel_format_name(_scaler.get_source_format()) << "' to '"
				 << ::streamfx::ffmpeg::tools::get_pixel_format_name(_scaler.get_target_format())
				 << "' with color space '"
				 << ::streamfx::ffmpeg::tools::get_color_space_name(_scaler.get_source_colorspace()) << "' and "
				 << (_scaler.is_source_full_range() ? "full" : "partial") << " range.";
			throw std::runtime_error(sstr.str());
		}
	}
}

void ffmpeg_instance::initialize_hw(obs_data_t*)
{
#ifndef D_PLATFORM_WINDOWS
	throw std::runtime_error("OBS Studio currently does not support zero copy encoding for this platform.");
#else
	// Initialize Video Encoding
	const video_output_info* voi = video_output_get_info(obs_encoder_video(_self));

	// Apply pixel format settings.
	::streamfx::ffmpeg::tools::context_setup_from_obs(voi, _context);
	_context->sw_pix_fmt = _context->pix_fmt;
	_context->pix_fmt    = AV_PIX_FMT_D3D11;

	// Try to create a hardware context.
	_context->hw_device_ctx = _hwinst->create_device_context();
	_context->hw_frames_ctx = av_hwframe_ctx_alloc(_context->hw_device_ctx);
	if (!_context->hw_frames_ctx) {
		throw std::runtime_error("Creating hardware context failed.");
	}

	// Initialize Hardware Context
	AVHWFramesContext* ctx = reinterpret_cast<AVHWFramesContext*>(_context->hw_frames_ctx->data);
	ctx->width             = _context->width;
	ctx->height            = _context->height;
	ctx->format            = _context->pix_fmt;
	ctx->sw_format         = _context->sw_pix_fmt;
	if (int32_t res = av_hwframe_ctx_init(_context->hw_frames_ctx); res < 0) {
		std::array<char, 4096> buffer;

		int len = snprintf(buffer.data(), buffer.size(), "Failed initialize hardware context: %s (%" PRIu32 ")",
						   ::streamfx::ffmpeg::tools::get_error_description(res), res);
		throw std::runtime_error(std::string(buffer.data(), buffer.data() + len));
	}
#endif
}

void ffmpeg_instance::push_free_frame(std::shared_ptr<AVFrame> frame)
{
	auto now = std::chrono::high_resolution_clock::now();
	if (_free_frames.size() > 0) {
		if ((now - _free_frames_last_used) < std::chrono::seconds(1)) {
			_free_frames.push(frame);
		}
	} else {
		_free_frames.push(frame);
		_free_frames_last_used = std::chrono::high_resolution_clock::now();
	}
}

std::shared_ptr<AVFrame> ffmpeg_instance::pop_free_frame()
{
	std::shared_ptr<AVFrame> frame;
	if (_free_frames.size() > 0) {
		// Re-use existing frames first.
		frame = _free_frames.top();
		_free_frames.pop();
	} else {
		if (_hwinst) {
			frame = _hwinst->allocate_frame(_context->hw_frames_ctx);
		} else {
			frame = std::shared_ptr<AVFrame>(av_frame_alloc(), [](AVFrame* frame) {
				av_frame_unref(frame);
				av_frame_free(&frame);
			});

			frame->width  = _context->width;
			frame->height = _context->height;
			frame->format = _context->pix_fmt;

			int res = av_frame_get_buffer(frame.get(), 32);
			if (res < 0) {
				throw std::runtime_error(::streamfx::ffmpeg::tools::get_error_description(res));
			}
		}
	}

	return frame;
}

void ffmpeg_instance::push_used_frame(std::shared_ptr<AVFrame> frame)
{
	_used_frames.push(frame);
}

std::shared_ptr<AVFrame> ffmpeg_instance::pop_used_frame()
{
	auto frame = _used_frames.front();
	_used_frames.pop();
	return frame;
}

bool ffmpeg_instance::get_extra_data(uint8_t** data, size_t* size)
{
	if (_extra_data.size() == 0)
		return false;

	*data = _extra_data.data();
	*size = _extra_data.size();
	return true;
}

bool ffmpeg_instance::get_sei_data(uint8_t** data, size_t* size)
{
	if (_sei_data.size() == 0)
		return false;

	*data = _sei_data.data();
	*size = _sei_data.size();
	return true;
}

void ffmpeg_instance::get_video_info(struct video_scale_info* info)
{
	if (!is_hardware_encode()) {
		// Override input with supported format if software encode.
		info->format = ::streamfx::ffmpeg::tools::avpixelformat_to_obs_videoformat(_scaler.get_source_format());
	}
}

int ffmpeg_instance::receive_packet(bool* received_packet, struct encoder_packet* packet)
{
	int res = 0;

	av_packet_unref(&_packet);

	{
		auto gctx = streamfx::obs::gs::context();
		res       = avcodec_receive_packet(_context, &_packet);
	}
	if (res != 0) {
		return res;
	}

	if (!_have_first_frame) {
		if (_codec->id == AV_CODEC_ID_H264) {
			uint8_t*    tmp_packet;
			uint8_t*    tmp_header;
			uint8_t*    tmp_sei;
			std::size_t sz_packet, sz_header, sz_sei;

			obs_extract_avc_headers(_packet.data, static_cast<size_t>(_packet.size), &tmp_packet, &sz_packet,
									&tmp_header, &sz_header, &tmp_sei, &sz_sei);

			if (sz_header) {
				_extra_data.resize(sz_header);
				std::memcpy(_extra_data.data(), tmp_header, sz_header);
			}

			if (sz_sei) {
				_sei_data.resize(sz_sei);
				std::memcpy(_sei_data.data(), tmp_sei, sz_sei);
			}

			// Not required, we only need the Extra Data and SEI Data anyway.
			//std::memcpy(_current_packet.data, tmp_packet, sz_packet);
			//_current_packet.size = static_cast<int>(sz_packet);

			bfree(tmp_packet);
			bfree(tmp_header);
			bfree(tmp_sei);
		} else if (_codec->id == AV_CODEC_ID_HEVC) {
			hevc::extract_header_sei(_packet.data, static_cast<size_t>(_packet.size), _extra_data, _sei_data);
		} else if (_context->extradata != nullptr) {
			_extra_data.resize(static_cast<size_t>(_context->extradata_size));
			std::memcpy(_extra_data.data(), _context->extradata, static_cast<size_t>(_context->extradata_size));
		}
		_have_first_frame = true;
	}

	// Allow Handler Post-Processing
	if (_handler)
		_handler->process_avpacket(_packet, _codec, _context);

	// Build packet for use in OBS.
	packet->type     = OBS_ENCODER_VIDEO;
	packet->pts      = _packet.pts;
	packet->dts      = _packet.dts;
	packet->data     = _packet.data;
	packet->size     = static_cast<size_t>(_packet.size);
	packet->keyframe = !!(_packet.flags & AV_PKT_FLAG_KEY);
	*received_packet = true;

	// Figure out priority and drop_priority.
	// In theory, this is done by OBS, but its not doing a great job.
	packet->priority      = packet->keyframe ? 3 : 2;
	packet->drop_priority = 3;
	for (size_t idx = 0, edx = _packet.side_data_elems; idx < edx; idx++) {
		auto& side_data = _packet.side_data[idx];
		if (side_data.type == AV_PKT_DATA_QUALITY_STATS) {
			// Decisions based on picture type, if present.
			switch (side_data.data[sizeof(uint32_t)]) {
			case AV_PICTURE_TYPE_I:  // I-Frame
			case AV_PICTURE_TYPE_SI: // Switching I-Frame
				if (_packet.flags & AV_PKT_FLAG_KEY) {
					// Recovery only via IDR-Frame.
					packet->priority      = 3; // OBS_NAL_PRIORITY_HIGHEST
					packet->drop_priority = 2; // OBS_NAL_PRIORITY_HIGH
				} else {
					// Recovery via I- or IDR-Frame.
					packet->priority      = 2; // OBS_NAL_PRIORITY_HIGH
					packet->drop_priority = 2; // OBS_NAL_PRIORITY_HIGH
				}
				break;
			case AV_PICTURE_TYPE_P:  // P-Frame
			case AV_PICTURE_TYPE_SP: // Switching P-Frame
				// Recovery via I- or IDR-Frame.
				packet->priority      = 1; // OBS_NAL_PRIORITY_LOW
				packet->drop_priority = 2; // OBS_NAL_PRIORITY_HIGH
				break;
			case AV_PICTURE_TYPE_B: // B-Frame
				// Recovery via I- or IDR-Frame.
				packet->priority      = 0; // OBS_NAL_PRIORITY_DISPOSABLE
				packet->drop_priority = 2; // OBS_NAL_PRIORITY_HIGH
				break;
			case AV_PICTURE_TYPE_BI: // BI-Frame, theoretically identical to I-Frame.
				// Recovery via I- or IDR-Frame.
				packet->priority      = 2; // OBS_NAL_PRIORITY_HIGH
				packet->drop_priority = 2; // OBS_NAL_PRIORITY_HIGH
				break;
			default: // Unknown picture type.
				// Recovery only via IDR-Frame
				packet->priority      = 2; // OBS_NAL_PRIORITY_HIGH
				packet->drop_priority = 3; // OBS_NAL_PRIORITY_HIGHEST
				break;
			}
		}
	}

	// Push free frame back into pool.
	push_free_frame(pop_used_frame());

	return res;
}

int ffmpeg_instance::send_frame(std::shared_ptr<AVFrame> const frame)
{
	int res = 0;
	{
		auto gctx = streamfx::obs::gs::context();
		res       = avcodec_send_frame(_context, frame.get());
	}
	if (res == 0) {
		push_used_frame(frame);
	}

	return res;
}

bool ffmpeg_instance::encode_avframe(std::shared_ptr<AVFrame> frame, encoder_packet* packet, bool* received_packet)
{
	bool sent_frame  = false;
	bool recv_packet = false;
	bool should_lag  = (_sent_frames >= _lag_in_frames);

	auto loop_begin = std::chrono::high_resolution_clock::now();
	auto loop_end   = loop_begin + std::chrono::milliseconds(50);

	while ((!sent_frame || (should_lag && !recv_packet)) && !(std::chrono::high_resolution_clock::now() > loop_end)) {
		bool eagain_is_stupid = false;

		if (!sent_frame) {
			int res = send_frame(frame);
			switch (res) {
			case 0:
				sent_frame = true;
				frame      = nullptr;
				break;
			case AVERROR(EAGAIN):
				// This means we should call receive_packet again, but what do we do with that data?
				// Why can't we queue on both? Do I really have to implement threading for this stuff?
				if (*received_packet == true) {
					DLOG_WARNING("Skipped frame due to EAGAIN when a packet was already returned.");
					sent_frame = true;
				}
				eagain_is_stupid = true;
				break;
			case AVERROR(EOF):
				DLOG_ERROR("Skipped frame due to end of stream.");
				sent_frame = true;
				break;
			default:
				DLOG_ERROR("Failed to encode frame: %s (%" PRId32 ").",
						   ::streamfx::ffmpeg::tools::get_error_description(res), res);
				return false;
			}
		}

		if (!recv_packet) {
			int res = receive_packet(received_packet, packet);
			switch (res) {
			case 0:
				recv_packet = true;
				break;
			case AVERROR(EOF):
				DLOG_ERROR("Received end of file.");
				recv_packet = true;
				break;
			case AVERROR(EAGAIN):
				if (sent_frame) {
					recv_packet = true;
				}
				if (eagain_is_stupid) {
					DLOG_ERROR("Both send and recieve returned EAGAIN, encoder is broken.");
					return false;
				}
				break;
			default:
				DLOG_ERROR("Failed to receive packet: %s (%" PRId32 ").",
						   ::streamfx::ffmpeg::tools::get_error_description(res), res);
				return false;
			}
		}

		if (!sent_frame || !recv_packet) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	if (!sent_frame)
		push_free_frame(frame);

	return true;
}

bool ffmpeg_instance::is_hardware_encode()
{
	return _hwinst != nullptr;
}

const AVCodec* ffmpeg_instance::get_avcodec()
{
	return _codec;
}

const AVCodecContext* ffmpeg_instance::get_avcodeccontext()
{
	return _context;
}

void ffmpeg_instance::parse_ffmpeg_commandline(std::string text)
{
	// Steps to properly parse a command line:
	// 1. Split by space and package by quotes.
	// 2. Parse each resulting option individually.

	// First, we split by space and of course respect quotes while doing so.
	// That means that "-foo= bar" is stored as std::string("-foo= bar"),
	//  and things like -foo="bar" is stored as std::string("-foo=\"bar\"").
	// However "-foo"=bar" -foo2=bar" is stored as std::string("-foo=bar -foo2=bar")
	//  because the quote was not escaped.
	std::list<std::string> opts;
	std::stringstream      opt_stream{std::ios_base::in | std::ios_base::out | std::ios_base::binary};
	std::stack<char>       quote_stack;
	for (std::size_t p = 0; p <= text.size(); p++) {
		char here = p < text.size() ? text.at(p) : 0;

		if (here == '\\') {
			std::size_t p2 = p + 1;
			if (p2 < text.size()) {
				char here2 = text.at(p2);
				if (isdigit(here2)) { // Octal
					// Not supported yet.
					p++;
				} else if (here2 == 'x') { // Hexadecimal
					// Not supported yet.
					p += 3;
				} else if (here2 == 'u') { // 4 or 8 wide Unicode.
										   // Not supported yet.
				} else if (here2 == 'a') {
					opt_stream << '\a';
					p++;
				} else if (here2 == 'b') {
					opt_stream << '\b';
					p++;
				} else if (here2 == 'f') {
					opt_stream << '\f';
					p++;
				} else if (here2 == 'n') {
					opt_stream << '\n';
					p++;
				} else if (here2 == 'r') {
					opt_stream << '\r';
					p++;
				} else if (here2 == 't') {
					opt_stream << '\t';
					p++;
				} else if (here2 == 'v') {
					opt_stream << '\v';
					p++;
				} else if (here2 == '\\') {
					opt_stream << '\\';
					p++;
				} else if (here2 == '\'') {
					opt_stream << '\'';
					p++;
				} else if (here2 == '"') {
					opt_stream << '"';
					p++;
				} else if (here2 == '?') {
					opt_stream << '\?';
					p++;
				}
			}
		} else if ((here == '\'') || (here == '"')) {
			if (quote_stack.size() > 1) {
				opt_stream << here;
			}
			if (quote_stack.size() == 0) {
				quote_stack.push(here);
			} else if (quote_stack.top() == here) {
				quote_stack.pop();
			} else {
				quote_stack.push(here);
			}
		} else if ((here == 0) || ((here == ' ') && (quote_stack.size() == 0))) {
			std::string ropt = opt_stream.str();
			if (ropt.size() > 0) {
				opts.push_back(ropt);
				opt_stream.str(std::string());
				opt_stream.clear();
			}
		} else {
			opt_stream << here;
		}
	}

	// Now that we have a list of parameters as neatly grouped strings, and
	//  have also dealt with escaping for the most part. We want to parse
	//  an FFmpeg commandline option set here, so the first character in
	//  the string must be a '-'.
	for (std::string& opt : opts) {
		// Skip empty options.
		if (opt.size() == 0)
			continue;

		// Skip options that don't start with a '-'.
		if (opt.at(0) != '-') {
			DLOG_WARNING("Option '%s' is malformed, must start with a '-'.", opt.c_str());
			continue;
		}

		// Skip options that don't contain a '='.
		const char* cstr  = opt.c_str();
		const char* eq_at = strchr(cstr, '=');
		if (eq_at == nullptr) {
			DLOG_WARNING("Option '%s' is malformed, must contain a '='.", opt.c_str());
			continue;
		}

		try {
			std::string key   = opt.substr(1, static_cast<size_t>((eq_at - cstr) - 1));
			std::string value = opt.substr(static_cast<size_t>((eq_at - cstr) + 1));

			int res = av_opt_set(_context, key.c_str(), value.c_str(), AV_OPT_SEARCH_CHILDREN);
			if (res < 0) {
				DLOG_WARNING("Option '%s' (key: '%s', value: '%s') encountered error: %s", opt.c_str(), key.c_str(),
							 value.c_str(), ::streamfx::ffmpeg::tools::get_error_description(res));
			}
		} catch (const std::exception& ex) {
			DLOG_ERROR("Option '%s' encountered exception: %s", opt.c_str(), ex.what());
		}
	}
}

ffmpeg_factory::ffmpeg_factory(const AVCodec* codec) : _avcodec(codec)
{
	// Generate default identifier.
	{
		std::stringstream str;
		str << S_PREFIX << _avcodec->name;
		_id = str.str();
	}

	{ // Generate default name.
		std::stringstream str;
		if (_avcodec->long_name) {
			str << _avcodec->long_name;
			str << " (" << _avcodec->name << ")";
		} else {
			str << _avcodec->name;
		}
		str << D_TRANSLATE(ST_I18N_FFMPEG_SUFFIX);
		_name = str.str();
	}

	// Try and find a codec name that libOBS understands.
	if (auto* desc = avcodec_descriptor_get(_avcodec->id); desc) {
		_codec = desc->name;
	} else {
		// If FFmpeg doesn't know better, fall back to the name.
		_codec = _avcodec->name;
	}

	// Find any available handlers for this codec.
	if (_handler = ffmpeg_manager::get()->get_handler(_avcodec->name); _handler) {
		// Override any found info with the one specified by the handler.
		_handler->adjust_info(this, _avcodec, _id, _name, _codec);

		// Add texture capability for hardware encoders.
		if (_handler->is_hardware_encoder(this)) {
			_info.caps |= OBS_ENCODER_CAP_PASS_TEXTURE;
		}
	} else {
		// If there are no handlers, default to mark it deprecated.
		_info.caps |= OBS_ENCODER_CAP_DEPRECATED;
	}

	{ // Build Info structure.
		_info.id    = _id.c_str();
		_info.codec = _codec.c_str();
		if (_avcodec->type == AVMediaType::AVMEDIA_TYPE_VIDEO) {
			_info.type = obs_encoder_type::OBS_ENCODER_VIDEO;
		} else if (_avcodec->type == AVMediaType::AVMEDIA_TYPE_AUDIO) {
			_info.type = obs_encoder_type::OBS_ENCODER_AUDIO;
		}
	}

	// Register encoder and proxies.
	finish_setup();
	const std::string proxies[] = {
		std::string("streamfx--") + _avcodec->name,
		std::string("StreamFX-") + _avcodec->name,
		std::string("obs-ffmpeg-encoder_") + _avcodec->name,
	};
	for (auto proxy_id : proxies) {
		register_proxy(proxy_id);
		if (_info.caps & OBS_ENCODER_CAP_PASS_TEXTURE) {
			std::string proxy_fallback_id = proxy_id + "_sw";
			register_proxy(proxy_fallback_id);
		}
	}
}

ffmpeg_factory::~ffmpeg_factory() {}

const char* ffmpeg_factory::get_name()
{
	return _name.c_str();
}

void ffmpeg_factory::get_defaults2(obs_data_t* settings)
{
	if (_handler)
		_handler->get_defaults(settings, _avcodec, nullptr, _handler->is_hardware_encoder(this));

	if ((_avcodec->capabilities & AV_CODEC_CAP_INTRA_ONLY) == 0) {
		obs_data_set_default_int(settings, ST_KEY_KEYFRAMES_INTERVALTYPE, 0);
		obs_data_set_default_double(settings, ST_KEY_KEYFRAMES_INTERVAL_SECONDS, 2.0);
		obs_data_set_default_int(settings, ST_KEY_KEYFRAMES_INTERVAL_FRAMES, 300);
	}

	{ // Integrated Options
		// FFmpeg
		obs_data_set_default_string(settings, ST_KEY_FFMPEG_CUSTOMSETTINGS, "");
		obs_data_set_default_int(settings, ST_KEY_FFMPEG_THREADS, 0);
		obs_data_set_default_int(settings, ST_KEY_FFMPEG_GPU, -1);
	}
}

void ffmpeg_factory::migrate(obs_data_t* data, uint64_t version)
{
	if (_handler)
		_handler->migrate(data, version, _avcodec, nullptr);
}

static bool modified_keyframes(obs_properties_t* props, obs_property_t*, obs_data_t* settings) noexcept
try {
	bool is_seconds = obs_data_get_int(settings, ST_KEY_KEYFRAMES_INTERVALTYPE) == 0;
	obs_property_set_visible(obs_properties_get(props, ST_KEY_KEYFRAMES_INTERVAL_FRAMES), !is_seconds);
	obs_property_set_visible(obs_properties_get(props, ST_KEY_KEYFRAMES_INTERVAL_SECONDS), is_seconds);
	return true;
} catch (const std::exception& ex) {
	DLOG_ERROR("Unexpected exception in function '%s': %s.", __FUNCTION_NAME__, ex.what());
	return false;
} catch (...) {
	DLOG_ERROR("Unexpected exception in function '%s'.", __FUNCTION_NAME__);
	return false;
}

obs_properties_t* ffmpeg_factory::get_properties2(instance_t* data)
{
	obs_properties_t* props = obs_properties_create();

#ifdef ENABLE_FRONTEND
	{
		obs_properties_add_button2(props, S_MANUAL_OPEN, D_TRANSLATE(S_MANUAL_OPEN),
								   streamfx::encoder::ffmpeg::ffmpeg_factory::on_manual_open, this);
	}
#endif

	if (data) {
		data->get_properties(props);
	}

	if (_handler)
		_handler->get_properties(props, _avcodec, nullptr, _handler->is_hardware_encoder(this));

	if (_handler && _handler->has_keyframe_support(this)) {
		// Key-Frame Options
		obs_properties_t* grp = props;
		if (!streamfx::util::are_property_groups_broken()) {
			grp = obs_properties_create();
			obs_properties_add_group(props, ST_I18N_KEYFRAMES, D_TRANSLATE(ST_I18N_KEYFRAMES), OBS_GROUP_NORMAL, grp);
		}

		{ // Key-Frame Interval Type
			auto p =
				obs_properties_add_list(grp, ST_KEY_KEYFRAMES_INTERVALTYPE, D_TRANSLATE(ST_I18N_KEYFRAMES_INTERVALTYPE),
										OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
			obs_property_set_modified_callback(p, modified_keyframes);
			obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_KEYFRAMES_INTERVALTYPE_("Seconds")), 0);
			obs_property_list_add_int(p, D_TRANSLATE(ST_I18N_KEYFRAMES_INTERVALTYPE_("Frames")), 1);
		}
		{ // Key-Frame Interval Seconds
			auto p = obs_properties_add_float(grp, ST_KEY_KEYFRAMES_INTERVAL_SECONDS,
											  D_TRANSLATE(ST_I18N_KEYFRAMES_INTERVAL), 0.00,
											  std::numeric_limits<int16_t>::max(), 0.01);
			obs_property_float_set_suffix(p, " seconds");
		}
		{ // Key-Frame Interval Frames
			auto p =
				obs_properties_add_int(grp, ST_KEY_KEYFRAMES_INTERVAL_FRAMES, D_TRANSLATE(ST_I18N_KEYFRAMES_INTERVAL),
									   0, std::numeric_limits<int32_t>::max(), 1);
			obs_property_int_set_suffix(p, " frames");
		}
	}

	{
		obs_properties_t* grp = props;
		if (!streamfx::util::are_property_groups_broken()) {
			auto prs = obs_properties_create();
			obs_properties_add_group(props, ST_I18N_FFMPEG, D_TRANSLATE(ST_I18N_FFMPEG), OBS_GROUP_NORMAL, prs);
			grp = prs;
		}

		{ // Custom Settings
			auto p =
				obs_properties_add_text(grp, ST_KEY_FFMPEG_CUSTOMSETTINGS, D_TRANSLATE(ST_I18N_FFMPEG_CUSTOMSETTINGS),
										obs_text_type::OBS_TEXT_DEFAULT);
		}

		if (_handler && _handler->is_hardware_encoder(this)) {
			auto p = obs_properties_add_int(grp, ST_KEY_FFMPEG_GPU, D_TRANSLATE(ST_I18N_FFMPEG_GPU), -1,
											std::numeric_limits<uint8_t>::max(), 1);
		}

		if (_handler && _handler->has_threading_support(this)) {
			auto p = obs_properties_add_int_slider(grp, ST_KEY_FFMPEG_THREADS, D_TRANSLATE(ST_I18N_FFMPEG_THREADS), 0,
												   static_cast<int64_t>(std::thread::hardware_concurrency() * 2), 1);
		}
	};

	return props;
}

#ifdef ENABLE_FRONTEND
bool ffmpeg_factory::on_manual_open(obs_properties_t* props, obs_property_t* property, void* data)
{
	ffmpeg_factory* ptr = static_cast<ffmpeg_factory*>(data);
	streamfx::open_url(ptr->_handler->get_help_url(ptr->_avcodec));
	return false;
}
#endif

const AVCodec* ffmpeg_factory::get_avcodec()
{
	return _avcodec;
}

obs_encoder_info* streamfx::encoder::ffmpeg::ffmpeg_factory::get_info()
{
	return &_info;
}

ffmpeg_manager::ffmpeg_manager() : _factories(), _handlers(), _debug_handler()
{
	// Handlers
	_debug_handler = ::std::make_shared<handler::debug_handler>();
#ifdef ENABLE_ENCODER_FFMPEG_AMF
	register_handler("h264_amf", ::std::make_shared<handler::amf_h264_handler>());
	register_handler("hevc_amf", ::std::make_shared<handler::amf_hevc_handler>());
#endif
#ifdef ENABLE_ENCODER_FFMPEG_NVENC
	register_handler("h264_nvenc", ::std::make_shared<handler::nvenc_h264_handler>());
	register_handler("hevc_nvenc", ::std::make_shared<handler::nvenc_hevc_handler>());
#endif
#ifdef ENABLE_ENCODER_FFMPEG_PRORES
	register_handler("prores_aw", ::std::make_shared<handler::prores_aw_handler>());
#endif
#ifdef ENABLE_ENCODER_FFMPEG_DNXHR
	register_handler("dnxhd", ::std::make_shared<handler::dnxhd_handler>());
#endif
}

ffmpeg_manager::~ffmpeg_manager()
{
	_factories.clear();
}

void ffmpeg_manager::register_encoders()
{
	// Encoders
	void* iterator = nullptr;
	for (const AVCodec* codec = av_codec_iterate(&iterator); codec != nullptr; codec = av_codec_iterate(&iterator)) {
		// Only register encoders.
		if (!av_codec_is_encoder(codec))
			continue;

		if ((codec->type == AVMediaType::AVMEDIA_TYPE_AUDIO) || (codec->type == AVMediaType::AVMEDIA_TYPE_VIDEO)) {
			try {
				_factories.emplace(codec, std::make_shared<ffmpeg_factory>(codec));
			} catch (const std::exception& ex) {
				DLOG_ERROR("Failed to register encoder '%s': %s", codec->name, ex.what());
			}
		}
	}
}

void ffmpeg_manager::register_handler(std::string codec, std::shared_ptr<handler::handler> handler)
{
	_handlers.emplace(codec, handler);
}

std::shared_ptr<handler::handler> ffmpeg_manager::get_handler(std::string codec)
{
	auto fnd = _handlers.find(codec);
	if (fnd != _handlers.end())
		return fnd->second;
#ifdef _DEBUG
	return _debug_handler;
#else
	return nullptr;
#endif
}

bool ffmpeg_manager::has_handler(std::string codec)
{
	return (_handlers.find(codec) != _handlers.end());
}

std::shared_ptr<ffmpeg_manager> _ffmepg_encoder_factory_instance = nullptr;

void ffmpeg_manager::initialize()
{
	if (!_ffmepg_encoder_factory_instance) {
		_ffmepg_encoder_factory_instance = std::make_shared<ffmpeg_manager>();
		_ffmepg_encoder_factory_instance->register_encoders();
	}
}

void ffmpeg_manager::finalize()
{
	_ffmepg_encoder_factory_instance.reset();
}

std::shared_ptr<ffmpeg_manager> ffmpeg_manager::get()
{
	return _ffmepg_encoder_factory_instance;
}
