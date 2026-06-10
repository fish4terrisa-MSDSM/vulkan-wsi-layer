#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <thread>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/dma-heap.h>

#include <util/timed_semaphore.hpp>
#include <vulkan/vulkan_core.h>

#include <xcb/dri3.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>

#include "swapchain.hpp"
#include "util/log.hpp"
#include "util/macros.hpp"
#include "wsi/external_memory.hpp"
#include "wsi/swapchain_base.hpp"
#include "wsi/extensions/present_id.hpp"
#include "dri3_presenter.hpp"
#include "util/drm/drm_utils.hpp"

namespace wsi
{
namespace x11
{

static int alloc_dma_heap(size_t size) {
    int fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        WSI_LOG_ERROR("Failed to open /dev/dma_heap/system");
        return -1;
    }
    struct dma_heap_allocation_data heap_data = {
        .len = size,
        .fd_flags = O_RDWR | O_CLOEXEC,
    };
    int ret = ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &heap_data);
    close(fd);
    if (ret != 0) {
        WSI_LOG_ERROR("DMA_HEAP_IOCTL_ALLOC failed");
        return -1;
    }
    return heap_data.fd;
}

swapchain::swapchain(layer::device_private_data &dev_data, const VkAllocationCallbacks *pAllocator,
                     surface &wsi_surface)
   : swapchain_base(dev_data, pAllocator)
   , m_connection(wsi_surface.get_connection())
   , m_window(wsi_surface.get_window())
   , m_wsi_surface(&wsi_surface)
   , m_thread_status_lock()
   , m_thread_status_cond()
{
   m_image_create_info.format = VK_FORMAT_UNDEFINED;
}

swapchain::~swapchain()
{
   {
      auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);
      m_present_event_thread_run = false;
      m_thread_status_cond.notify_all();
   }

   if (m_present_event_thread.joinable())
   {
      m_present_event_thread.join();
   }

   m_page_flip_thread_run = false;
   m_page_flip_semaphore.post();

   teardown();
}

VkResult swapchain::init_platform(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info,
                                  bool &use_presentation_thread)
{
   UNUSED(device);
   use_presentation_thread = true;

   auto dri3 = std::make_unique<dri3_presenter>();
   if (!dri3->is_available(m_connection)) {
      WSI_LOG_ERROR("x11 swapchain: DRI3 is not available");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   if (dri3->init(m_connection, m_window, m_wsi_surface) != VK_SUCCESS) {
      WSI_LOG_ERROR("x11 swapchain: DRI3 initialization failed");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   m_dri3_presenter = std::move(dri3);
   WSI_LOG_INFO("x11 swapchain: Successfully initialized termux-x11 DRI3 presenter");

   try
   {
      m_present_event_thread = std::thread(&swapchain::present_event_thread, this);
   }
   catch (...)
   {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   return VK_SUCCESS;
}

VkResult swapchain::create_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image)
{
   auto image_data = m_allocator.create<x11_image_data>(1, m_device, m_allocator);
   if (image_data == nullptr)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   image.data = image_data;
   image_data->device = m_device;
   image_data->device_data = &m_device_data;

   m_image_create_info = image_create_info;
   return VK_SUCCESS;
}

VkResult swapchain::allocate_and_bind_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image)
{
   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
   image.status = swapchain_image::FREE;
   auto image_data = static_cast<x11_image_data *>(image.data);
   image_status_lock.unlock();

   VkExternalMemoryImageCreateInfoKHR ext_info = {};
   ext_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_KHR;
   ext_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   
   image_create_info.pNext = &ext_info;
   
   // Termux-X11 explicitly requires linear backing memory since it mmap()s the FD 
   // and calls glTexSubImage2D on the CPU.
   image_create_info.tiling = VK_IMAGE_TILING_LINEAR;

   VkResult res = m_device_data.disp.CreateImage(m_device, &image_create_info, get_allocation_callbacks(), &image.image);
   if (res != VK_SUCCESS) {
      WSI_LOG_ERROR("Failed to create VK_IMAGE_TILING_LINEAR swapchain image");
      return res;
   }

   VkMemoryRequirements reqs;
   m_device_data.disp.GetImageMemoryRequirements(m_device, image.image, &reqs);

   // Direct dma_heap allocation ensuring mapped capability
   int dma_buf_fd = alloc_dma_heap(reqs.size);
   if (dma_buf_fd < 0) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   // Register FD with external memory wrapper to bind
   image_data->external_mem.set_buffer_fds({dma_buf_fd, -1, -1, -1});
   image_data->external_mem.set_num_memories(1);
   image_data->external_mem.set_format_info(false, 1);
   image_data->external_mem.set_memory_handle_type(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

   res = image_data->external_mem.import_memory_and_bind_swapchain_image(image.image);
   if (res != VK_SUCCESS) {
      return res;
   }

   // Query Vulkan layout to get exactly what the driver chose for rowPitch.
   // Because the tiling is LINEAR, querying VK_IMAGE_ASPECT_COLOR_BIT works reliably.
   VkImageSubresource subres = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
   VkSubresourceLayout layout;
   m_device_data.disp.GetImageSubresourceLayout(m_device, image.image, &subres, &layout);

   uint32_t width = image_create_info.extent.width;
   uint32_t height = image_create_info.extent.height;
   uint32_t stride = layout.rowPitch;
   uint32_t fourcc = util::drm::vk_to_drm_format(image_create_info.format);
   if (!fourcc) fourcc = 0x34325241; // DRM_FORMAT_ARGB8888 fallback

   int depth = 24;
   uint32_t dummy_w, dummy_h;
   m_wsi_surface->get_size_and_depth(&dummy_w, &dummy_h, &depth);

   // 1274 is Termux-X11's custom internal modifier for 'RAW_MMAPPABLE_FD'.
   // This guarantees that termux-x11 accepts the FD and mmap()s it correctly.
   res = m_dri3_presenter->create_image_resources(image_data, width, height, depth, stride, fourcc, 1274);
   if (res != VK_SUCCESS) {
       return res;
   }

   auto present_fence = sync_fd_fence_sync::create(m_device_data);
   if (!present_fence.has_value()) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   image_data->present_fence = std::move(present_fence.value());

   return VK_SUCCESS;
}

void swapchain::present_event_thread()
{
   auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);
   m_present_event_thread_run = true;

   while (m_present_event_thread_run)
   {
      if (error_has_occured()) break;

      thread_status_lock.unlock();

      xcb_generic_event_t *event;
      while ((event = xcb_poll_for_event(m_connection)) != nullptr)
      {
         // We let X11 do its thing in the background. Because termux-x11 uses XCB_PRESENT_OPTION_COPY, 
         // it doesn't stall application flow. We can discard events we don't care about.
         free(event);
      }

      thread_status_lock.lock();
      m_thread_status_cond.wait_for(thread_status_lock, std::chrono::milliseconds(8));
   }

   m_present_event_thread_run = false;
   m_thread_status_cond.notify_all();
}

void swapchain::present_image(const pending_present_request &pending_present)
{
   auto image_data = reinterpret_cast<x11_image_data *>(m_swapchain_images[pending_present.image_index].data);
   
   // Send to termux-x11 directly. XCB_PRESENT_OPTION_COPY makes the server copy immediately.
   VkResult present_result = m_dri3_presenter->present_image(image_data, 0);

   if (present_result != VK_SUCCESS) {
      WSI_LOG_ERROR("Failed to present image using DRI3: %d", present_result);
   }

   if (m_device_data.is_present_id_enabled())
   {
      auto *ext = get_swapchain_extension<wsi_ext_present_id>(true);
      ext->set_present_id(pending_present.present_id);
   }

   // Because of OPTION_COPY, the image is immediately available for reuse by Vulkan.
   unpresent_image(pending_present.image_index);
}

bool swapchain::free_image_found()
{
   for (auto &img : m_swapchain_images)
   {
      if (img.status == swapchain_image::FREE)
      {
         return true;
      }
   }
   return false;
}

VkResult swapchain::get_free_buffer(uint64_t *timeout)
{
   auto thread_status_lock = std::unique_lock<std::mutex>(m_thread_status_lock);

   if (*timeout == 0)
   {
      return free_image_found() ? VK_SUCCESS : VK_NOT_READY;
   }
   else if (*timeout == UINT64_MAX)
   {
      while (!free_image_found())
      {
         if (!m_present_event_thread_run)
            return VK_ERROR_OUT_OF_DATE_KHR;

         m_thread_status_cond.wait(thread_status_lock);
      }
   }
   else
   {
      auto time_point = std::chrono::high_resolution_clock::now() + std::chrono::nanoseconds(*timeout);

      while (!free_image_found())
      {
         if (!m_present_event_thread_run)
            return VK_ERROR_OUT_OF_DATE_KHR;

         if (m_thread_status_cond.wait_until(thread_status_lock, time_point) == std::cv_status::timeout)
         {
            return VK_TIMEOUT;
         }
      }
   }

   *timeout = 0;
   return VK_SUCCESS;
}

void swapchain::destroy_image(wsi::swapchain_image &image)
{
   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
   if (image.status != wsi::swapchain_image::INVALID)
   {
      if (image.image != VK_NULL_HANDLE)
      {
         m_device_data.disp.DestroyImage(m_device, image.image, get_allocation_callbacks());
         image.image = VK_NULL_HANDLE;
      }

      image.status = wsi::swapchain_image::INVALID;
   }

   image_status_lock.unlock();

   if (image.data != nullptr)
   {
      auto data = reinterpret_cast<x11_image_data *>(image.data);

      if (data != nullptr && m_dri3_presenter)
      {
         m_dri3_presenter->destroy_image_resources(data);
      }

      m_allocator.destroy(1, data);
      image.data = nullptr;
   }
}

VkResult swapchain::image_set_present_payload(swapchain_image &image, VkQueue queue,
                                              const queue_submit_semaphores &semaphores, const void *submission_pnext)
{
   auto data = reinterpret_cast<x11_image_data *>(image.data);
   return data->present_fence.set_payload(queue, semaphores, submission_pnext);
}

VkResult swapchain::image_wait_present(swapchain_image &image, uint64_t timeout)
{
   auto data = reinterpret_cast<x11_image_data *>(image.data);
   return data->present_fence.wait_payload(timeout);
}

VkResult swapchain::bind_swapchain_image(VkDevice &device, const VkBindImageMemoryInfo *bind_image_mem_info,
                                         const VkBindImageMemorySwapchainInfoKHR *bind_sc_info)
{
   UNUSED(device);
   const wsi::swapchain_image &swapchain_image = m_swapchain_images[bind_sc_info->imageIndex];
   auto image_data = reinterpret_cast<x11_image_data *>(swapchain_image.data);
   return image_data->external_mem.bind_swapchain_image_memory(bind_image_mem_info->image);
}

VkResult swapchain::add_required_extensions(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info)
{
   UNUSED(device);
   UNUSED(swapchain_create_info);

   if (m_device_data.is_present_id_enabled())
   {
      if (!add_swapchain_extension(m_allocator.make_unique<wsi_ext_present_id>()))
      {
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }
   }

   return VK_SUCCESS;
}

} /* namespace x11 */
} /* namespace wsi */
