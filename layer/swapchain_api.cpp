/*
 * Copyright (c) 2017, 2019, 2021 Arm Limited.
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
 * @file swapchain_api.cpp
 *
 * @brief Contains the Vulkan entrypoints for the swapchain.
 */

#include <cassert>
#include <cstdlib>
#include <new>

#include <wsi/wsi_factory.hpp>

#include "private_data.hpp"
#include "swapchain_api.hpp"

extern "C" {

VKAPI_ATTR VkResult wsi_layer_vkCreateSwapchainKHR(VkDevice device,
                                                   const VkSwapchainCreateInfoKHR *pSwapchainCreateInfo,
                                                   const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
   assert(pSwapchain != nullptr);
   layer::device_private_data &device_data = layer::device_private_data::get(device);
   VkSurfaceKHR surface = pSwapchainCreateInfo->surface;

   if (!device_data.should_layer_create_swapchain(surface))
   {
      if (!device_data.can_icds_create_swapchain(surface))
      {
         return VK_ERROR_INITIALIZATION_FAILED;
      }
      return device_data.disp.CreateSwapchainKHR(device_data.device, pSwapchainCreateInfo, pAllocator, pSwapchain);
   }

   auto sc = wsi::allocate_surface_swapchain(surface, device_data, pAllocator);
   if (sc == nullptr)
   {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   VkResult result = sc->init(device, pSwapchainCreateInfo);
   if (result != VK_SUCCESS)
   {
      return result;
   }

   result = device_data.add_layer_swapchain(reinterpret_cast<VkSwapchainKHR>(sc.get()));
   if (result != VK_SUCCESS)
   {
      return result;
   }

   *pSwapchain = reinterpret_cast<VkSwapchainKHR>(sc.release());
   return result;
}

VKAPI_ATTR void wsi_layer_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapc,
                                                const VkAllocationCallbacks *pAllocator)
{
   layer::device_private_data &device_data = layer::device_private_data::get(device);

   if (!device_data.layer_owns_swapchain(swapc))
   {
      return device_data.disp.DestroySwapchainKHR(device_data.device, swapc, pAllocator);
   }

   assert(swapc != VK_NULL_HANDLE);
   wsi::swapchain_base *sc = reinterpret_cast<wsi::swapchain_base *>(swapc);
   wsi::destroy_surface_swapchain(sc, device_data, pAllocator);
}

VKAPI_ATTR VkResult wsi_layer_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapc,
                                                      uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages)
{
   layer::device_private_data &device_data = layer::device_private_data::get(device);

   if (!device_data.layer_owns_swapchain(swapc))
   {
      return device_data.disp.GetSwapchainImagesKHR(device_data.device, swapc, pSwapchainImageCount, pSwapchainImages);
   }

   assert(pSwapchainImageCount != nullptr);
   assert(swapc != VK_NULL_HANDLE);
   wsi::swapchain_base *sc = reinterpret_cast<wsi::swapchain_base *>(swapc);
   return sc->get_swapchain_images(pSwapchainImageCount, pSwapchainImages);
}

VKAPI_ATTR VkResult wsi_layer_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapc, uint64_t timeout,
                                                    VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
   layer::device_private_data &device_data = layer::device_private_data::get(device);

   if (!device_data.layer_owns_swapchain(swapc))
   {
      return device_data.disp.AcquireNextImageKHR(device_data.device, swapc, timeout, semaphore, fence, pImageIndex);
   }

   assert(swapc != VK_NULL_HANDLE);
   assert(semaphore != VK_NULL_HANDLE || fence != VK_NULL_HANDLE);
   assert(pImageIndex != nullptr);
   wsi::swapchain_base *sc = reinterpret_cast<wsi::swapchain_base *>(swapc);
   return sc->acquire_next_image(timeout, semaphore, fence, pImageIndex);
}

VKAPI_ATTR VkResult wsi_layer_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
   assert(queue != VK_NULL_HANDLE);
   assert(pPresentInfo != nullptr);

   layer::device_private_data &device_data = layer::device_private_data::get(queue);

   if (!device_data.layer_owns_all_swapchains(pPresentInfo->pSwapchains, pPresentInfo->swapchainCount))
   {
      return device_data.disp.QueuePresentKHR(queue, pPresentInfo);
   }

   VkResult ret = VK_SUCCESS;
   for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
   {
      VkSwapchainKHR swapc = pPresentInfo->pSwapchains[i];

      wsi::swapchain_base *sc = reinterpret_cast<wsi::swapchain_base *>(swapc);
      assert(sc != nullptr);

      VkResult res = sc->queue_present(queue, pPresentInfo, pPresentInfo->pImageIndices[i]);

      if (pPresentInfo->pResults != nullptr)
      {
         pPresentInfo->pResults[i] = res;
      }

      if (res != VK_SUCCESS && ret == VK_SUCCESS)
      {
         ret = res;
      }
   }

   return ret;
}

VKAPI_ATTR VkResult wsi_layer_vkGetDeviceGroupPresentCapabilitiesKHR(
   VkDevice device, VkDeviceGroupPresentCapabilitiesKHR *pDeviceGroupPresentCapabilities)
{
   assert(pDeviceGroupPresentCapabilities != nullptr);

   pDeviceGroupPresentCapabilities->presentMask[0] = 1;
   pDeviceGroupPresentCapabilities->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;

   for (uint32_t i = 1; i < VK_MAX_DEVICE_GROUP_SIZE_KHR; i++)
   {
      pDeviceGroupPresentCapabilities->presentMask[i] = 0;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult wsi_layer_vkGetDeviceGroupSurfacePresentModesKHR(VkDevice device, VkSurfaceKHR surface,
                                                                     VkDeviceGroupPresentModeFlagsKHR *pModes)
{
   assert(pModes != nullptr);

   auto &device_data = layer::device_private_data::get(device);
   auto &instance = device_data.instance_data;

   if (!instance.should_layer_handle_surface(device_data.physical_device, surface))
   {
      return device_data.disp.GetDeviceGroupSurfacePresentModesKHR(device, surface, pModes);
   }

   *pModes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult wsi_layer_vkGetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physicalDevice,
                                                                      VkSurfaceKHR surface, uint32_t *pRectCount,
                                                                      VkRect2D *pRects)
{
   assert(surface);
   assert(pRectCount != nullptr);

   auto &instance = layer::instance_private_data::get(physicalDevice);

   if (!instance.should_layer_handle_surface(physicalDevice, surface))
   {
      return instance.disp.GetPhysicalDevicePresentRectanglesKHR(physicalDevice, surface, pRectCount, pRects);
   }

   VkResult result;
   wsi::surface_properties *props = wsi::get_surface_properties(instance, surface);
   assert(props);

   if (nullptr == pRects)
   {
      *pRectCount = 1;
      result = VK_SUCCESS;
   }
   else if (0 == *pRectCount)
   {
      result = VK_INCOMPLETE;
   }
   else
   {
      *pRectCount = 1;

      VkSurfaceCapabilitiesKHR surface_caps;
      result = props->get_surface_capabilities(physicalDevice, surface, &surface_caps);

      if (result != VK_SUCCESS)
      {
         return result;
      }

      pRects[0].offset.x = 0;
      pRects[0].offset.y = 0;
      pRects[0].extent = surface_caps.currentExtent;
   }

   return result;
}

VKAPI_ATTR VkResult wsi_layer_vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR *pAcquireInfo,
                                                     uint32_t *pImageIndex)
{
   assert(pAcquireInfo != VK_NULL_HANDLE);
   assert(pAcquireInfo->swapchain != VK_NULL_HANDLE);
   assert(pAcquireInfo->semaphore != VK_NULL_HANDLE || pAcquireInfo->fence != VK_NULL_HANDLE);
   assert(pImageIndex != nullptr);

   auto &device_data = layer::device_private_data::get(device);

   if (!device_data.layer_owns_swapchain(pAcquireInfo->swapchain))
   {
      return device_data.disp.AcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
   }

   wsi::swapchain_base *sc = reinterpret_cast<wsi::swapchain_base *>(pAcquireInfo->swapchain);

   return sc->acquire_next_image(pAcquireInfo->timeout, pAcquireInfo->semaphore, pAcquireInfo->fence, pImageIndex);
}

} /* extern "C" */
