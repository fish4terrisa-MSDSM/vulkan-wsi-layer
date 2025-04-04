/*
 * Copyright (c) 2024-2025 Arm Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/**
 * @file swapchain.hpp
 *
 * @brief Contains the class definition for a display swapchain.
 */

#pragma once

#include <vulkan/vk_icd.h>
#include <vulkan/vulkan.h>

#include "drm_display.hpp"
#include "surface.hpp"
#include <util/wsialloc/wsialloc.h>
#include <wsi/external_memory.hpp>

namespace wsi
{

namespace display
{

struct display_image_data
{
   display_image_data(const VkDevice &device, const util::allocator &allocator)
      : external_mem(device, allocator)
      , fb_id(std::numeric_limits<uint32_t>::max())
   {
   }

   external_memory external_mem;
   uint32_t fb_id;
   sync_fd_fence_sync present_fence;
};

struct image_creation_parameters
{
   wsialloc_format m_allocated_format;
   util::vector<VkSubresourceLayout> m_image_layout;
   VkExternalMemoryImageCreateInfoKHR m_external_info;
   VkImageDrmFormatModifierExplicitCreateInfoEXT m_drm_mod_info;

   image_creation_parameters(wsialloc_format allocated_format, util::allocator allocator,
                             VkExternalMemoryImageCreateInfoKHR external_info,
                             VkImageDrmFormatModifierExplicitCreateInfoEXT drm_mod_info)
      : m_allocated_format(allocated_format)
      , m_image_layout(allocator)
      , m_external_info(external_info)
      , m_drm_mod_info(drm_mod_info)
   {
   }
};

/**
 * @brief Display swapchain class.
 */
class swapchain : public wsi::swapchain_base
{
public:
   swapchain(layer::device_private_data &dev_data, const VkAllocationCallbacks *pAllocator, surface &wsi_surface);
   virtual ~swapchain();
   virtual VkResult init_platform(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info,
                                  bool &use_presentation_thread) override;

   virtual VkResult bind_swapchain_image(VkDevice &device, const VkBindImageMemoryInfo *bind_image_mem_info,
                                         const VkBindImageMemorySwapchainInfoKHR *bind_sc_info) override;

   VkResult allocate_and_bind_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image) override;

   virtual VkResult create_swapchain_image(VkImageCreateInfo image_create_info, swapchain_image &image) override;

   /**
    * @brief Method to present and image
    *
    * It sends the next image for presentation to the presentation engine.
    *
    * @param pending_present Information on the pending present request.
    */
   void present_image(const pending_present_request &pending_present) override;

   virtual VkResult image_set_present_payload(swapchain_image &image, VkQueue queue,
                                              const queue_submit_semaphores &semaphores,
                                              const void *submission_pnext) override;

   virtual VkResult image_wait_present(swapchain_image &image, uint64_t timeout) override;

   void destroy_image(swapchain_image &image) override;

private:
   VkResult allocate_image(display_image_data *image_data);

   VkResult allocate_wsialloc(VkImageCreateInfo &image_create_info, display_image_data *image_data,
                              util::vector<wsialloc_format> &importable_formats, wsialloc_format *allocated_format,
                              bool avoid_allocation);

   VkResult get_surface_compatible_formats(const VkImageCreateInfo &info,
                                           util::vector<wsialloc_format> &importable_formats,
                                           util::vector<uint64_t> &exportable_modifers,
                                           util::vector<VkDrmFormatModifierPropertiesEXT> &drm_format_props);

   VkResult create_framebuffer(const VkImageCreateInfo &image_create_info, display_image_data *image_data);

   /**
    * @brief Adds required extensions to the extension list of the swapchain
    *
    * @param device Vulkan device
    * @param swapchain_create_info Swapchain create info
    * @return VK_SUCCESS on success, other result codes on failure
    */
   VkResult add_required_extensions(VkDevice device, const VkSwapchainCreateInfoKHR *swapchain_create_info) override;

   wsialloc_allocator *m_wsi_allocator;
   drm_display_mode *m_display_mode;
   image_creation_parameters m_image_creation_parameters;
};
} /* namespace display */

} /* namespace wsi*/
