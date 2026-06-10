#pragma once

#include "wsi/swapchain_base.hpp"
#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include <xcb/present.h>
#include "surface.hpp"
#include "wsi/external_memory.hpp"
#include "dri3_presenter.hpp"

namespace wsi
{
namespace x11
{

struct x11_image_data
{
   x11_image_data(const VkDevice &device, const util::allocator &allocator)
      : external_mem(device, allocator)
   {
   }

   external_memory external_mem;
   xcb_pixmap_t pixmap = XCB_PIXMAP_NONE;
   fence_sync present_fence;

   uint32_t width = 0;
   uint32_t height = 0;
   int depth = 0;

   VkDevice device = VK_NULL_HANDLE;
   layer::device_private_data *device_data = nullptr;
};

class swapchain : public wsi::swapchain_base
{
public:
   explicit swapchain(layer::device_private_data &dev_data, const VkAllocationCallbacks *pAllocator,
                      surface &wsi_surface);

   ~swapchain();

protected:
   VkResult init_platform(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info,
                          bool &use_presentation_thread) override;

   VkResult allocate_and_bind_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image) override;
   VkResult create_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image) override;
   void present_image(const pending_present_request &pending_present) override;
   void destroy_image(wsi::swapchain_image &image) override;
   VkResult image_set_present_payload(swapchain_image &image, VkQueue queue, const queue_submit_semaphores &semaphores, const void *submission_pnext) override;
   VkResult image_wait_present(swapchain_image &image, uint64_t timeout) override;
   VkResult bind_swapchain_image(VkDevice &device, const VkBindImageMemoryInfo *bind_image_mem_info, const VkBindImageMemorySwapchainInfoKHR *bind_sc_info) override;
   VkResult add_required_extensions(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info) override;

private:
   xcb_connection_t *m_connection;
   xcb_window_t m_window;
   surface *m_wsi_surface;

   xcb_present_event_t m_event_id = 0;
   xcb_special_event_t *m_special_event = nullptr;

   std::unique_ptr<dri3_presenter> m_dri3_presenter;

   void present_event_thread();
   bool m_present_event_thread_run;
   std::thread m_present_event_thread;

   std::mutex m_present_mutex;
   std::condition_variable m_present_cond;
   uint32_t m_outstanding_pixmaps = 0;
};

} /* namespace x11 */
} /* namespace wsi */
