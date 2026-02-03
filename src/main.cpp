/*
 * Copyright (C) 2026 CrowdWare
 *
 * This file is part of RaidSimulator.
 */

// RaidSimulator - Vulkan + ImGui + SMLUI + REST Chunk Loading

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include "sml_ui.h"
#include "voxel_renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <cmath>
#include <vector>
#include <map>
#include <algorithm>

#define GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <curl/curl.h>

static VkAllocationCallbacks* g_Allocator = nullptr;
static VkInstance g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice g_Device = VK_NULL_HANDLE;
static uint32_t g_QueueFamily = (uint32_t)-1;
static VkQueue g_Queue = VK_NULL_HANDLE;
static VkPipelineCache g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
static uint32_t g_MinImageCount = 2;
static bool g_SwapChainRebuild = false;
static voxel::VoxelRenderer g_VoxelRenderer;

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

static void check_vk_result(VkResult err) {
    if (err == VK_SUCCESS)
        return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0)
        abort();
}

static bool LoadFileText(const char* path, std::string* out_text) {
    std::ifstream file(path);
    if (!file.is_open())
        return false;
    std::ostringstream ss;
    ss << file.rdbuf();
    *out_text = ss.str();
    return true;
}

static bool FileExists(const std::string& path) {
    std::ifstream file(path.c_str());
    return file.good();
}

static std::string ResolveRepoPath(const std::string& rel) {
    return std::string("RaidSimulator/") + rel;
}

struct ChunkHeader {
    int32_t chunk_x = 0;
    int32_t chunk_y = 0;
    int32_t chunk_z = 0;
    uint16_t block_count = 0;
    uint16_t block_size_cm = 60;
};

struct ChunkBlock {
    uint8_t x;
    uint8_t y;
    uint8_t z;
    uint8_t tile_id;
};

struct ChunkData {
    ChunkHeader header;
    std::vector<ChunkBlock> blocks;
};

struct SpawnPoint {
    bool valid = false;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

static size_t CurlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    std::vector<unsigned char>* out = reinterpret_cast<std::vector<unsigned char>*>(userdata);
    out->insert(out->end(), ptr, ptr + total);
    return total;
}

static bool FetchChunkBinary(const std::string& url, std::vector<unsigned char>* out_data) {
    if (!out_data)
        return false;
    out_data->clear();
    CURL* curl = curl_easy_init();
    if (!curl)
        return false;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_data);
    CURLcode res = curl_easy_perform(curl);
    long response = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK && response == 200);
}

static bool ParseChunkBinary(const std::vector<unsigned char>& data, ChunkData* out_chunk) {
    if (!out_chunk)
        return false;
    if (data.size() < sizeof(ChunkHeader))
        return false;
    ChunkHeader header = {};
    std::memcpy(&header, data.data(), sizeof(ChunkHeader));
    size_t expected = sizeof(ChunkHeader) + sizeof(ChunkBlock) * header.block_count;
    if (data.size() < expected)
        return false;
    out_chunk->header = header;
    out_chunk->blocks.resize(header.block_count);
    std::memcpy(out_chunk->blocks.data(), data.data() + sizeof(ChunkHeader), sizeof(ChunkBlock) * header.block_count);
    return true;
}

static std::string TileKeyForId(uint8_t tile_id) {
    static const char* kTileKeys[] = {"s", "t", "u", "v", "w", "x", "y", "z"};
    if (tile_id < sizeof(kTileKeys) / sizeof(kTileKeys[0]))
        return kTileKeys[tile_id];
    return "s";
}

static bool ApplyChunkToBlocks(const ChunkData& chunk,
                               float block_size,
                               const std::map<uint8_t, int>& tile_mesh_index,
                               std::vector<voxel::VoxelRenderer::Block>* out_blocks,
                               SpawnPoint* out_spawn) {
    if (!out_blocks)
        return false;
    out_blocks->clear();
    out_blocks->reserve(chunk.blocks.size());
    size_t spawn_count = 0;
    static const uint8_t kSpawnTileId = 8;
    static const uint8_t kSpawnTileIdAscii = static_cast<uint8_t>('S');
    for (size_t i = 0; i < chunk.blocks.size(); ++i) {
        const ChunkBlock& blk = chunk.blocks[i];
        const float world_x = (chunk.header.chunk_x * 32 + blk.x) * block_size + block_size * 0.5f;
        const float world_y = (chunk.header.chunk_y * 32 + blk.y) * block_size + block_size * 0.5f;
        const float world_z = (chunk.header.chunk_z * 32 + blk.z) * block_size + block_size * 0.5f;
        if (blk.tile_id == kSpawnTileId || blk.tile_id == kSpawnTileIdAscii) {
            spawn_count += 1;
            if (out_spawn) {
                out_spawn->x = world_x;
                out_spawn->y = world_y;
                out_spawn->z = world_z;
            }
            continue;
        }
        voxel::VoxelRenderer::Block block;
        block.x = world_x;
        block.y = world_y;
        block.z = world_z;
        block.key = TileKeyForId(blk.tile_id);
        std::map<uint8_t, int>::const_iterator it = tile_mesh_index.find(blk.tile_id);
        block.mesh_index = (it != tile_mesh_index.end()) ? it->second : 0;
        block.tex_index = block.mesh_index;
        out_blocks->push_back(block);
    }
    if (out_spawn) {
        out_spawn->valid = (spawn_count == 1);
    }
    if (spawn_count != 1) {
        fprintf(stderr, "Spawn marker error: expected 1 spawn tile, found %zu\n", spawn_count);
        return false;
    }
    return true;
}

static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension) {
    for (int i = 0; i < properties.Size; i++)
        if (strcmp(properties[i].extensionName, extension) == 0)
            return true;
    return false;
}

static void SetupVulkan(ImVector<const char*> instance_extensions) {
    VkResult err;
    VkInstanceCreateInfo create_info = {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

    VkApplicationInfo app_info = {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "RaidSimulator";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "VoxelEngine";
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = VK_API_VERSION_1_0;
    create_info.pApplicationInfo = &app_info;

    uint32_t properties_count = 0;
    ImVector<VkExtensionProperties> properties;
    vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
    properties.resize(properties_count);
    err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
    check_vk_result(err);

    if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
    if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
#endif

    create_info.enabledExtensionCount = (uint32_t)instance_extensions.Size;
    create_info.ppEnabledExtensionNames = instance_extensions.Data;

    err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
    check_vk_result(err);

    g_PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(g_Instance);
    IM_ASSERT(g_PhysicalDevice != VK_NULL_HANDLE);

    g_QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(g_PhysicalDevice);
    IM_ASSERT(g_QueueFamily != (uint32_t)-1);

    ImVector<const char*> device_extensions;
    device_extensions.push_back("VK_KHR_swapchain");
    properties_count = 0;
    vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, nullptr);
    properties.resize(properties_count);
    vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &properties_count, properties.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
    if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
        device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif

    const float queue_priority[] = {1.0f};
    VkDeviceQueueCreateInfo queue_info[1] = {};
    queue_info[0].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info[0].queueFamilyIndex = g_QueueFamily;
    queue_info[0].queueCount = 1;
    queue_info[0].pQueuePriorities = queue_priority;

    VkDeviceCreateInfo device_info = {};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = queue_info;
    device_info.enabledExtensionCount = (uint32_t)device_extensions.Size;
    device_info.ppEnabledExtensionNames = device_extensions.Data;

    err = vkCreateDevice(g_PhysicalDevice, &device_info, g_Allocator, &g_Device);
    check_vk_result(err);
    vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);

    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
        {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}
    };
    VkDescriptorPoolCreateInfo pool_info = {};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = 1000 * IM_ARRAYSIZE(pool_sizes);
    pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
    pool_info.pPoolSizes = pool_sizes;

    err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
    check_vk_result(err);
}

static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* wd, VkSurfaceKHR surface, int width, int height) {
    wd->Surface = surface;

    VkBool32 res = 0;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, wd->Surface, &res);
    if (res != VK_TRUE) {
        fprintf(stderr, "Error: no WSI support on physical device 0\n");
        exit(1);
    }

    const VkFormat requestSurfaceImageFormat[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(g_PhysicalDevice, wd->Surface, requestSurfaceImageFormat, (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), requestSurfaceColorSpace);

    VkPresentModeKHR present_modes[] = {VK_PRESENT_MODE_FIFO_KHR};
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(g_PhysicalDevice, wd->Surface, present_modes, IM_ARRAYSIZE(present_modes));

    ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, width, height, g_MinImageCount, 0);
}

static void CleanupVulkan() {
    vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
    vkDestroyDevice(g_Device, g_Allocator);
    vkDestroyInstance(g_Instance, g_Allocator);
}

static void CleanupVulkanWindow() {
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, &g_MainWindowData, g_Allocator);
}

static void FrameRender(ImGui_ImplVulkanH_Window* wd, ImDrawData* draw_data) {
    VkResult err;
    VkSemaphore image_acquired_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    err = vkAcquireNextImageKHR(g_Device, wd->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_SwapChainRebuild = true;
        return;
    }
    check_vk_result(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
        check_vk_result(err);
        err = vkResetFences(g_Device, 1, &fd->Fence);
        check_vk_result(err);
    }
    {
        err = vkResetCommandPool(g_Device, fd->CommandPool, 0);
        check_vk_result(err);
        VkCommandBufferBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        check_vk_result(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = wd->RenderPass;
        info.framebuffer = fd->Framebuffer;
        info.renderArea.extent.width = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount = 1;
        info.pClearValues = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    g_VoxelRenderer.render(fd->CommandBuffer, wd->Width, wd->Height);
    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &image_acquired_semaphore;
        info.pWaitDstStageMask = &wait_stage;
        info.commandBufferCount = 1;
        info.pCommandBuffers = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &render_complete_semaphore;

        err = vkEndCommandBuffer(fd->CommandBuffer);
        check_vk_result(err);
        err = vkQueueSubmit(g_Queue, 1, &info, fd->Fence);
        check_vk_result(err);
    }
}

static void FramePresent(ImGui_ImplVulkanH_Window* wd) {
    if (g_SwapChainRebuild)
        return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &render_complete_semaphore;
    info.swapchainCount = 1;
    info.pSwapchains = &wd->Swapchain;
    info.pImageIndices = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(g_Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR) {
        g_SwapChainRebuild = true;
        return;
    }
    check_vk_result(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->ImageCount;
}

int main(int, char**) {
    std::string ui_path = ResolveRepoPath("UI.sml");
    smlui::UiDocument ui_document;
    std::string parse_error;
    std::string ui_text;
    if (!LoadFileText(ui_path.c_str(), &ui_text)) {
        fprintf(stderr, "SML load error: could not read %s\n", ui_path.c_str());
    } else if (!ui_document.parseFromString(ui_text, &parse_error)) {
        fprintf(stderr, "SML parse error: %s\n", parse_error.c_str());
    }

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    float main_scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
    smlui::UiWindow ui_window = ui_document.window();
    GLFWwindow* window = glfwCreateWindow((int)(ui_window.size.x * main_scale), (int)(ui_window.size.y * main_scale),
                                          ui_window.title.empty() ? "RaidSimulator" : ui_window.title.c_str(),
                                          nullptr, nullptr);
    if (!glfwVulkanSupported()) {
        printf("GLFW: Vulkan Not Supported\n");
        return 1;
    }

    ImVector<const char*> extensions;
    uint32_t extensions_count = 0;
    const char** glfw_extensions = glfwGetRequiredInstanceExtensions(&extensions_count);
    for (uint32_t i = 0; i < extensions_count; i++)
        extensions.push_back(glfw_extensions[i]);
    SetupVulkan(extensions);

    VkSurfaceKHR surface;
    VkResult err = glfwCreateWindowSurface(g_Instance, window, g_Allocator, &surface);
    check_vk_result(err);

    int w, h;
    glfwGetFramebufferSize(window, &w, &h);
    ImGui_ImplVulkanH_Window* wd = &g_MainWindowData;
    SetupVulkanWindow(wd, surface, w, h);

    std::string shader_world_vert = ResolveRepoPath("shaders/world.vert.spv");
    std::string shader_world_frag = ResolveRepoPath("shaders/world.frag.spv");
    std::string shader_pick_vert = ResolveRepoPath("shaders/pick.vert.spv");
    std::string shader_pick_frag = ResolveRepoPath("shaders/pick.frag.spv");
    std::string ground_texture = ResolveRepoPath("../RaidBuilder/assets/textures/raid_ground.png");
    std::string block_texture = ResolveRepoPath("../RaidBuilder/assets/textures/raid_stone.png");

    std::vector<std::string> block_texture_paths;
    block_texture_paths.push_back(block_texture);

    if (!g_VoxelRenderer.init(g_Device, g_PhysicalDevice, g_Queue, g_QueueFamily, wd->RenderPass,
                              shader_world_vert.c_str(),
                              shader_world_frag.c_str(),
                              shader_pick_vert.c_str(),
                              shader_pick_frag.c_str(),
                              ground_texture.c_str(),
                              block_texture_paths)) {
        fprintf(stderr, "VoxelRenderer init failed (missing shaders?)\n");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;

    ImGui_ImplGlfw_InitForVulkan(window, true);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.PipelineCache = g_PipelineCache;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = wd->ImageCount;
    init_info.Allocator = g_Allocator;
    init_info.PipelineInfoMain.RenderPass = wd->RenderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = check_vk_result;
    ImGui_ImplVulkan_Init(&init_info);

    ImFont* font_13 = nullptr;
    ImFont* font_15 = nullptr;
    ImFontConfig font_cfg;
    font_cfg.SizePixels = 13.0f;
    font_13 = io.Fonts->AddFontDefault(&font_cfg);
    ImFontConfig label_cfg;
    label_cfg.SizePixels = 15.0f;
    font_15 = io.Fonts->AddFontDefault(&label_cfg);
    io.FontDefault = font_13;

    curl_global_init(CURL_GLOBAL_ALL);
    std::string server_base = "http://localhost:8080";
    std::string chunk_url = server_base + "/chunk?x=0&y=0&z=0";
    std::vector<unsigned char> chunk_raw;
    ChunkData chunk;
    std::vector<voxel::VoxelRenderer::Block> blocks;
    std::map<uint8_t, int> tile_mesh_index;
    tile_mesh_index[0] = 0;
    tile_mesh_index[1] = 0;
    float camera_x = 6.0f;
    float camera_y = 6.0f;
    float camera_z = 6.0f;
    if (FetchChunkBinary(chunk_url, &chunk_raw) && ParseChunkBinary(chunk_raw, &chunk)) {
        float block_size = chunk.header.block_size_cm > 0 ? (chunk.header.block_size_cm / 100.0f) : 0.6f;
        SpawnPoint spawn;
        bool spawn_ok = ApplyChunkToBlocks(chunk, block_size, tile_mesh_index, &blocks, &spawn);
        g_VoxelRenderer.setBlocks(blocks, block_size);
        if (spawn_ok) {
            camera_x = spawn.x;
            camera_y = spawn.y;
            camera_z = spawn.z;
        }
        if (!blocks.empty()) {
            float min_x = blocks[0].x;
            float min_y = blocks[0].y;
            float min_z = blocks[0].z;
            float max_x = blocks[0].x;
            float max_y = blocks[0].y;
            float max_z = blocks[0].z;
            for (size_t i = 1; i < blocks.size(); ++i) {
                min_x = std::min(min_x, blocks[i].x);
                min_y = std::min(min_y, blocks[i].y);
                min_z = std::min(min_z, blocks[i].z);
                max_x = std::max(max_x, blocks[i].x);
                max_y = std::max(max_y, blocks[i].y);
                max_z = std::max(max_z, blocks[i].z);
            }
            printf("Loaded %zu blocks. Bounds: X[%.2f..%.2f] Y[%.2f..%.2f] Z[%.2f..%.2f]\n",
                   blocks.size(), min_x, max_x, min_y, max_y, min_z, max_z);
        } else {
            printf("Loaded 0 blocks from server.\n");
        }
    }

    float camera_yaw = 3.1415926f * 0.75f;
    float camera_pitch = -0.4f;
    float move_speed = 4.5f;
    float mouse_sensitivity = 0.0035f;
    double last_mouse_x = 0.0;
    double last_mouse_y = 0.0;
    bool was_rotating = false;

    ImVec4 clear_color = ImVec4(0.18f, 0.35f, 0.75f, 1.00f);
    double last_time = glfwGetTime();
    float fps = 0.0f;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        double now_time = glfwGetTime();
        float dt = (float)(now_time - last_time);
        last_time = now_time;
        if (dt > 0.0f)
            fps = 1.0f / dt;

        int fb_width, fb_height;
        glfwGetFramebufferSize(window, &fb_width, &fb_height);
        if (fb_width > 0 && fb_height > 0 && (g_SwapChainRebuild || g_MainWindowData.Width != fb_width || g_MainWindowData.Height != fb_height)) {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(g_Instance, g_PhysicalDevice, g_Device, wd, g_QueueFamily, g_Allocator, fb_width, fb_height, g_MinImageCount, 0);
            g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) != 0) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        bool play_clicked = false;
        ui_document.render(viewport, font_15, &play_clicked);

        const bool key_w = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS;
        const bool key_a = glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS;
        const bool key_s = glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS;
        const bool key_d = glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS;
        const bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
        const bool rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        const bool ui_active = ImGui::IsAnyItemActive();
        const bool rotating = rmb || (lmb && !ui_active);

        if (rotating) {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        } else {
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
        }

        double mouse_x = 0.0;
        double mouse_y = 0.0;
        glfwGetCursorPos(window, &mouse_x, &mouse_y);
        if (rotating) {
            if (!was_rotating) {
                last_mouse_x = mouse_x;
                last_mouse_y = mouse_y;
            } else {
                const double dx = mouse_x - last_mouse_x;
                const double dy = mouse_y - last_mouse_y;
                camera_yaw += static_cast<float>(dx) * mouse_sensitivity;
                camera_pitch -= static_cast<float>(dy) * mouse_sensitivity;
                camera_pitch = std::max(-1.4f, std::min(1.4f, camera_pitch));
                last_mouse_x = mouse_x;
                last_mouse_y = mouse_y;
            }
        }
        was_rotating = rotating;

        const bool forward_input = key_w || (lmb && rmb);
        const bool back_input = key_s;
        const bool left_input = key_a;
        const bool right_input = key_d;

        if (forward_input || back_input || left_input || right_input) {
            const float yaw = camera_yaw;
            const float forward_x = std::cos(yaw);
            const float forward_z = std::sin(yaw);
            const float right_x = -forward_z;
            const float right_z = forward_x;
            float move_x = 0.0f;
            float move_z = 0.0f;
            if (forward_input) {
                move_x += forward_x;
                move_z += forward_z;
            }
            if (back_input) {
                move_x -= forward_x;
                move_z -= forward_z;
            }
            if (left_input) {
                move_x -= right_x;
                move_z -= right_z;
            }
            if (right_input) {
                move_x += right_x;
                move_z += right_z;
            }
            const float len = std::sqrt(move_x * move_x + move_z * move_z);
            if (len > 0.0001f) {
                move_x /= len;
                move_z /= len;
            }
            camera_x += move_x * move_speed * dt;
            camera_z += move_z * move_speed * dt;
        }

        g_VoxelRenderer.setCamera(camera_x, camera_y, camera_z, camera_yaw, camera_pitch);

        ImGui::SetNextWindowBgAlpha(0.0f);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 140.0f, viewport->WorkPos.y + 8.0f));
        ImGuiWindowFlags hud_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                    ImGuiWindowFlags_NoFocusOnAppearing;
        ImGui::Begin("FPSOverlay", nullptr, hud_flags);
        ImGui::Text("FPS: %.1f", fps);
        ImGui::End();

        ImGui::Render();
        ImDrawData* main_draw_data = ImGui::GetDrawData();
        const bool main_is_minimized = (main_draw_data->DisplaySize.x <= 0.0f || main_draw_data->DisplaySize.y <= 0.0f);
        wd->ClearValue.color.float32[0] = clear_color.x * clear_color.w;
        wd->ClearValue.color.float32[1] = clear_color.y * clear_color.w;
        wd->ClearValue.color.float32[2] = clear_color.z * clear_color.w;
        wd->ClearValue.color.float32[3] = clear_color.w;
        if (!main_is_minimized)
            FrameRender(wd, main_draw_data);
        FramePresent(wd);
    }

    curl_global_cleanup();
    vkDeviceWaitIdle(g_Device);
    g_VoxelRenderer.shutdown();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    CleanupVulkanWindow();
    CleanupVulkan();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}