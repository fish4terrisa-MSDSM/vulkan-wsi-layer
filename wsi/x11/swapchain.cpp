#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/dma-heap.h>
#include <algorithm>
#include <poll.h>

#include <util/timed_semaphore.hpp>
#include <vulkan/vulkan_core.h>

#include <xcb/dri3.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/present.h>

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

    /* DMA-BUF system heaps on Android strictly require the allocation length
     * to be page-aligned. Unaligned sizes trigger EINVAL inside the ioctl. */
    size_t page_size = 4096;
    long sys_page_size = sysconf(_SC_PAGE_SIZE);
    if (sys_page_size > 0) {
        page_size = (size_t)sys_page_size;
    }
    size_t aligned_size = (size + page_size - 1) & ~(page_size - 1);

    struct dma_heap_allocation_data heap_data = {};
    heap_data.len = aligned_size;
    heap_data.fd_flags = O_RDWR | O_CLOEXEC;

    int ret = ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &heap_data);
    close(fd);
    if (ret != 0) {
        WSI_LOG_ERROR("DMA_HEAP_IOCTL_ALLOC failed, size: %zu, aligned: %zu, errno: %d", size, aligned_size, errno);
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
{
   m_image_create_info.format = VK_FORMAT_UNDEFINED;
}

swapchain::~swapchain()
{
   // Disconnect swapchain reference from the detached thread to prevent use-after-free
   if (m_thread_ctx) {
      std::unique_lock<std::recursive_mutex> lock(m_thread_ctx->mutex);
      m_thread_ctx->run = false;
      m_thread_ctx->sc = nullptr;
   }

   m_present_event_thread_run = false;
   {
      std::unique_lock<std::mutex> lock(m_present_mutex);
      m_outstanding_pixmaps = 0;
      m_present_cond.notify_all();
   }

   if (m_present_event_thread.joinable())
   {
      // Detach instead of join to safely handle destruction under X11 Display Lock (XLockDisplay)
      m_present_event_thread.detach();
   }

   /* Call the base's teardown */
   teardown();

   {
      std::lock_guard<std::recursive_mutex> xcb_lock(g_xcb_mutex);

      if (m_connection && m_window) {
         xcb_present_notify_msc_checked(m_connection, m_window, 0, 0, 0, 0);
      }

      if (m_event_id && m_window) {
         xcb_present_select_input_checked(m_connection, m_event_id, m_window, XCB_PRESENT_EVENT_MASK_NO_EVENT);
         m_event_id = 0;
      }
      xcb_flush(m_connection);
   }

   if (m_command_pool != VK_NULL_HANDLE) {
      m_device_data.disp.DestroyCommandPool(m_device, m_command_pool, get_allocation_callbacks());
      m_command_pool = VK_NULL_HANDLE;
   }
}

VkResult swapchain::init_platform(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info,
                                  bool &use_presentation_thread)
{
   UNUSED(device);
   UNUSED(swapchain_create_info);
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

   std::lock_guard<std::recursive_mutex> xcb_lock(g_xcb_mutex);

   const xcb_query_extension_reply_t *ext = xcb_get_extension_data(m_connection, &xcb_present_id);
   if (!ext || !ext->present) {
      WSI_LOG_ERROR("x11 swapchain: Present extension not available");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   m_event_id = xcb_generate_id(m_connection);
   xcb_present_select_input(m_connection, m_event_id, m_window, XCB_PRESENT_EVENT_MASK_IDLE_NOTIFY | XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
   m_special_event = xcb_register_for_special_xge(m_connection, &xcb_present_id, m_event_id, nullptr);

   if (!m_special_event) {
      WSI_LOG_ERROR("x11 swapchain: Failed to register for special event");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   WSI_LOG_INFO("x11 swapchain: Successfully initialized termux-x11 DRI3 presenter");

   m_thread_ctx = std::make_shared<event_thread_context>();
   m_thread_ctx->sc = this;
   m_thread_ctx->run = true;

   try
   {
      m_present_event_thread = std::thread(present_event_thread_func, m_thread_ctx, m_connection, m_special_event);
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

   image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
   image_create_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

   image_create_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_ALIAS_BIT;

   VkResult res = m_device_data.disp.CreateImage(m_device, &image_create_info, get_allocation_callbacks(), &image.image);
   if (res != VK_SUCCESS) {
      WSI_LOG_ERROR("Failed to create optimal swapchain image");
      return res;
   }
   image_data->optimal_image = image.image;

   m_image_create_info = image_create_info;
   return VK_SUCCESS;
}

VkResult swapchain::allocate_and_bind_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image)
{
   std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
   image.status = swapchain_image::FREE;
   assert(image.data != nullptr);
   auto image_data = static_cast<x11_image_data *>(image.data);
   image_status_lock.unlock();

   VkMemoryRequirements reqs;
   m_device_data.disp.GetImageMemoryRequirements(m_device, image_data->optimal_image, &reqs);

   VkPhysicalDeviceMemoryProperties mem_props;
   m_device_data.instance_data.disp.GetPhysicalDeviceMemoryProperties(m_device_data.physical_device, &mem_props);
   uint32_t mem_type_idx = UINT32_MAX;
   for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
      if ((reqs.memoryTypeBits & (1 << i)) && 
          (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
         mem_type_idx = i;
         break;
      }
   }
   if (mem_type_idx == UINT32_MAX) {
      for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
         if (reqs.memoryTypeBits & (1 << i)) {
            mem_type_idx = i;
            break;
         }
      }
   }

   VkMemoryAllocateInfo mem_info = {};
   mem_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
   mem_info.allocationSize = reqs.size;
   mem_info.memoryTypeIndex = mem_type_idx;

   VkResult res = m_device_data.disp.AllocateMemory(m_device, &mem_info, get_allocation_callbacks(), &image_data->optimal_memory);
   if (res != VK_SUCCESS) {
      WSI_LOG_ERROR("Failed to allocate memory for optimal image");
      return res;
   }

   res = m_device_data.disp.BindImageMemory(m_device, image_data->optimal_image, image_data->optimal_memory, 0);
   if (res != VK_SUCCESS) {
      WSI_LOG_ERROR("Failed to bind memory to optimal image");
      return res;
   }

   VkImageCreateInfo linear_info = image_create_info;
   linear_info.tiling = VK_IMAGE_TILING_LINEAR;
   linear_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

   VkExternalMemoryImageCreateInfoKHR ext_info = {};
   ext_info.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO_KHR;
   ext_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   ext_info.pNext = linear_info.pNext;
   linear_info.pNext = &ext_info;

   res = m_device_data.disp.CreateImage(m_device, &linear_info, get_allocation_callbacks(), &image_data->linear_image);
   if (res != VK_SUCCESS) {
      WSI_LOG_ERROR("Failed to create linear presentation image");
      return res;
   }

   VkMemoryRequirements linear_reqs;
   m_device_data.disp.GetImageMemoryRequirements(m_device, image_data->linear_image, &linear_reqs);

   int dma_buf_fd = alloc_dma_heap(linear_reqs.size);
   if (dma_buf_fd < 0) {
      WSI_LOG_ERROR("Failed to allocate DMA heap memory");
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   image_data->external_mem.set_buffer_fds({dma_buf_fd, -1, -1, -1});
   image_data->external_mem.set_num_memories(1);
   image_data->external_mem.set_format_info(false, 1);
   image_data->external_mem.set_memory_handle_type(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

   res = image_data->external_mem.import_memory_and_bind_swapchain_image(image_data->linear_image);
   if (res != VK_SUCCESS) {
      WSI_LOG_ERROR("Failed to import memory and bind linear image");
      return res;
   }

   VkImageSubresource subres = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
   VkSubresourceLayout layout;
   m_device_data.disp.GetImageSubresourceLayout(m_device, image_data->linear_image, &subres, &layout);

   uint32_t width = image_create_info.extent.width;
   uint32_t height = image_create_info.extent.height;
   uint32_t stride = layout.rowPitch;
   uint32_t fourcc = util::drm::vk_to_drm_format(image_create_info.format);
   if (!fourcc) fourcc = 0x34325241;

   int depth = 24;
   uint32_t dummy_w, dummy_h;
   m_wsi_surface->get_size_and_depth(&dummy_w, &dummy_h, &depth);

   res = m_dri3_presenter->create_image_resources(image_data, width, height, depth, stride, fourcc, 1274);
   if (res != VK_SUCCESS) {
      WSI_LOG_ERROR("Failed to create DRI3 image resources");
      return res;
   }

   auto present_fence = fence_sync::create(m_device_data);
   if (!present_fence.has_value())
   {
      WSI_LOG_ERROR("Failed to create presentation fence");
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   image_data->present_fence = std::move(present_fence.value());

   if (m_command_pool == VK_NULL_HANDLE) {
      VkCommandPoolCreateInfo pool_info = {};
      pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      pool_info.queueFamilyIndex = 0;
      pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
      res = m_device_data.disp.CreateCommandPool(m_device, &pool_info, get_allocation_callbacks(), &m_command_pool);
      if (res != VK_SUCCESS) {
         WSI_LOG_ERROR("Failed to create command pool");
         return res;
      }
   }

   VkCommandBufferAllocateInfo cmd_alloc = {};
   cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
   cmd_alloc.commandPool = m_command_pool;
   cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
   cmd_alloc.commandBufferCount = 1;
   res = m_device_data.disp.AllocateCommandBuffers(m_device, &cmd_alloc, &image_data->copy_cmd);
   if (res != VK_SUCCESS) {
      WSI_LOG_ERROR("Failed to allocate copy command buffer");
      return res;
}

   VkCommandBufferBeginInfo begin_info = {};
   begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
   res = m_device_data.disp.BeginCommandBuffer(image_data->copy_cmd, &begin_info);
   if (res != VK_SUCCESS) {
      WSI_LOG_ERROR("Failed to begin command buffer");
      return res;
   }

   VkImageMemoryBarrier barrier_src = {};
   barrier_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   barrier_src.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   barrier_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   barrier_src.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
   barrier_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   barrier_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   barrier_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   barrier_src.image = image_data->optimal_image;
   barrier_src.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

   VkImageMemoryBarrier barrier_dst = {};
   barrier_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
   barrier_dst.srcAccessMask = 0;
   barrier_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   barrier_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
   barrier_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   barrier_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   barrier_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
   barrier_dst.image = image_data->linear_image;
   barrier_dst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

   m_device_data.disp.CmdPipelineBarrier(image_data->copy_cmd,
                                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &barrier_src);

   m_device_data.disp.CmdPipelineBarrier(image_data->copy_cmd,
                                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &barrier_dst);

   VkImageCopy copy_region = {};
   copy_region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
   copy_region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
   copy_region.extent = { width, height, 1 };

   m_device_data.disp.CmdCopyImage(image_data->copy_cmd,
                                   image_data->optimal_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   image_data->linear_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   1, &copy_region);

   barrier_src.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   barrier_src.dstAccessMask = 0;
   barrier_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   barrier_src.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

   barrier_dst.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
   barrier_dst.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
   barrier_dst.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   barrier_dst.newLayout = VK_IMAGE_LAYOUT_GENERAL;

   m_device_data.disp.CmdPipelineBarrier(image_data->copy_cmd,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &barrier_src);

   m_device_data.disp.CmdPipelineBarrier(image_data->copy_cmd,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                         0, 0, nullptr, 0, nullptr, 1, &barrier_dst);

   res = m_device_data.disp.EndCommandBuffer(image_data->copy_cmd);
   if (res != VK_SUCCESS) {
      WSI_LOG_ERROR("Failed to end command buffer");
      return res;
   }

   return VK_SUCCESS;
}

void swapchain::present_event_thread_func(std::shared_ptr<event_thread_context> ctx, xcb_connection_t *conn, xcb_special_event_t *special_event)
{
   while (ctx->run)
   {
      xcb_generic_event_t *event = nullptr;
      {
         std::lock_guard<std::recursive_mutex> xcb_lock(g_xcb_mutex);
         event = xcb_poll_for_special_event(conn, special_event);
      }
      if (event) {
         std::unique_lock<std::recursive_mutex> lock(ctx->mutex);
         if (ctx->sc) {
            ctx->sc->process_present_event(event);
         }
         lock.unlock();

         free(event);
         continue;
      }

      struct pollfd pfd = {};
      pfd.fd = xcb_get_file_descriptor(conn);
      pfd.events = POLLIN;
      int ret = poll(&pfd, 1, 10);
      if (ret < 0 && errno == EINTR) {
         continue;
      }

      {
         std::lock_guard<std::recursive_mutex> xcb_lock(g_xcb_mutex);
         if (xcb_connection_has_error(conn)) {
            break;
         }
      }
   }

   {
      std::lock_guard<std::recursive_mutex> xcb_lock(g_xcb_mutex);
      xcb_unregister_for_special_event(conn, special_event);
      xcb_flush(conn);
   }
}

void swapchain::process_present_event(xcb_generic_event_t *event)
{
   auto *ge = (xcb_present_generic_event_t *)event;
   if (ge->evtype == XCB_PRESENT_EVENT_IDLE_NOTIFY) {
      auto *idle = (xcb_present_idle_notify_event_t *)event;

      std::unique_lock<std::recursive_mutex> image_status_lock(m_image_status_mutex);
      for (uint32_t i = 0; i < m_swapchain_images.size(); i++) {
         auto image_data = static_cast<x11_image_data *>(m_swapchain_images[i].data);
         if (image_data != nullptr && image_data->pixmap == idle->pixmap) {
            unpresent_image(i);
            break;
         }
      }
   } else if (ge->evtype == XCB_PRESENT_EVENT_COMPLETE_NOTIFY) {
      std::unique_lock<std::mutex> lock(m_present_mutex);
      if (m_outstanding_pixmaps > 0) {
         m_outstanding_pixmaps--;
         m_present_cond.notify_one();
      }
   }
}

void swapchain::present_image(const pending_present_request &pending_present)
{
   auto image_data = reinterpret_cast<x11_image_data *>(m_swapchain_images[pending_present.image_index].data);
   if (image_data == nullptr) {
      WSI_LOG_ERROR("present_image: image_data is null");
      return;
   }
   
   if (m_present_mode == VK_PRESENT_MODE_FIFO_KHR || m_present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
      std::unique_lock<std::mutex> lock(m_present_mutex);
      while (m_outstanding_pixmaps >= 1) {
         m_present_cond.wait(lock);
      }
      m_outstanding_pixmaps++;
   }

   VkResult present_result = m_dri3_presenter->present_image(image_data, 0, m_present_mode);

   if (present_result != VK_SUCCESS) {
      WSI_LOG_ERROR("Failed to present image using DRI3: %d", present_result);
   }

   if (m_device_data.is_present_id_enabled())
   {
      auto *ext = get_swapchain_extension<wsi_ext_present_id>(true);
      ext->set_present_id(pending_present.present_id);
   }
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

      if (data->linear_image != VK_NULL_HANDLE) {
         m_device_data.disp.DestroyImage(m_device, data->linear_image, get_allocation_callbacks());
         data->linear_image = VK_NULL_HANDLE;
      }

      if (data->optimal_memory != VK_NULL_HANDLE) {
         m_device_data.disp.FreeMemory(m_device, data->optimal_memory, get_allocation_callbacks());
         data->optimal_memory = VK_NULL_HANDLE;
      }

      if (data->copy_cmd != VK_NULL_HANDLE && m_command_pool != VK_NULL_HANDLE) {
         m_device_data.disp.FreeCommandBuffers(m_device, m_command_pool, 1, &data->copy_cmd);
         data->copy_cmd = VK_NULL_HANDLE;
      }

      m_allocator.destroy(1, data);
      image.data = nullptr;
   }
}

VkResult swapchain::image_set_present_payload(swapchain_image &image, VkQueue queue,
                                              const queue_submit_semaphores &semaphores, const void *submission_pnext)
{
   auto data = reinterpret_cast<x11_image_data *>(image.data);
   if (data == nullptr) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   return data->present_fence.set_payload(queue, semaphores, submission_pnext, data->copy_cmd);
}

VkResult swapchain::image_wait_present(swapchain_image &image, uint64_t timeout)
{
   auto data = reinterpret_cast<x11_image_data *>(image.data);
   if (data == nullptr) {
      return VK_SUCCESS;
   }
   return data->present_fence.wait_payload(timeout);
}

VkResult swapchain::bind_swapchain_image(VkDevice &device, const VkBindImageMemoryInfo *bind_image_mem_info,
                                         const VkBindImageMemorySwapchainInfoKHR *bind_sc_info)
{
   auto &device_data = layer::device_private_data::get(device);
   const wsi::swapchain_image &swapchain_image = m_swapchain_images[bind_sc_info->imageIndex];
   auto image_data = reinterpret_cast<x11_image_data *>(swapchain_image.data);
   if (image_data == nullptr) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   return device_data.disp.BindImageMemory(device, bind_image_mem_info->image, image_data->optimal_memory, 0);
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
