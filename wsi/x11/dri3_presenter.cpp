#include "dri3_presenter.hpp"
#include "surface.hpp"
#include "swapchain.hpp"
#include "util/log.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/xcb.h>

namespace wsi
{
namespace x11
{

dri3_presenter::dri3_presenter() {}

dri3_presenter::~dri3_presenter() {}

bool dri3_presenter::query_dri3_present(xcb_connection_t *connection)
{
   static bool checked = false;
   static bool supported = false;

   if (checked) {
      return supported;
   }

   auto dri3_cookie = xcb_dri3_query_version(connection, 1, 2);
   auto *dri3_reply = xcb_dri3_query_version_reply(connection, dri3_cookie, nullptr);
   if (!dri3_reply)
   {
      checked = true;
      return false;
   }
   uint32_t dri3_major = dri3_reply->major_version;
   free(dri3_reply);

   if (dri3_major < 1)
   {
      checked = true;
      return false;
   }

   auto present_cookie = xcb_present_query_version(connection, 1, 2);
   auto *present_reply = xcb_present_query_version_reply(connection, present_cookie, nullptr);
   if (!present_reply)
   {
      checked = true;
      return false;
   }
   free(present_reply);

   checked = true;
   supported = true;
   return true;
}

bool dri3_presenter::is_available(xcb_connection_t *connection)
{
   return query_dri3_present(connection);
}

VkResult dri3_presenter::init(xcb_connection_t *connection, xcb_window_t window, surface *wsi_surface)
{
   m_connection = connection;
   m_window = window;
   m_wsi_surface = wsi_surface;
   return VK_SUCCESS;
}

VkResult dri3_presenter::create_image_resources(x11_image_data *image_data, uint32_t width, uint32_t height,
                                                int depth, uint32_t stride, uint32_t fourcc, uint64_t modifier)
{
   UNUSED(fourcc);
   if (!m_connection)
   {
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   int dma_buf_fd = image_data->external_mem.get_buffer_fds()[0];
   if (dma_buf_fd < 0)
   {
      WSI_LOG_ERROR("dri3_presenter: no DMA-BUF fd in image data");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   int fd_for_dri3 = dup(dma_buf_fd);
   if (fd_for_dri3 < 0)
   {
      WSI_LOG_ERROR("dri3_presenter: dup() failed: %s", strerror(errno));
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   // termux-x11 accepts 32bpp for xcb_dri3_pixmap_from_buffers mapped DMA_BUFs
   uint8_t bpp = 32;

   xcb_pixmap_t pixmap = xcb_generate_id(m_connection);

   // Unchecked call used here to avoid massively stalling the caller waiting for the round trip latency. 
   xcb_dri3_pixmap_from_buffers(m_connection, pixmap, m_window,
                                1, width, height, stride, 0, 0, 0, 0, 0, 0, 0, depth, bpp, modifier, &fd_for_dri3);

   image_data->pixmap = pixmap;
   image_data->width = width;
   image_data->height = height;
   image_data->depth = depth;

   return VK_SUCCESS;
}

VkResult dri3_presenter::present_image(x11_image_data *image_data, uint32_t serial, VkPresentModeKHR present_mode)
{
   UNUSED(serial);

   if (!m_connection || image_data->pixmap == XCB_PIXMAP_NONE)
   {
      return VK_ERROR_SURFACE_LOST_KHR;
   }

   m_present_serial++;

   uint32_t options = XCB_PRESENT_OPTION_NONE;
   if (present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR ||
       present_mode == VK_PRESENT_MODE_MAILBOX_KHR ||
       present_mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) {
      options |= XCB_PRESENT_OPTION_ASYNC;
   }

   xcb_present_pixmap(m_connection, m_window, image_data->pixmap, m_present_serial,
                      XCB_NONE, XCB_NONE, 0, 0, XCB_NONE, XCB_NONE, XCB_NONE,
                      options, 0, 0, 0, 0, nullptr);

   xcb_flush(m_connection);
   return VK_SUCCESS;
}

void dri3_presenter::destroy_image_resources(x11_image_data *image_data)
{
   if (m_connection && image_data->pixmap != XCB_PIXMAP_NONE)
   {
      xcb_free_pixmap(m_connection, image_data->pixmap);
      image_data->pixmap = XCB_PIXMAP_NONE;
   }
}

} /* namespace x11 */
} /* namespace wsi */
