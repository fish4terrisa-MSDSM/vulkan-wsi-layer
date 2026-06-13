#pragma once
#include <memory>
#include <chrono>
#include <mutex>
#include <vulkan/vk_icd.h>
#include <xcb/xcb.h>
#include <xcb/xproto.h>
#include "wsi/surface.hpp"
#include "surface_properties.hpp"

namespace wsi
{
namespace x11
{

class surface : public wsi::surface
{
public:
   bool init();

   surface() = delete;
   struct init_parameters;

   surface(const init_parameters &);
   ~surface();

   wsi::surface_properties &get_properties() override;
   util::unique_ptr<swapchain_base> allocate_swapchain(layer::device_private_data &dev_data,
                                                       const VkAllocationCallbacks *allocator) override;
   static util::unique_ptr<surface> make_surface(const util::allocator &allocator, xcb_connection_t *conn,
                                                 xcb_window_t window);

   bool get_size_and_depth(uint32_t *width, uint32_t *height, int *depth);

   xcb_connection_t *get_connection()
   {
      return m_connection;
   }

   xcb_window_t get_window()
   {
      return m_window;
   };

private:
   xcb_connection_t *m_connection;
   xcb_window_t m_window;
   surface_properties properties;

   /* Optimized size cache for eliminating blocking X11 IPC bottlenecks during polling */
   uint32_t m_cached_width{ 0 };
   uint32_t m_cached_height{ 0 };
   int m_cached_depth{ 24 };
   std::chrono::steady_clock::time_point m_last_query_time{};
   std::mutex m_query_mutex;
};

} /* namespace x11 */
} /* namespace wsi */
