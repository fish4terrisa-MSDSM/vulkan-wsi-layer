#include <iostream>
#include <fstream>
#include <vector>
#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <mutex>
#include <numeric>
#include <algorithm>
#include <xcb/xcb.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_xcb.h>

// Mutex to serialize access to the Vulkan graphics queue
std::mutex queueMutex;

struct ThreadStats {
    double totalTimeMs = 0.0;
    double swapchainCreateTimeMs = 0.0;
    double swapchainDestroyTimeMs = 0.0;
    double pipelineCreateTimeMs = 0.0;
    double pipelineRunTimeMs = 0.0;
    double pipelineDestroyTimeMs = 0.0;
};

struct MetricResult {
    double minVal = 0.0;
    double maxVal = 0.0;
    double avgVal = 0.0;
};

MetricResult calculateMetrics(const std::vector<double>& values) {
    if (values.empty()) return {};
    double minVal = *std::min_element(values.begin(), values.end());
    double maxVal = *std::max_element(values.begin(), values.end());
    double sumVal = std::accumulate(values.begin(), values.end(), 0.0);
    return { minVal, maxVal, sumVal / values.size() };
}

const std::string vertShaderGLSL = R"(
#version 450
layout(location = 0) out vec3 fragColor;
vec2 positions[3] = vec2[](
    vec2(0.0, -0.5),
    vec2(0.5, 0.5),
    vec2(-0.5, 0.5)
);
vec3 colors[3] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0)
);
void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragColor = colors[gl_VertexIndex];
}
)";

const std::string fragShaderGLSL = R"(
#version 450
layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;
void main() {
    outColor = vec4(fragColor, 1.0);
}
)";

std::vector<uint32_t> compileShader(const std::string& glsl, const std::string& stage) {
    std::string glslFilename = "temp_shader." + stage;
    std::string spvFilename = "temp_shader_" + stage + ".spv";

    std::ofstream out(glslFilename);
    out << glsl;
    out.close();

    std::string cmd = "glslc " + glslFilename + " -o " + spvFilename + " > /dev/null 2>&1";
    int ret = std::system(cmd.c_str());
    if (ret != 0) {
        cmd = "glslangValidator -V " + glslFilename + " -o " + spvFilename + " > /dev/null 2>&1";
        ret = std::system(cmd.c_str());
    }

    if (ret != 0) {
        std::cerr << "Error: Failed to compile " << stage << " shader.\n"
                  << "Ensure 'glslc' or 'glslangValidator' is installed.\n";
        std::remove(glslFilename.c_str());
        return {};
    }

    std::ifstream in(spvFilename, std::ios::binary | std::ios::ate);
    if (!in.is_open()) {
        std::remove(glslFilename.c_str());
        return {};
    }

    size_t size = in.tellg();
    std::vector<uint32_t> spv(size / sizeof(uint32_t));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(spv.data()), size);
    in.close();

    std::remove(glslFilename.c_str());
    std::remove(spvFilename.c_str());

    return spv;
}

VkShaderModule createShaderModule(VkDevice device, const std::vector<uint32_t>& code) {
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size() * sizeof(uint32_t);
    createInfo.pCode = code.data();

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module.");
    }
    return shaderModule;
}

void runThreadLoop(
    int threadIdx,
    VkSurfaceKHR surface,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkQueue graphicsQueue,
    int graphicsFamily,
    VkRenderPass renderPass,
    VkPipelineLayout pipelineLayout,
    VkGraphicsPipelineCreateInfo pipelineInfoTemplate,
    VkSurfaceFormatKHR surfaceFormat,
    VkSurfaceTransformFlagBitsKHR preTransform,
    VkCompositeAlphaFlagBitsKHR compositeAlpha,
    ThreadStats& stats
) {
    auto threadStart = std::chrono::high_resolution_clock::now();

    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    // 1. Thread-local command pool and buffer
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphicsFamily;

    if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
        std::cerr << "Thread " << threadIdx << ": Failed to create command pool\n";
        return;
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
        std::cerr << "Thread " << threadIdx << ": Failed to allocate command buffer\n";
        vkDestroyCommandPool(device, commandPool, nullptr);
        return;
    }

    // 2. Thread-local synchronization primitives
    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphore) != VK_SUCCESS ||
        vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphore) != VK_SUCCESS) {
        std::cerr << "Thread " << threadIdx << ": Failed to create semaphores\n";
        if (imageAvailableSemaphore) vkDestroySemaphore(device, imageAvailableSemaphore, nullptr);
        vkDestroyCommandPool(device, commandPool, nullptr);
        return;
    }

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device, &fenceInfo, nullptr, &fence) != VK_SUCCESS) {
        std::cerr << "Thread " << threadIdx << ": Failed to create fence\n";
        vkDestroySemaphore(device, renderFinishedSemaphore, nullptr);
        vkDestroySemaphore(device, imageAvailableSemaphore, nullptr);
        vkDestroyCommandPool(device, commandPool, nullptr);
        return;
    }

    VkSurfaceCapabilitiesKHR capabilities{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &capabilities) != VK_SUCCESS) {
        std::cerr << "Thread " << threadIdx << ": Failed to query capabilities\n";
        vkDestroyFence(device, fence, nullptr);
        vkDestroySemaphore(device, renderFinishedSemaphore, nullptr);
        vkDestroySemaphore(device, imageAvailableSemaphore, nullptr);
        vkDestroyCommandPool(device, commandPool, nullptr);
        return;
    }

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && imageCount > capabilities.maxImageCount) {
        imageCount = capabilities.maxImageCount;
    }

    float r = ((threadIdx % 9) / 9.0f);
    float g = (((threadIdx / 9) % 9) / 9.0f);
    float b = 1.0f - (r + g) * 0.5f;
    VkClearValue clearColor = {{{ r, g, b, 1.0f }}};

    VkGraphicsPipelineCreateInfo pipelineInfo = pipelineInfoTemplate;

    const int loops = 100;
    for (int i = 0; i < loops; ++i) {
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        std::vector<VkImageView> swapchainImageViews;
        std::vector<VkFramebuffer> swapchainFramebuffers;
        VkPipeline pipeline = VK_NULL_HANDLE;

        // A. Create swapchain
        auto scStart = std::chrono::high_resolution_clock::now();

        VkSwapchainCreateInfoKHR swapchainCreateInfo{};
        swapchainCreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        swapchainCreateInfo.surface = surface;
        swapchainCreateInfo.minImageCount = imageCount;
        swapchainCreateInfo.imageFormat = surfaceFormat.format;
        swapchainCreateInfo.imageColorSpace = surfaceFormat.colorSpace;
        swapchainCreateInfo.imageExtent = capabilities.currentExtent;
        swapchainCreateInfo.imageArrayLayers = 1;
        swapchainCreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        swapchainCreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        swapchainCreateInfo.preTransform = preTransform;
        swapchainCreateInfo.compositeAlpha = compositeAlpha;
        swapchainCreateInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        swapchainCreateInfo.clipped = VK_TRUE;

        VkResult scRes = vkCreateSwapchainKHR(device, &swapchainCreateInfo, nullptr, &swapchain);
        if (scRes != VK_SUCCESS) {
            auto scEnd = std::chrono::high_resolution_clock::now();
            stats.swapchainCreateTimeMs += std::chrono::duration<double, std::milli>(scEnd - scStart).count();
            continue;
        }

        // B. Get swapchain images defensively
        uint32_t swapchainImageCount = 0;
        if (vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, nullptr) != VK_SUCCESS || swapchainImageCount == 0) {
            auto scEnd = std::chrono::high_resolution_clock::now();
            stats.swapchainCreateTimeMs += std::chrono::duration<double, std::milli>(scEnd - scStart).count();

            auto scDestStart = std::chrono::high_resolution_clock::now();
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            auto scDestEnd = std::chrono::high_resolution_clock::now();
            stats.swapchainDestroyTimeMs += std::chrono::duration<double, std::milli>(scDestEnd - scDestStart).count();
            continue;
        }

        std::vector<VkImage> swapchainImages(swapchainImageCount);
        if (vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, swapchainImages.data()) != VK_SUCCESS) {
            auto scEnd = std::chrono::high_resolution_clock::now();
            stats.swapchainCreateTimeMs += std::chrono::duration<double, std::milli>(scEnd - scStart).count();

            auto scDestStart = std::chrono::high_resolution_clock::now();
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            auto scDestEnd = std::chrono::high_resolution_clock::now();
            stats.swapchainDestroyTimeMs += std::chrono::duration<double, std::milli>(scDestEnd - scDestStart).count();
            continue;
        }

        swapchainImageViews.resize(swapchainImageCount, VK_NULL_HANDLE);
        swapchainFramebuffers.resize(swapchainImageCount, VK_NULL_HANDLE);

        bool initSuccess = true;
        for (size_t j = 0; j < swapchainImageCount; ++j) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = swapchainImages[j];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = surfaceFormat.format;
            viewInfo.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;

            if (vkCreateImageView(device, &viewInfo, nullptr, &swapchainImageViews[j]) != VK_SUCCESS) {
                initSuccess = false;
                break;
            }

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = 1;
            framebufferInfo.pAttachments = &swapchainImageViews[j];
            framebufferInfo.width = capabilities.currentExtent.width;
            framebufferInfo.height = capabilities.currentExtent.height;
            framebufferInfo.layers = 1;

            if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &swapchainFramebuffers[j]) != VK_SUCCESS) {
                initSuccess = false;
                break;
            }
        }

        auto scEnd = std::chrono::high_resolution_clock::now();
        stats.swapchainCreateTimeMs += std::chrono::duration<double, std::milli>(scEnd - scStart).count();

        if (!initSuccess) {
            auto scDestStart = std::chrono::high_resolution_clock::now();
            for (auto fb : swapchainFramebuffers) if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device, fb, nullptr);
            for (auto iv : swapchainImageViews) if (iv != VK_NULL_HANDLE) vkDestroyImageView(device, iv, nullptr);
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            auto scDestEnd = std::chrono::high_resolution_clock::now();
            stats.swapchainDestroyTimeMs += std::chrono::duration<double, std::milli>(scDestEnd - scDestStart).count();
            continue;
        }

        // C. Create pipeline
        auto plcStart = std::chrono::high_resolution_clock::now();
        VkResult pipeRes = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        auto plcEnd = std::chrono::high_resolution_clock::now();
        stats.pipelineCreateTimeMs += std::chrono::duration<double, std::milli>(plcEnd - plcStart).count();

        if (pipeRes != VK_SUCCESS) {
            auto scDestStart = std::chrono::high_resolution_clock::now();
            for (auto fb : swapchainFramebuffers) vkDestroyFramebuffer(device, fb, nullptr);
            for (auto iv : swapchainImageViews) vkDestroyImageView(device, iv, nullptr);
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            auto scDestEnd = std::chrono::high_resolution_clock::now();
            stats.swapchainDestroyTimeMs += std::chrono::duration<double, std::milli>(scDestEnd - scDestStart).count();
            continue;
        }

        // D. Acquire image safely (start timing pipeline run here)
        auto runStart = std::chrono::high_resolution_clock::now();

        uint32_t imageIndex = 0;
        VkResult acquireRes = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
        if (acquireRes != VK_SUCCESS && acquireRes != VK_SUBOPTIMAL_KHR) {
            auto runEnd = std::chrono::high_resolution_clock::now();
            stats.pipelineRunTimeMs += std::chrono::duration<double, std::milli>(runEnd - runStart).count();

            auto plcDestStart = std::chrono::high_resolution_clock::now();
            vkDestroyPipeline(device, pipeline, nullptr);
            auto plcDestEnd = std::chrono::high_resolution_clock::now();
            stats.pipelineDestroyTimeMs += std::chrono::duration<double, std::milli>(plcDestEnd - plcDestStart).count();

            auto scDestStart = std::chrono::high_resolution_clock::now();
            for (auto fb : swapchainFramebuffers) vkDestroyFramebuffer(device, fb, nullptr);
            for (auto iv : swapchainImageViews) vkDestroyImageView(device, iv, nullptr);
            vkDestroySwapchainKHR(device, swapchain, nullptr);
            auto scDestEnd = std::chrono::high_resolution_clock::now();
            stats.swapchainDestroyTimeMs += std::chrono::duration<double, std::milli>(scDestEnd - scDestStart).count();
            continue;
        }

        // E. Record commands
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        vkBeginCommandBuffer(commandBuffer, &beginInfo);

        VkRenderPassBeginInfo rpBeginInfo{};
        rpBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBeginInfo.renderPass = renderPass;
        rpBeginInfo.framebuffer = swapchainFramebuffers[imageIndex];
        rpBeginInfo.renderArea.offset = {0, 0};
        rpBeginInfo.renderArea.extent = capabilities.currentExtent;
        rpBeginInfo.clearValueCount = 1;
        rpBeginInfo.pClearValues = &clearColor;

        vkCmdBeginRenderPass(commandBuffer, &rpBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(capabilities.currentExtent.width);
        viewport.height = static_cast<float>(capabilities.currentExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = capabilities.currentExtent;

        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        vkCmdDraw(commandBuffer, 3, 1, 0, 0);

        vkCmdEndRenderPass(commandBuffer);
        vkEndCommandBuffer(commandBuffer);

        // F. Submit and Present safely
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkSemaphore waitSemaphores[] = { imageAvailableSemaphore };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        VkSemaphore signalSemaphores[] = { renderFinishedSemaphore };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &imageIndex;

        VkResult submitRes = VK_SUCCESS;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            submitRes = vkQueueSubmit(graphicsQueue, 1, &submitInfo, fence);
            if (submitRes == VK_SUCCESS) {
                vkQueuePresentKHR(graphicsQueue, &presentInfo);
            }
        }

        // Wait on fence ONLY if queue submission succeeded to prevent deadlocks
        if (submitRes == VK_SUCCESS) {
            vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(device, 1, &fence);
        }

        auto runEnd = std::chrono::high_resolution_clock::now();
        stats.pipelineRunTimeMs += std::chrono::duration<double, std::milli>(runEnd - runStart).count();

        // G. Clean up loop-local resources safely
        auto plcDestStart = std::chrono::high_resolution_clock::now();
        vkDestroyPipeline(device, pipeline, nullptr);
        auto plcDestEnd = std::chrono::high_resolution_clock::now();
        stats.pipelineDestroyTimeMs += std::chrono::duration<double, std::milli>(plcDestEnd - plcDestStart).count();

        auto scDestStart = std::chrono::high_resolution_clock::now();
        for (size_t j = 0; j < swapchainImageCount; ++j) {
            if (swapchainFramebuffers[j] != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, swapchainFramebuffers[j], nullptr);
            }
            if (swapchainImageViews[j] != VK_NULL_HANDLE) {
                vkDestroyImageView(device, swapchainImageViews[j], nullptr);
            }
        }
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        auto scDestEnd = std::chrono::high_resolution_clock::now();
        stats.swapchainDestroyTimeMs += std::chrono::duration<double, std::milli>(scDestEnd - scDestStart).count();
    }

    // H. Clean up thread-local persistency objects
    vkDestroyFence(device, fence, nullptr);
    vkDestroySemaphore(device, renderFinishedSemaphore, nullptr);
    vkDestroySemaphore(device, imageAvailableSemaphore, nullptr);
    vkDestroyCommandPool(device, commandPool, nullptr);

    auto threadEnd = std::chrono::high_resolution_clock::now();
    stats.totalTimeMs = std::chrono::duration<double, std::milli>(threadEnd - threadStart).count();
}

int main() {
    // 1. Initialize XCB connection
    xcb_connection_t* connection = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(connection)) {
        std::cerr << "Failed to connect to X server via XCB.\n";
        return -1;
    }

    const xcb_setup_t* setup = xcb_get_setup(connection);
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    xcb_screen_t* screen = iter.data;

    uint16_t window_width = 900;
    uint16_t window_height = 900;

    xcb_window_t parent_window = xcb_generate_id(connection);
    uint32_t value_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    uint32_t value_list[2] = { screen->black_pixel, XCB_EVENT_MASK_EXPOSURE };

    xcb_create_window(connection, XCB_COPY_FROM_PARENT, parent_window, screen->root,
                      0, 0, window_width, window_height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen->root_visual, value_mask, value_list);
    xcb_map_window(connection, parent_window);

    xcb_window_t child_windows[81];
    for (int r = 0; r < 9; ++r) {
        for (int c = 0; c < 9; ++c) {
            int idx = r * 9 + c;
            child_windows[idx] = xcb_generate_id(connection);
            xcb_create_window(connection, XCB_COPY_FROM_PARENT, child_windows[idx], parent_window,
                              c * 100, r * 100, 100, 100, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                              screen->root_visual, value_mask, value_list);
            xcb_map_window(connection, child_windows[idx]);
        }
    }
    xcb_flush(connection);

    // 2. Initialize Vulkan Instance
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Vulkan Concurrent WSI Speed Test";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "No Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    std::vector<const char*> instanceExtensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XCB_SURFACE_EXTENSION_NAME
    };

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();

    VkInstance instance;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan instance.\n";
        return -1;
    }

    // 3. Create XCB Surfaces safely on the main thread
    std::vector<VkSurfaceKHR> surfaces(81, VK_NULL_HANDLE);
    for (int i = 0; i < 81; ++i) {
        VkXcbSurfaceCreateInfoKHR surfaceCreateInfo{};
        surfaceCreateInfo.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
        surfaceCreateInfo.connection = connection;
        surfaceCreateInfo.window = child_windows[i];

        if (vkCreateXcbSurfaceKHR(instance, &surfaceCreateInfo, nullptr, &surfaces[i]) != VK_SUCCESS) {
            std::cerr << "Failed to create XCB surface for child window " << i << ".\n";
            return -1;
        }
    }

    // 4. Select Physical Device
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        std::cerr << "No Vulkan compatible physical devices found.\n";
        return -1;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    VkPhysicalDevice physicalDevice = devices[0];

    // 5. Find Queue Family
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilies.data());

    int graphicsFamily = -1;
    for (uint32_t i = 0; i < queueFamilyCount; i++) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily = i;
            break;
        }
    }
    if (graphicsFamily == -1) {
        std::cerr << "Failed to find graphics queue family.\n";
        return -1;
    }

    // 6. Create Logical Device
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueCreateInfo{};
    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.queueFamilyIndex = graphicsFamily;
    queueCreateInfo.queueCount = 1;
    queueCreateInfo.pQueuePriorities = &queuePriority;

    std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME
    };

    VkDeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceCreateInfo.queueCreateInfoCount = 1;
    deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
    deviceCreateInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = deviceExtensions.data();

    VkDevice device;
    if (vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device) != VK_SUCCESS) {
        std::cerr << "Failed to create logical device.\n";
        return -1;
    }

    VkQueue graphicsQueue;
    vkGetDeviceQueue(device, graphicsFamily, 0, &graphicsQueue);

    // 7. Compile Shaders
    auto vertSPV = compileShader(vertShaderGLSL, "vert");
    auto fragSPV = compileShader(fragShaderGLSL, "frag");
    if (vertSPV.empty() || fragSPV.empty()) {
        return -1;
    }

    VkShaderModule vertShaderModule = createShaderModule(device, vertSPV);
    VkShaderModule fragShaderModule = createShaderModule(device, fragSPV);

    VkPipelineShaderStageCreateInfo shaderStages[2]{};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = vertShaderModule;
    shaderStages[0].pName = "main";

    shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = fragShaderModule;
    shaderStages[1].pName = "main";

    // 8. Query formats with variable initialization to prevent heap corruption
    VkSurfaceCapabilitiesKHR capabilities{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surfaces[0], &capabilities);

    uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surfaces[0], &formatCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surfaces[0], &formatCount, formats.data());

    VkSurfaceFormatKHR surfaceFormat = formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            surfaceFormat = f;
            break;
        }
    }

    VkSurfaceTransformFlagBitsKHR preTransform = capabilities.currentTransform;
    if (capabilities.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) {
        preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }

    VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) {
        compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    } else if (capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
        compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    }

    // 9. Build Render Pass
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = surfaceFormat.format;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;

    VkRenderPass renderPass;
    if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
        std::cerr << "Failed to create render pass.\n";
        return -1;
    }

    // 10. Pipeline State Templates
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;

    VkPipelineLayout pipelineLayout;
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        std::cerr << "Failed to create pipeline layout.\n";
        return -1;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    // 11. Run Concurrent Threads
    std::cout << "Starting concurrent WSI pipeline benchmark...\n";

    auto startTime = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    threads.reserve(81);

    std::vector<ThreadStats> allStats(81);

    for (int i = 0; i < 81; ++i) {
        threads.emplace_back(
            runThreadLoop,
            i,
            surfaces[i],
            physicalDevice,
            device,
            graphicsQueue,
            graphicsFamily,
            renderPass,
            pipelineLayout,
            pipelineInfo,
            surfaceFormat,
            preTransform,
            compositeAlpha,
            std::ref(allStats[i])
        );
    }

    for (auto& t : threads) {
        t.join();
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::milli> duration = endTime - startTime;

    // Process and report results
    std::vector<double> threadLifetimes;
    std::vector<double> scCreationSums;
    std::vector<double> scDestructionSums;
    std::vector<double> plcCreationSums;
    std::vector<double> plcExecutionSums;

    threadLifetimes.reserve(81);
    scCreationSums.reserve(81);
    scDestructionSums.reserve(81);
    plcCreationSums.reserve(81);
    plcExecutionSums.reserve(81);

    for (const auto& s : allStats) {
        threadLifetimes.push_back(s.totalTimeMs);
        scCreationSums.push_back(s.swapchainCreateTimeMs);
        scDestructionSums.push_back(s.swapchainDestroyTimeMs);
        plcCreationSums.push_back(s.pipelineCreateTimeMs);
        plcExecutionSums.push_back(s.pipelineRunTimeMs);
    }

    MetricResult threadRes = calculateMetrics(threadLifetimes);
    MetricResult scCreateRes = calculateMetrics(scCreationSums);
    MetricResult scDestroyRes = calculateMetrics(scDestructionSums);
    MetricResult plcCreateRes = calculateMetrics(plcCreationSums);
    MetricResult plcExecRes = calculateMetrics(plcExecutionSums);

    std::cout << "-------------------------------------------\n";
    std::cout << "Concurrent Benchmark Complete!\n";
    std::cout << "Total overall execution time: " << duration.count() << " ms\n";
    std::cout << "-------------------------------------------\n";
    std::cout << "Detailed Performance Statistics (across 81 threads, 100 loops each):\n\n";

    std::cout << "1. Thread Execution Lifetime (Total time to run one of the 81 threads):\n";
    std::cout << "   Minimum: " << threadRes.minVal << " ms\n";
    std::cout << "   Maximum: " << threadRes.maxVal << " ms\n";
    std::cout << "   Average: " << threadRes.avgVal << " ms\n\n";

    std::cout << "2. Swapchain Creation (Summed per thread after 100 loops):\n";
    std::cout << "   Minimum: " << scCreateRes.minVal << " ms\n";
    std::cout << "   Maximum: " << scCreateRes.maxVal << " ms\n";
    std::cout << "   Average: " << scCreateRes.avgVal << " ms\n\n";

    std::cout << "3. Swapchain Destruction (Summed per thread after 100 loops):\n";
    std::cout << "   Minimum: " << scDestroyRes.minVal << " ms\n";
    std::cout << "   Maximum: " << scDestroyRes.maxVal << " ms\n";
    std::cout << "   Average: " << scDestroyRes.avgVal << " ms\n\n";

    std::cout << "4. Graphics Pipeline Execution / Run (Summed per thread after 100 loops):\n";
    std::cout << "   Minimum: " << plcExecRes.minVal << " ms\n";
    std::cout << "   Maximum: " << plcExecRes.maxVal << " ms\n";
    std::cout << "   Average: " << plcExecRes.avgVal << " ms\n\n";

    std::cout << "5. Graphics Pipeline Creation / Compilation (Summed per thread after 100 loops):\n";
    std::cout << "   Minimum: " << plcCreateRes.minVal << " ms\n";
    std::cout << "   Maximum: " << plcCreateRes.maxVal << " ms\n";
    std::cout << "   Average: " << plcCreateRes.avgVal << " ms\n";
    std::cout << "-------------------------------------------\n";

    // 12. Cleanup
    for (int i = 0; i < 81; ++i) {
        vkDestroySurfaceKHR(instance, surfaces[i], nullptr);
    }
    vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyShaderModule(device, fragShaderModule, nullptr);
    vkDestroyShaderModule(device, vertShaderModule, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    for (int i = 0; i < 81; ++i) {
        xcb_destroy_window(connection, child_windows[i]);
    }
    xcb_destroy_window(connection, parent_window);
    xcb_disconnect(connection);

    return 0;
}
