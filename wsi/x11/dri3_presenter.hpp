
#pragma once

#include <cstdint>
#include <vulkan/vulkan.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>

namespace wsi
{
namespace x11
{

class surface;
struct x11_image_data;

class dri3_presenter
{
public:
   dri3_presenter();
   ~dri3_presenter();

   bool is_available(xcb_connection_t *connection);

   VkResult init(xcb_connection_t *connection, xcb_window_t window, surface *wsi_surface);

   VkResult create_image_resources(x11_image_data *image_data, uint32_t width, uint32_t height,
                                   int depth, uint32_t stride, uint32_t fourcc, uint64_t modifier);

   VkResult present_image(x11_image_data *image_data, uint32_t serial, VkPresentModeKHR present_mode);

   void destroy_image_resources(x11_image_data *image_data);

private:
   xcb_connection_t *m_connection = nullptr;
   xcb_window_t m_window = 0;
   surface *m_wsi_surface = nullptr;
   uint32_t m_present_serial = 0;

   bool query_dri3_present(xcb_connection_t *connection);
};

} /* namespace x11 */
} /* namespace wsi */
