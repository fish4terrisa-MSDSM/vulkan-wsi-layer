#include <vulkan/vulkan.h>
#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>
#include <iostream>
#include <chrono>

int main() {
    VkInstance instance;
    VkInstanceCreateInfo inst_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    const char* inst_exts[] = {"VK_KHR_surface", "VK_KHR_xcb_surface"};
    inst_info.enabledExtensionCount = 2;
    inst_info.ppEnabledExtensionNames = inst_exts;
    vkCreateInstance(&inst_info, nullptr, &instance);

    xcb_connection_t* conn = xcb_connect(nullptr, nullptr);
    xcb_window_t win = xcb_generate_id(conn);
    xcb_create_window(conn, 24, win, xcb_setup_roots_iterator(xcb_get_setup(conn)).data->root, 0, 0, 800, 600, 0, 0, 0, 0, nullptr);

    VkXcbSurfaceCreateInfoKHR surf_info = {VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR, nullptr, 0, conn, win};
    VkSurfaceKHR surface;
    auto vkCreateXcbSurfaceKHR = (PFN_vkCreateXcbSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateXcbSurfaceKHR");
    vkCreateXcbSurfaceKHR(instance, &surf_info, nullptr, &surface);

    uint32_t pdev_count = 1;
    VkPhysicalDevice pdev;
    vkEnumeratePhysicalDevices(instance, &pdev_count, &pdev);

    const float queue_prio = 1.0f;
    VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, 0, 1, &queue_prio};
    const char* dev_exts[] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo dev_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, nullptr, 0, 1, &queue_info, 0, nullptr, 1, dev_exts, nullptr};
    VkDevice device;
    vkCreateDevice(pdev, &dev_info, nullptr, &device);

    VkSwapchainCreateInfoKHR sc_info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, nullptr, 0, surface, 3, VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, {800, 600}, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, VK_NULL_HANDLE};

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; i++) {
        VkSwapchainKHR swapchain;
        vkCreateSwapchainKHR(device, &sc_info, nullptr, &swapchain);
        vkDestroySwapchainKHR(device, swapchain, nullptr);
    }
    auto end = std::chrono::high_resolution_clock::now();
    std::cout << "100 Swapchain creations/destructions took: " 
              << std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() 
              << " ms" << std::endl;

    return 0;
}
