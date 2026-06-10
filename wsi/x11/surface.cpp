#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include "surface.hpp"
#include "swapchain.hpp"
#include "surface_properties.hpp"

namespace wsi
{
namespace x11
{

struct surface::init_parameters
{
   const util::allocator &allocator;
   xcb_connection_t *connection;
   xcb_window_t window;
};

surface::surface(const init_parameters &params)
   : wsi::surface()
   , m_connection(params.connection)
   , m_window(params.window)
   , properties(this, params.allocator)
{
}

surface::~surface()
{
}

bool surface::init()
{
   return true;
}

bool surface::get_size_and_depth(uint32_t *width, uint32_t *height, int *depth)
{
   auto cookie = xcb_get_geometry(m_connection, m_window);
   if (auto *geom = xcb_get_geometry_reply(m_connection, cookie, nullptr))
   {
      *width = static_cast<uint32_t>(geom->width);
      *height = static_cast<uint32_t>(geom->height);
      *depth = static_cast<int>(geom->depth);
      free(geom);
      return true;
   }
   return false;
}

wsi::surface_properties &surface::get_properties()
{
   return properties;
}

util::unique_ptr<swapchain_base> surface::allocate_swapchain(layer::device_private_data &dev_data,
                                                             const VkAllocationCallbacks *allocator)
{
   util::allocator alloc{ dev_data.get_allocator(), VK_SYSTEM_ALLOCATION_SCOPE_OBJECT, allocator };
   return util::unique_ptr<swapchain_base>(alloc.make_unique<swapchain>(dev_data, allocator, *this));
}

util::unique_ptr<surface> surface::make_surface(const util::allocator &allocator, xcb_connection_t *conn,
                                                xcb_window_t window)
{
   init_parameters params{ allocator, conn, window };
   auto wsi_surface = allocator.make_unique<surface>(params);
   if (wsi_surface != nullptr && wsi_surface->init())
   {
      return wsi_surface;
   }
   return nullptr;
}

} /* namespace x11 */
} /* namespace wsi */
