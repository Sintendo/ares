/* Copyright (c) 2017-2022 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "wsi.hpp"
#include "quirks.hpp"

#if defined(ANDROID) && defined(HAVE_SWAPPY)
#include "swappy/swappyVk.h"
#endif

namespace Vulkan
{
WSI::WSI()
{
	// With frame latency of 1, we get the ideal latency where
	// we present, and then wait for the previous present to complete.
	// Once this unblocks, it means that the present we just queued up is scheduled to complete next vblank,
	// and the next frame to be recorded will have to be ready in 2 frames.
	// This is ideal, since worst case for full performance, we will have a pipeline of CPU -> GPU,
	// where CPU can spend 1 frame's worth of time, and GPU can spend one frame's worth of time.
	// For mobile, opt for 2 frames of latency, since TBDR likes deeper pipelines and we can absorb more
	// surfaceflinger jank.
#ifdef ANDROID
	present_frame_latency = 2;
#else
	present_frame_latency = 1;
#endif

	const char *env = getenv("GRANITE_VULKAN_PRESENT_WAIT_LATENCY");
	if (env)
	{
		present_frame_latency = uint32_t(strtoul(env, nullptr, 0));
		LOGI("Overriding VK_KHR_present_wait latency to %u frames.\n", present_frame_latency);
	}

	// Primaries are ST.2020 with D65 whitepoint as specified.
	hdr_metadata.displayPrimaryRed = { 0.708f, 0.292f };
	hdr_metadata.displayPrimaryGreen = { 0.170f, 0.797f };
	hdr_metadata.displayPrimaryBlue = { 0.131f, 0.046f };
	hdr_metadata.whitePoint = { 0.3127f, 0.3290f };

	// HDR10 range? Just arbitrary values, user can override later.
	hdr_metadata.minLuminance = 0.01f;
	hdr_metadata.maxLuminance = 1000.0f;
	hdr_metadata.maxContentLightLevel = 1000.0f;
	hdr_metadata.maxFrameAverageLightLevel = 200.0f;
}

void WSI::set_hdr_metadata(const VkHdrMetadataEXT &hdr)
{
	hdr_metadata = hdr;
	if (swapchain && swapchain_surface_format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
		table->vkSetHdrMetadataEXT(device->get_device(), 1, &swapchain, &hdr_metadata);
}

void WSIPlatform::set_window_title(const std::string &)
{
}

void WSIPlatform::destroy_surface(VkInstance instance, VkSurfaceKHR surface)
{
	vkDestroySurfaceKHR(instance, surface, nullptr);
}

uintptr_t WSIPlatform::get_fullscreen_monitor()
{
	return 0;
}

const VkApplicationInfo *WSIPlatform::get_application_info()
{
	return nullptr;
}

void WSI::set_window_title(const std::string &title)
{
	if (platform)
		platform->set_window_title(title);
}

double WSI::get_smooth_elapsed_time() const
{
	return smooth_elapsed_time;
}

double WSI::get_smooth_frame_time() const
{
	return smooth_frame_time;
}

bool WSI::init_from_existing_context(ContextHandle existing_context)
{
	VK_ASSERT(platform);
	context = std::move(existing_context);
	table = &context->get_device_table();
	return true;
}

bool WSI::init_external_swapchain(std::vector<ImageHandle> swapchain_images_)
{
	VK_ASSERT(context);
	VK_ASSERT(device);
	swapchain_width = platform->get_surface_width();
	swapchain_height = platform->get_surface_height();
	swapchain_aspect_ratio = platform->get_aspect_ratio();

	external_swapchain_images = std::move(swapchain_images_);

	swapchain_width = external_swapchain_images.front()->get_width();
	swapchain_height = external_swapchain_images.front()->get_height();
	swapchain_surface_format = { external_swapchain_images.front()->get_format(), VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };

	LOGI("Created swapchain %u x %u (fmt: %u).\n",
	     swapchain_width, swapchain_height, static_cast<unsigned>(swapchain_surface_format.format));

	platform->event_swapchain_destroyed();
	platform->event_swapchain_created(device.get(), VK_NULL_HANDLE, swapchain_width, swapchain_height,
	                                  swapchain_aspect_ratio,
	                                  external_swapchain_images.size(),
	                                  swapchain_surface_format.format, swapchain_surface_format.colorSpace,
	                                  swapchain_current_prerotate);

	device->init_external_swapchain(this->external_swapchain_images);
	platform->get_frame_timer().reset();
	external_acquire.reset();
	external_release.reset();
	return true;
}

void WSI::set_platform(WSIPlatform *platform_)
{
	platform = platform_;
}

bool WSI::init_device()
{
	VK_ASSERT(context);
	device = Util::make_handle<Device>();
	device->set_context(*context);
	platform->event_device_created(device.get());
	return true;
}

bool WSI::init_device(DeviceHandle device_handle)
{
	VK_ASSERT(context);
	device = std::move(device_handle);
	platform->event_device_created(device.get());
	return true;
}

bool WSI::init_surface_swapchain()
{
	VK_ASSERT(surface == VK_NULL_HANDLE);
	VK_ASSERT(context);
	VK_ASSERT(device);

	surface = platform->create_surface(context->get_instance(), context->get_gpu());
	if (surface == VK_NULL_HANDLE)
	{
		LOGE("Failed to create VkSurfaceKHR.\n");
		return false;
	}

	unsigned width = platform->get_surface_width();
	unsigned height = platform->get_surface_height();
	swapchain_aspect_ratio = platform->get_aspect_ratio();

	VkBool32 supported = VK_FALSE;
	uint32_t queue_present_support = 0;

	for (auto &index : context->get_queue_info().family_indices)
	{
		if (index != VK_QUEUE_FAMILY_IGNORED)
		{
			if (vkGetPhysicalDeviceSurfaceSupportKHR(context->get_gpu(), index, surface, &supported) ==
			    VK_SUCCESS && supported)
			{
				queue_present_support |= 1u << index;
			}
		}
	}

	if ((queue_present_support & (1u << context->get_queue_info().family_indices[QUEUE_INDEX_GRAPHICS])) == 0)
	{
		LOGE("No presentation queue found for GPU. Is it connected to a display?\n");
		return false;
	}

	device->set_swapchain_queue_family_support(queue_present_support);

	if (!blocking_init_swapchain(width, height))
	{
		LOGE("Failed to create swapchain.\n");
		return false;
	}

	device->init_swapchain(swapchain_images, swapchain_width, swapchain_height, swapchain_surface_format.format,
	                       swapchain_current_prerotate,
	                       current_extra_usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
	platform->get_frame_timer().reset();
	return true;
}

bool WSI::init_simple(unsigned num_thread_indices, const Context::SystemHandles &system_handles)
{
	if (!init_context_from_platform(num_thread_indices, system_handles))
		return false;
	if (!init_device())
		return false;
	if (!init_surface_swapchain())
		return false;
	return true;
}

bool WSI::init_context_from_platform(unsigned num_thread_indices, const Context::SystemHandles &system_handles)
{
	VK_ASSERT(platform);
	auto instance_ext = platform->get_instance_extensions();
	auto device_ext = platform->get_device_extensions();
	auto new_context = Util::make_handle<Context>();

	new_context->set_application_info(platform->get_application_info());
	new_context->set_num_thread_indices(num_thread_indices);
	new_context->set_system_handles(system_handles);
	if (!new_context->init_instance_and_device(
		instance_ext.data(), instance_ext.size(),
		device_ext.data(), device_ext.size()))
	{
		LOGE("Failed to create Vulkan device.\n");
		return false;
	}

	return init_from_existing_context(std::move(new_context));
}

void WSI::reinit_surface_and_swapchain(VkSurfaceKHR new_surface)
{
	LOGI("init_surface_and_swapchain()\n");
	if (new_surface != VK_NULL_HANDLE)
	{
		VK_ASSERT(surface == VK_NULL_HANDLE);
		surface = new_surface;
	}

	swapchain_width = platform->get_surface_width();
	swapchain_height = platform->get_surface_height();
	update_framebuffer(swapchain_width, swapchain_height);
}

void WSI::drain_swapchain()
{
	release_semaphores.clear();
	device->set_acquire_semaphore(0, Semaphore{});
	device->consume_release_semaphore();
	device->wait_idle();
}

void WSI::tear_down_swapchain()
{
	drain_swapchain();
	platform->event_swapchain_destroyed();

	if (swapchain != VK_NULL_HANDLE)
	{
		if (device->get_device_features().present_wait_features.presentWait && present_last_id)
			table->vkWaitForPresentKHR(context->get_device(), swapchain, present_last_id, UINT64_MAX);
		table->vkDestroySwapchainKHR(context->get_device(), swapchain, nullptr);
	}
	swapchain = VK_NULL_HANDLE;
	has_acquired_swapchain_index = false;
	present_id = 0;
	present_last_id = 0;
}

void WSI::deinit_surface_and_swapchain()
{
	LOGI("deinit_surface_and_swapchain()\n");

	tear_down_swapchain();

	if (surface != VK_NULL_HANDLE)
	{
		platform->destroy_surface(context->get_instance(), surface);
		surface = VK_NULL_HANDLE;
	}
}

void WSI::set_external_frame(unsigned index, Semaphore acquire_semaphore, double frame_time)
{
	external_frame_index = index;
	external_acquire = std::move(acquire_semaphore);
	frame_is_external = true;
	external_frame_time = frame_time;
}

bool WSI::begin_frame_external()
{
	device->next_frame_context();

	// Need to handle this stuff from outside.
	if (has_acquired_swapchain_index)
		return false;

	auto frame_time = platform->get_frame_timer().frame(external_frame_time);
	auto elapsed_time = platform->get_frame_timer().get_elapsed();

	// Assume we have been given a smooth frame pacing.
	smooth_frame_time = frame_time;
	smooth_elapsed_time = elapsed_time;

	// Poll after acquire as well for optimal latency.
	platform->poll_input();

	swapchain_index = external_frame_index;
	platform->event_frame_tick(frame_time, elapsed_time);

	platform->event_swapchain_index(device.get(), swapchain_index);
	device->set_acquire_semaphore(swapchain_index, external_acquire);
	external_acquire.reset();
	return true;
}

Semaphore WSI::consume_external_release_semaphore()
{
	Semaphore sem;
	std::swap(external_release, sem);
	return sem;
}

//#define VULKAN_WSI_TIMING_DEBUG

void WSI::wait_swapchain_latency()
{
	if (device->get_device_features().present_wait_features.presentWait &&
	    present_last_id > present_frame_latency &&
	    current_present_mode == PresentMode::SyncToVBlank)
	{
		// The effective latency is more like present_frame_latency + 1.
		// If 0, we wait for vblank, and we must do CPU work and GPU work in one frame
		// to hit next vblank.
		uint64_t target = present_last_id - present_frame_latency;

#ifdef VULKAN_WSI_TIMING_DEBUG
		auto begin_wait = Util::get_current_time_nsecs();
#endif
		auto wait_ts = device->write_calibrated_timestamp();
		VkResult wait_result = table->vkWaitForPresentKHR(context->get_device(), swapchain, target, UINT64_MAX);
		device->register_time_interval("WSI", std::move(wait_ts),
		                               device->write_calibrated_timestamp(), "wait_frame_latency");
		if (wait_result != VK_SUCCESS)
			LOGE("vkWaitForPresentKHR failed, vr %d.\n", wait_result);
#ifdef VULKAN_WSI_TIMING_DEBUG
		auto end_wait = Util::get_current_time_nsecs();
				LOGI("WaitForPresentKHR took %.3f ms.\n", 1e-6 * double(end_wait - begin_wait));
#endif
	}
}

bool WSI::begin_frame()
{
	if (frame_is_external)
		return begin_frame_external();

#ifdef VULKAN_WSI_TIMING_DEBUG
	auto next_frame_start = Util::get_current_time_nsecs();
#endif

	device->next_frame_context();

#ifdef VULKAN_WSI_TIMING_DEBUG
	auto next_frame_end = Util::get_current_time_nsecs();
	LOGI("Waited for vacant frame context for %.3f ms.\n", (next_frame_end - next_frame_start) * 1e-6);
#endif

	if (swapchain == VK_NULL_HANDLE || platform->should_resize() || swapchain_is_suboptimal)
		update_framebuffer(platform->get_surface_width(), platform->get_surface_height());

	if (swapchain == VK_NULL_HANDLE)
	{
		LOGE("Completely lost swapchain. Cannot continue.\n");
		return false;
	}

	if (has_acquired_swapchain_index)
		return true;

	external_release.reset();

	VkResult result;
	do
	{
		auto acquire = device->request_legacy_semaphore();

#ifdef VULKAN_WSI_TIMING_DEBUG
		auto acquire_start = Util::get_current_time_nsecs();
#endif

		auto acquire_ts = device->write_calibrated_timestamp();
		result = table->vkAcquireNextImageKHR(context->get_device(), swapchain, UINT64_MAX, acquire->get_semaphore(),
		                                      VK_NULL_HANDLE, &swapchain_index);
		device->register_time_interval("WSI", std::move(acquire_ts), device->write_calibrated_timestamp(), "acquire");

#if defined(ANDROID)
		// Android 10 can return suboptimal here, only because of pre-transform.
		// We don't care about that, and treat this as success.
		if (result == VK_SUBOPTIMAL_KHR && !support_prerotate)
			result = VK_SUCCESS;
#endif

		if (result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
		{
			LOGE("Lost exclusive full-screen ...\n");
		}

#ifdef VULKAN_WSI_TIMING_DEBUG
		auto acquire_end = Util::get_current_time_nsecs();
		LOGI("vkAcquireNextImageKHR took %.3f ms.\n", (acquire_end - acquire_start) * 1e-6);
#endif

		if (result == VK_SUBOPTIMAL_KHR)
		{
#ifdef VULKAN_DEBUG
			LOGI("AcquireNextImageKHR is suboptimal, will recreate.\n");
#endif
			swapchain_is_suboptimal = true;
		}

		if (result >= 0)
		{
			has_acquired_swapchain_index = true;
			acquire->signal_external();

			wait_swapchain_latency();

			auto frame_time = platform->get_frame_timer().frame();
			auto elapsed_time = platform->get_frame_timer().get_elapsed();

			smooth_frame_time = frame_time;
			smooth_elapsed_time = elapsed_time;

			// Poll after acquire as well for optimal latency.
			platform->poll_input();
			platform->event_frame_tick(frame_time, elapsed_time);

			platform->event_swapchain_index(device.get(), swapchain_index);

			device->set_acquire_semaphore(swapchain_index, acquire);
		}
		else if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
		{
			VK_ASSERT(swapchain_width != 0);
			VK_ASSERT(swapchain_height != 0);

			tear_down_swapchain();

			if (!blocking_init_swapchain(swapchain_width, swapchain_height))
				return false;
			device->init_swapchain(swapchain_images, swapchain_width, swapchain_height,
			                       swapchain_surface_format.format, swapchain_current_prerotate,
			                       current_extra_usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		}
		else
		{
			return false;
		}
	} while (result < 0);
	return true;
}

bool WSI::end_frame()
{
	device->end_frame_context();

	// Take ownership of the release semaphore so that the external user can use it.
	if (frame_is_external)
	{
		// If we didn't render into the swapchain this frame, we will return a blank semaphore.
		external_release = device->consume_release_semaphore();
		VK_ASSERT(!external_release || external_release->is_signalled());
		frame_is_external = false;
	}
	else
	{
		if (!device->swapchain_touched())
			return true;

		has_acquired_swapchain_index = false;

		auto release = device->consume_release_semaphore();
		VK_ASSERT(release);
		VK_ASSERT(release->is_signalled());
		auto release_semaphore = release->get_semaphore();
		VK_ASSERT(release_semaphore != VK_NULL_HANDLE);

		VkResult result = VK_SUCCESS;
		VkPresentInfoKHR info = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
		info.waitSemaphoreCount = 1;
		info.pWaitSemaphores = &release_semaphore;
		info.swapchainCount = 1;
		info.pSwapchains = &swapchain;
		info.pImageIndices = &swapchain_index;
		info.pResults = &result;

		VkPresentIdKHR present_id_info = { VK_STRUCTURE_TYPE_PRESENT_ID_KHR };
		if (device->get_device_features().present_id_features.presentId)
		{
			present_id_info.swapchainCount = 1;
			present_id_info.pPresentIds = &present_id;
			present_id++;
			present_id_info.pNext = info.pNext;
			info.pNext = &present_id_info;
		}

#ifdef VULKAN_WSI_TIMING_DEBUG
		auto present_start = Util::get_current_time_nsecs();
#endif

		auto present_ts = device->write_calibrated_timestamp();

#if defined(ANDROID) && defined(HAVE_SWAPPY)
		VkResult overall = SwappyVk_queuePresent(device->get_current_present_queue(), &info);
#else
		VkResult overall = table->vkQueuePresentKHR(device->get_current_present_queue(), &info);
#endif

		device->register_time_interval("WSI", std::move(present_ts), device->write_calibrated_timestamp(), "present");

#if defined(ANDROID)
		// Android 10 can return suboptimal here, only because of pre-transform.
		// We don't care about that, and treat this as success.
		if (overall == VK_SUBOPTIMAL_KHR && !support_prerotate)
			overall = VK_SUCCESS;
		if (result == VK_SUBOPTIMAL_KHR && !support_prerotate)
			result = VK_SUCCESS;
#endif

		if (overall == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT ||
		    result == VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT)
		{
			LOGE("Lost exclusive full-screen ...\n");
		}

#ifdef VULKAN_WSI_TIMING_DEBUG
		auto present_end = Util::get_current_time_nsecs();
		LOGI("vkQueuePresentKHR took %.3f ms.\n", (present_end - present_start) * 1e-6);
#endif

		// The presentID only seems to get updated if QueuePresent returns success.
		// This makes sense I guess. Record the latest present ID which was successfully presented
		// so we don't risk deadlock.
		if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) &&
		    device->get_device_features().present_id_features.presentId)
		{
			present_last_id = present_id;
		}

		if (overall == VK_SUBOPTIMAL_KHR || result == VK_SUBOPTIMAL_KHR)
		{
#ifdef VULKAN_DEBUG
			LOGI("QueuePresent is suboptimal, will recreate.\n");
#endif
			swapchain_is_suboptimal = true;
		}

		if (overall < 0 || result < 0)
		{
			LOGE("vkQueuePresentKHR failed.\n");
			tear_down_swapchain();
			return false;
		}
		else
		{
			release->wait_external();
			// Cannot release the WSI wait semaphore until we observe that the image has been
			// waited on again.
			release_semaphores[swapchain_index] = release;
		}

		// Re-init swapchain.
		if (present_mode != current_present_mode || backbuffer_format != current_backbuffer_format ||
		    extra_usage != current_extra_usage)
		{
			current_present_mode = present_mode;
			current_backbuffer_format = backbuffer_format;
			current_extra_usage = extra_usage;
			update_framebuffer(swapchain_width, swapchain_height);
		}
	}

	return true;
}

void WSI::update_framebuffer(unsigned width, unsigned height)
{
	if (context && device)
	{
		drain_swapchain();
		if (blocking_init_swapchain(width, height))
		{
			device->init_swapchain(swapchain_images, swapchain_width, swapchain_height, swapchain_surface_format.format,
			                       swapchain_current_prerotate,
			                       current_extra_usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
		}
	}

	if (platform)
		platform->notify_current_swapchain_dimensions(swapchain_width, swapchain_height);
}

void WSI::set_present_mode(PresentMode mode)
{
	present_mode = mode;
	if (!has_acquired_swapchain_index && present_mode != current_present_mode)
	{
		current_present_mode = present_mode;
		update_framebuffer(swapchain_width, swapchain_height);
	}
}

void WSI::set_extra_usage_flags(VkImageUsageFlags usage)
{
	extra_usage = usage;
	if (!has_acquired_swapchain_index && extra_usage != current_extra_usage)
	{
		current_extra_usage = extra_usage;
		update_framebuffer(swapchain_width, swapchain_height);
	}
}

void WSI::set_backbuffer_format(BackbufferFormat format)
{
	backbuffer_format = format;
	if (!has_acquired_swapchain_index && backbuffer_format != current_backbuffer_format)
	{
		current_backbuffer_format = backbuffer_format;
		update_framebuffer(swapchain_width, swapchain_height);
	}
}

void WSI::set_backbuffer_srgb(bool enable)
{
	set_backbuffer_format(enable ? BackbufferFormat::sRGB : BackbufferFormat::UNORM);
}

void WSI::teardown()
{
	if (platform)
		platform->release_resources();

	if (context)
		tear_down_swapchain();

	if (surface != VK_NULL_HANDLE)
	{
		platform->destroy_surface(context->get_instance(), surface);
		surface = VK_NULL_HANDLE;
	}

	if (platform)
		platform->event_device_destroyed();
	external_release.reset();
	external_acquire.reset();
	external_swapchain_images.clear();
	device.reset();
	context.reset();
}

bool WSI::blocking_init_swapchain(unsigned width, unsigned height)
{
	SwapchainError err;
	unsigned retry_counter = 0;
	do
	{
		swapchain_aspect_ratio = platform->get_aspect_ratio();
		err = init_swapchain(width, height);

		if (err != SwapchainError::None)
			platform->notify_current_swapchain_dimensions(0, 0);

		if (err == SwapchainError::Error)
		{
			if (++retry_counter > 3)
				return false;

			// Try to not reuse the swapchain.
			tear_down_swapchain();
		}
		else if (err == SwapchainError::NoSurface)
		{
			LOGW("WSI cannot make forward progress due to minimization, blocking ...\n");
			platform->block_until_wsi_forward_progress(*this);
			LOGW("Woke up!\n");
		}
	} while (err != SwapchainError::None);

	return swapchain != VK_NULL_HANDLE;
}

VkSurfaceFormatKHR WSI::find_suitable_present_format(const std::vector<VkSurfaceFormatKHR> &formats, BackbufferFormat desired_format) const
{
	size_t format_count = formats.size();
	VkSurfaceFormatKHR format = { VK_FORMAT_UNDEFINED };

	if (desired_format == BackbufferFormat::HDR10 && !device->get_device_features().supports_hdr_metadata)
	{
		LOGW("VK_EXT_hdr_metadata is not supported, ignoring HDR10.\n");
		return format;
	}

	VkFormatFeatureFlags features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
	                                VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
	if ((current_extra_usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0)
		features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;

	if (format_count == 0)
	{
		LOGE("Surface has no formats?\n");
		return format;
	}

	for (size_t i = 0; i < format_count; i++)
	{
		if (!device->image_format_is_supported(formats[i].format, features))
			continue;

		if (desired_format == BackbufferFormat::HDR10)
		{
			if (formats[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
			    (formats[i].format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
			     formats[i].format == VK_FORMAT_A2R10G10B10_UNORM_PACK32))
			{
				format = formats[i];
				break;
			}
		}
		else if (desired_format == BackbufferFormat::sRGB)
		{
			if (formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
			    (formats[i].format == VK_FORMAT_R8G8B8A8_SRGB ||
			     formats[i].format == VK_FORMAT_B8G8R8A8_SRGB ||
			     formats[i].format == VK_FORMAT_A8B8G8R8_SRGB_PACK32))
			{
				format = formats[i];
				break;
			}
		}
		else
		{
			if (formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
			    (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM ||
			     formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
			     formats[i].format == VK_FORMAT_A8B8G8R8_UNORM_PACK32))
			{
				format = formats[i];
				break;
			}
		}
	}

	return format;
}

WSI::SwapchainError WSI::init_swapchain(unsigned width, unsigned height)
{
	if (surface == VK_NULL_HANDLE)
	{
		LOGE("Cannot create swapchain with surface == VK_NULL_HANDLE.\n");
		return SwapchainError::Error;
	}

	VkSurfaceCapabilitiesKHR surface_properties;
	VkPhysicalDeviceSurfaceInfo2KHR surface_info = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR };
	surface_info.surface = surface;
	bool use_surface_info = device->get_device_features().supports_surface_capabilities2;
	bool use_application_controlled_exclusive_fullscreen = false;

#ifdef _WIN32
	VkSurfaceFullScreenExclusiveInfoEXT exclusive_info = { VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT };
	VkSurfaceFullScreenExclusiveWin32InfoEXT exclusive_info_win32 = { VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT };

	HMONITOR monitor = reinterpret_cast<HMONITOR>(platform->get_fullscreen_monitor());
	if (!device->get_device_features().supports_full_screen_exclusive)
		monitor = nullptr;

	surface_info.pNext = &exclusive_info;
	if (monitor != nullptr)
	{
		exclusive_info.pNext = &exclusive_info_win32;
		exclusive_info_win32.hmonitor = monitor;
		LOGI("Win32: Got a full-screen monitor.\n");
	}
	else
		LOGI("Win32: Not running full-screen.\n");

	const char *exclusive = getenv("GRANITE_EXCLUSIVE_FULL_SCREEN");
	bool prefer_exclusive = exclusive && strtoul(exclusive, nullptr, 0) != 0;

	if (device->get_device_features().driver_properties.driverID == VK_DRIVER_ID_AMD_PROPRIETARY_KHR &&
	    current_backbuffer_format == BackbufferFormat::HDR10)
	{
		LOGI("HDR requested on AMD Windows. Forcing exclusive fullscreen, or HDR will not work properly.\n");
		prefer_exclusive = true;
	}

	if (prefer_exclusive)
	{
		LOGI("Win32: Opting in to exclusive full-screen!\n");
		exclusive_info.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT;
	}
	else
	{
		LOGI("Win32: Opting out of exclusive full-screen!\n");
		exclusive_info.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
	}
#endif

	auto gpu = context->get_gpu();
	if (use_surface_info)
	{
		VkSurfaceCapabilities2KHR surface_capabilities2 = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR };

#ifdef _WIN32
		VkSurfaceCapabilitiesFullScreenExclusiveEXT capability_full_screen_exclusive = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT };
		if (device->get_device_features().supports_full_screen_exclusive && exclusive_info_win32.hmonitor)
		{
			surface_capabilities2.pNext = &capability_full_screen_exclusive;
			capability_full_screen_exclusive.pNext = &exclusive_info_win32;
		}
#endif

		if (vkGetPhysicalDeviceSurfaceCapabilities2KHR(gpu, &surface_info, &surface_capabilities2) != VK_SUCCESS)
			return SwapchainError::Error;

		surface_properties = surface_capabilities2.surfaceCapabilities;

#ifdef _WIN32
		if (capability_full_screen_exclusive.fullScreenExclusiveSupported)
			LOGI("Surface could support app-controlled exclusive fullscreen.\n");

		use_application_controlled_exclusive_fullscreen = exclusive_info.fullScreenExclusive == VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT &&
		                                                  capability_full_screen_exclusive.fullScreenExclusiveSupported == VK_TRUE;
		if (monitor == nullptr)
			use_application_controlled_exclusive_fullscreen = false;
#endif

		if (use_application_controlled_exclusive_fullscreen)
		{
			LOGI("Using app-controlled exclusive fullscreen.\n");
#ifdef _WIN32
			exclusive_info.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
			exclusive_info.pNext = &exclusive_info_win32;
#endif
		}
		else
		{
			LOGI("Not using app-controlled exclusive fullscreen.\n");
		}
	}
	else
	{
		if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &surface_properties) != VK_SUCCESS)
			return SwapchainError::Error;
	}

	// Happens on Windows when you minimize a window.
	if (surface_properties.maxImageExtent.width == 0 && surface_properties.maxImageExtent.height == 0)
		return SwapchainError::NoSurface;

	uint32_t format_count;
	std::vector<VkSurfaceFormatKHR> formats;

	if (use_surface_info)
	{
		if (vkGetPhysicalDeviceSurfaceFormats2KHR(gpu, &surface_info, &format_count, nullptr) != VK_SUCCESS)
			return SwapchainError::Error;

		std::vector<VkSurfaceFormat2KHR> formats2(format_count);

		for (auto &f : formats2)
		{
			f = {};
			f.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
		}

		if (vkGetPhysicalDeviceSurfaceFormats2KHR(gpu, &surface_info, &format_count, formats2.data()) != VK_SUCCESS)
			return SwapchainError::Error;

		formats.reserve(format_count);
		for (auto &f : formats2)
			formats.push_back(f.surfaceFormat);
	}
	else
	{
		if (vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &format_count, nullptr) != VK_SUCCESS)
			return SwapchainError::Error;
		formats.resize(format_count);
		if (vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &format_count, formats.data()) != VK_SUCCESS)
			return SwapchainError::Error;
	}

	if (current_extra_usage && support_prerotate)
	{
		LOGW("Disabling prerotate support due to extra usage flags in swapchain.\n");
		support_prerotate = false;
	}

	if (current_extra_usage & ~surface_properties.supportedUsageFlags)
	{
		LOGW("Attempting to use unsupported usage flags 0x%x for swapchain.\n", current_extra_usage);
		current_extra_usage &= surface_properties.supportedUsageFlags;
		extra_usage = current_extra_usage;
	}

	auto attempt_backbuffer_format = current_backbuffer_format;
	auto surface_format = find_suitable_present_format(formats, attempt_backbuffer_format);

	if (surface_format.format == VK_FORMAT_UNDEFINED && attempt_backbuffer_format == BackbufferFormat::HDR10)
	{
		LOGW("Could not find suitable present format for HDR10. Attempting fallback to sRGB.\n");
		attempt_backbuffer_format = BackbufferFormat::sRGB;
		surface_format = find_suitable_present_format(formats, attempt_backbuffer_format);
	}

	if (surface_format.format == VK_FORMAT_UNDEFINED)
	{
		LOGW("Could not find supported format for swapchain usage flags 0x%x.\n", current_extra_usage);
		current_extra_usage = 0;
		extra_usage = 0;
		surface_format = find_suitable_present_format(formats, attempt_backbuffer_format);
	}

	static const char *transform_names[] = {
		"IDENTITY_BIT_KHR",
		"ROTATE_90_BIT_KHR",
		"ROTATE_180_BIT_KHR",
		"ROTATE_270_BIT_KHR",
		"HORIZONTAL_MIRROR_BIT_KHR",
		"HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR",
		"HORIZONTAL_MIRROR_ROTATE_180_BIT_KHR",
		"HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR",
		"INHERIT_BIT_KHR",
	};

	LOGI("Current transform is enum 0x%x.\n", unsigned(surface_properties.currentTransform));

	for (unsigned i = 0; i <= 8; i++)
	{
		if (surface_properties.supportedTransforms & (1u << i))
			LOGI("Supported transform 0x%x: %s.\n", 1u << i, transform_names[i]);
	}

	VkSurfaceTransformFlagBitsKHR pre_transform;
	if (!support_prerotate && (surface_properties.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0)
		pre_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	else
	{
		// Only attempt to use prerotate if we can deal with it purely using a XY clip fixup.
		// For horizontal flip we need to start flipping front-face as well ...
		if ((surface_properties.currentTransform & (
				VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR |
				VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR |
				VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)) != 0)
			pre_transform = surface_properties.currentTransform;
		else
			pre_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	}

	if (pre_transform != surface_properties.currentTransform)
	{
		LOGW("surfaceTransform (0x%x) != currentTransform (0x%u). Might get performance penalty.\n",
		     unsigned(pre_transform), unsigned(surface_properties.currentTransform));
	}

	swapchain_current_prerotate = pre_transform;

	VkExtent2D swapchain_size;
	LOGI("Swapchain current extent: %d x %d\n",
	     int(surface_properties.currentExtent.width),
	     int(surface_properties.currentExtent.height));

	if (width == 0)
	{
		if (surface_properties.currentExtent.width != ~0u)
			width = surface_properties.currentExtent.width;
		else
			width = 1280;
		LOGI("Auto selected width = %u.\n", width);
	}

	if (height == 0)
	{
		if (surface_properties.currentExtent.height != ~0u)
			height = surface_properties.currentExtent.height;
		else
			height = 720;
		LOGI("Auto selected height = %u.\n", height);
	}

	// Try to match the swapchain size up with what we expect w.r.t. aspect ratio.
	float target_aspect_ratio = float(width) / float(height);
	if ((swapchain_aspect_ratio > 1.0f && target_aspect_ratio < 1.0f) ||
	    (swapchain_aspect_ratio < 1.0f && target_aspect_ratio > 1.0f))
	{
		std::swap(width, height);
	}

	// If we are using pre-rotate of 90 or 270 degrees, we need to flip width and height again.
	if (swapchain_current_prerotate &
	    (VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR |
	     VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR |
	     VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR))
	{
		std::swap(width, height);
	}

	// Clamp the target width, height to boundaries.
	swapchain_size.width =
	    std::max(std::min(width, surface_properties.maxImageExtent.width), surface_properties.minImageExtent.width);
	swapchain_size.height =
	    std::max(std::min(height, surface_properties.maxImageExtent.height), surface_properties.minImageExtent.height);

	uint32_t num_present_modes;

	std::vector<VkPresentModeKHR> present_modes;

#ifdef _WIN32
	if (use_surface_info && device->get_device_features().supports_full_screen_exclusive)
	{
		if (vkGetPhysicalDeviceSurfacePresentModes2EXT(gpu, &surface_info, &num_present_modes, nullptr) != VK_SUCCESS)
			return SwapchainError::Error;
		present_modes.resize(num_present_modes);
		if (vkGetPhysicalDeviceSurfacePresentModes2EXT(gpu, &surface_info, &num_present_modes, present_modes.data()) !=
		    VK_SUCCESS)
			return SwapchainError::Error;
	}
	else
#endif
	{
		if (vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &num_present_modes, nullptr) != VK_SUCCESS)
			return SwapchainError::Error;
		present_modes.resize(num_present_modes);
		if (vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, surface, &num_present_modes, present_modes.data()) != VK_SUCCESS)
			return SwapchainError::Error;
	}

	VkPresentModeKHR swapchain_present_mode = VK_PRESENT_MODE_FIFO_KHR;
	bool use_vsync = current_present_mode == PresentMode::SyncToVBlank;
	if (!use_vsync)
	{
		bool allow_mailbox = current_present_mode != PresentMode::UnlockedForceTearing;
		bool allow_immediate = current_present_mode != PresentMode::UnlockedNoTearing;

#ifdef _WIN32
		if (device->get_gpu_properties().vendorID == VENDOR_ID_NVIDIA)
		{
			// If we're trying to go exclusive full-screen,
			// we need to ban certain types of present modes which apparently do not work as we expect.
			if (use_application_controlled_exclusive_fullscreen)
				allow_mailbox = false;
			else
				allow_immediate = false;
		}
#endif

		for (uint32_t i = 0; i < num_present_modes; i++)
		{
			if ((allow_immediate && present_modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) ||
			    (allow_mailbox && present_modes[i] == VK_PRESENT_MODE_MAILBOX_KHR))
			{
				swapchain_present_mode = present_modes[i];
				break;
			}
		}
	}

	uint32_t desired_swapchain_images = 3;
	{
		const char *num_images = getenv("GRANITE_VULKAN_SWAPCHAIN_IMAGES");
		if (num_images)
			desired_swapchain_images = uint32_t(strtoul(num_images, nullptr, 0));
	}

	LOGI("Targeting %u swapchain images.\n", desired_swapchain_images);

	if (desired_swapchain_images < surface_properties.minImageCount)
		desired_swapchain_images = surface_properties.minImageCount;

	if ((surface_properties.maxImageCount > 0) && (desired_swapchain_images > surface_properties.maxImageCount))
		desired_swapchain_images = surface_properties.maxImageCount;

	VkCompositeAlphaFlagBitsKHR composite_mode = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (surface_properties.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
		composite_mode = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	else if (surface_properties.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
		composite_mode = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	else if (surface_properties.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)
		composite_mode = VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
	else if (surface_properties.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
		composite_mode = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
	else
		LOGW("No sensible composite mode supported?\n");

	VkSwapchainKHR old_swapchain = swapchain;

	VkSwapchainCreateInfoKHR info = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	info.surface = surface;
	info.minImageCount = desired_swapchain_images;
	info.imageFormat = surface_format.format;
	info.imageColorSpace = surface_format.colorSpace;
	info.imageExtent.width = swapchain_size.width;
	info.imageExtent.height = swapchain_size.height;
	info.imageArrayLayers = 1;
	info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | current_extra_usage;
	info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.preTransform = pre_transform;
	info.compositeAlpha = composite_mode;
	info.presentMode = swapchain_present_mode;
	info.clipped = VK_TRUE;
	info.oldSwapchain = old_swapchain;

	if (device->get_device_features().present_wait_features.presentWait &&
	    old_swapchain != VK_NULL_HANDLE && present_last_id)
	{
		table->vkWaitForPresentKHR(context->get_device(), old_swapchain, present_last_id, UINT64_MAX);
	}

#ifdef _WIN32
	if (device->get_device_features().supports_full_screen_exclusive)
		info.pNext = &exclusive_info;
#endif

	platform->event_swapchain_destroyed();
	auto res = table->vkCreateSwapchainKHR(context->get_device(), &info, nullptr, &swapchain);
	if (old_swapchain != VK_NULL_HANDLE)
		table->vkDestroySwapchainKHR(context->get_device(), old_swapchain, nullptr);
	has_acquired_swapchain_index = false;
	present_id = 0;
	present_last_id = 0;

#ifdef _WIN32
	if (use_application_controlled_exclusive_fullscreen)
	{
		bool success = vkAcquireFullScreenExclusiveModeEXT(context->get_device(), swapchain) == VK_SUCCESS;
		if (success)
			LOGI("Successfully acquired exclusive full-screen.\n");
		else
			LOGI("Failed to acquire exclusive full-screen. Using borderless windowed.\n");
	}
#endif

	if (res != VK_SUCCESS)
	{
		LOGE("Failed to create swapchain (code: %d)\n", int(res));
		swapchain = VK_NULL_HANDLE;
		return SwapchainError::Error;
	}

	swapchain_width = swapchain_size.width;
	swapchain_height = swapchain_size.height;
	swapchain_surface_format = surface_format;
	swapchain_is_suboptimal = false;

	LOGI("Created swapchain %u x %u (fmt: %u, transform: %u).\n", swapchain_width, swapchain_height,
	     unsigned(swapchain_surface_format.format), unsigned(swapchain_current_prerotate));

	uint32_t image_count;
	if (table->vkGetSwapchainImagesKHR(context->get_device(), swapchain, &image_count, nullptr) != VK_SUCCESS)
		return SwapchainError::Error;
	swapchain_images.resize(image_count);
	release_semaphores.resize(image_count);
	if (table->vkGetSwapchainImagesKHR(context->get_device(), swapchain, &image_count, swapchain_images.data()) != VK_SUCCESS)
		return SwapchainError::Error;

	LOGI("Got %u swapchain images.\n", image_count);

	platform->event_swapchain_created(device.get(), swapchain, swapchain_width, swapchain_height,
	                                  swapchain_aspect_ratio, image_count,
	                                  swapchain_surface_format.format,
	                                  swapchain_surface_format.colorSpace,
	                                  swapchain_current_prerotate);

	if (swapchain_surface_format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
		table->vkSetHdrMetadataEXT(device->get_device(), 1, &swapchain, &hdr_metadata);

	return SwapchainError::None;
}

void WSI::set_support_prerotate(bool enable)
{
	support_prerotate = enable;
}

VkSurfaceTransformFlagBitsKHR WSI::get_current_prerotate() const
{
	return swapchain_current_prerotate;
}

WSI::~WSI()
{
	teardown();
}

void WSIPlatform::event_device_created(Device *) {}
void WSIPlatform::event_device_destroyed() {}
void WSIPlatform::event_swapchain_created(Device *, VkSwapchainKHR, unsigned, unsigned, float, size_t,
                                          VkFormat, VkColorSpaceKHR,
                                          VkSurfaceTransformFlagBitsKHR) {}
void WSIPlatform::event_swapchain_destroyed() {}
void WSIPlatform::event_frame_tick(double, double) {}
void WSIPlatform::event_swapchain_index(Device *, unsigned) {}
}
