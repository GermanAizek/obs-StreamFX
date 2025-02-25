/*
 * Modern effects for a modern Streamer
 * Copyright (C) 2020 Michael Fabian Dirks
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

#include "nvidia-cuda-stream.hpp"
#include <stdexcept>
#include "util/util-logging.hpp"

#ifdef _DEBUG
#define ST_PREFIX "<%s> "
#define D_LOG_ERROR(x, ...) P_LOG_ERROR(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_WARNING(x, ...) P_LOG_WARN(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_INFO(x, ...) P_LOG_INFO(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#define D_LOG_DEBUG(x, ...) P_LOG_DEBUG(ST_PREFIX##x, __FUNCTION_SIG__, __VA_ARGS__)
#else
#define ST_PREFIX "<nvidia::cuda::stream> "
#define D_LOG_ERROR(...) P_LOG_ERROR(ST_PREFIX __VA_ARGS__)
#define D_LOG_WARNING(...) P_LOG_WARN(ST_PREFIX __VA_ARGS__)
#define D_LOG_INFO(...) P_LOG_INFO(ST_PREFIX __VA_ARGS__)
#define D_LOG_DEBUG(...) P_LOG_DEBUG(ST_PREFIX __VA_ARGS__)
#endif

streamfx::nvidia::cuda::stream::~stream()
{
	D_LOG_DEBUG("Finalizing... (Addr: 0x%" PRIuPTR ")", this);

	_cuda->cuStreamDestroy(_stream);
}

streamfx::nvidia::cuda::stream::stream(::streamfx::nvidia::cuda::stream_flags flags, int32_t priority)
	: _cuda(::streamfx::nvidia::cuda::cuda::get())
{
	D_LOG_DEBUG("Initializating... (Addr: 0x%" PRIuPTR ")", this);

	streamfx::nvidia::cuda::result res;
	if (priority == 0) {
		res = _cuda->cuStreamCreate(&_stream, flags);
	} else {
		res = _cuda->cuStreamCreateWithPriority(&_stream, flags, priority);
	}
	switch (res) {
	case streamfx::nvidia::cuda::result::SUCCESS:
		break;
	default:
		throw std::runtime_error("Failed to create CUstream object.");
	}
}

::streamfx::nvidia::cuda::stream_t streamfx::nvidia::cuda::stream::get()
{
	return _stream;
}

void streamfx::nvidia::cuda::stream::synchronize()
{
	//D_LOG_DEBUG("Synchronizing... (Addr: 0x%" PRIuPTR ")", this);
	if (auto res = _cuda->cuStreamSynchronize(_stream); res != ::streamfx::nvidia::cuda::result::SUCCESS) {
		throw ::streamfx::nvidia::cuda::cuda_error(res);
	}
}
