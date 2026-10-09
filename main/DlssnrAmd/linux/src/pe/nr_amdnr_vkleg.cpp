// Copyright (c) 2026 Mauri de Souza Meneguzzo (mauri870). MIT, see LICENSE.
// The Vulkan half of the native transport; see nr_amdnr_vkleg.hpp.
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include "nr_amdnr_vkleg.hpp"
#include "nr_pe_log.hpp"
#include "nr_pe_session.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#ifndef NR_ARCH_RDNA3
#define NR_ARCH_RDNA3 0
#endif

using nr::pe::log;

namespace nr::amdnr {

namespace {

constexpr VkFormat kColourFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
constexpr uint32_t kColourTexel = 8, kMotionTexel = 4;
constexpr uint32_t kSlots = 3;

// Run a Vulkan call; on failure say which one and return false from the enclosing function.
#define LEG_TRY(call)                                                                               \
    do {                                                                                            \
        const VkResult leg_result_ = (call);                                                        \
        if (leg_result_ != VK_SUCCESS) {                                                            \
            if (why) *why = std::string(#call) + " failed (VkResult " + std::to_string(int(leg_result_)) + ")"; \
            return false;                                                                           \
        }                                                                                           \
    } while (0)

// The same for functions that return a pointer.
#define LEG_TRY_NULL(call)                                                                          \
    do {                                                                                            \
        const VkResult leg_result_ = (call);                                                        \
        if (leg_result_ != VK_SUCCESS) {                                                            \
            *why = std::string(#call) + " failed (VkResult " + std::to_string(int(leg_result_)) + ")"; \
            return nullptr;                                                                         \
        }                                                                                           \
    } while (0)

}  // namespace

struct LegExchange {
    uint32_t width{}, height{}, motion_width{}, motion_height{};
    VkDeviceSize colour_pitch{}, motion_pitch{}, out_pitch{};
    LegBuffer colour_in, motion_in, out;
    VkImage colour{}, motion{}, answer{};
    VkDeviceMemory colour_memory{}, motion_memory{}, answer_memory{};
    uint64_t feature{};
    bool answer_used{};   // the answer image has been through a layout transition
};

struct VulkanLeg::Impl {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    uint32_t family{};
    VkQueue frame_queue{};   // also the session's build queue
    std::mutex queue_mutex;  // submits on it, ours and the session's
    VkPhysicalDeviceMemoryProperties memory{};
    std::string gpu_name;
    bool external_win32{};
    VkCommandPool pool{};
    struct Slot { VkCommandBuffer cmd{}; VkFence fence{}; };
    Slot slots[kSlots]{};
    uint32_t next_slot = 0;
    std::unique_ptr<nr::pe::Session> session;
    nr::pe::DeviceHandles handles{};
    bool last_ran = false;
    PFN_vkImportSemaphoreWin32HandleKHR import_semaphore{};
    PFN_vkGetMemoryWin32HandlePropertiesKHR memory_handle_properties{};

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags wanted, VkMemoryPropertyFlags avoid = 0) const {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags f = memory.memoryTypes[i].propertyFlags;
            if ((bits & (1u << i)) && (f & wanted) == wanted && !(f & avoid)) return i;
        }
        return ~0u;
    }
};

VulkanLeg::VulkanLeg() : impl_(std::make_unique<Impl>()) {}

VulkanLeg::~VulkanLeg() {
    Impl& s = *impl_;
    if (!s.device) return;
    vkDeviceWaitIdle(s.device);
    s.session.reset();
    for (auto& slot : s.slots)
        if (slot.fence) vkDestroyFence(s.device, slot.fence, nullptr);
    if (s.pool) vkDestroyCommandPool(s.device, s.pool, nullptr);
    vkDestroyDevice(s.device, nullptr);
    if (s.instance) vkDestroyInstance(s.instance, nullptr);
}

const std::string& VulkanLeg::gpu_name() const { return impl_->gpu_name; }

std::unique_ptr<VulkanLeg> VulkanLeg::create(const Options& options, std::string* why) {
    std::unique_ptr<VulkanLeg> leg(new VulkanLeg());
    Impl& s = *leg->impl_;
    s.external_win32 = options.external_win32;

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "DLSSNR-AMD";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ii.pApplicationInfo = &app;
    {
        const VkResult r = vkCreateInstance(&ii, nullptr, &s.instance);
        if (r != VK_SUCCESS) {
            *why = "vkCreateInstance failed (VkResult " + std::to_string(int(r)) + "): is there a Vulkan driver?";
            return nullptr;
        }
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(s.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(s.instance, &count, devices.data());
    std::string seen;
    for (VkPhysicalDevice p : devices) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &id};
        vkGetPhysicalDeviceProperties2(p, &props);
        seen += std::string(seen.empty() ? "" : ", ") + props.properties.deviceName;
        const bool match = options.luid ? id.deviceLUIDValid && !std::memcmp(id.deviceLUID, options.luid, VK_LUID_SIZE)
                                        : props.properties.vendorID == 0x1002 &&
                                              props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (match && !s.physical) {
            s.physical = p;
            s.gpu_name = props.properties.deviceName;
        }
    }
    if (!s.physical) {
        *why = std::string(options.luid ? "no Vulkan device is on the D3D12 adapter" : "no AMD GPU found") +
               (seen.empty() ? " (the Vulkan loader lists none)" : " (Vulkan lists: " + seen + ")");
        return nullptr;
    }
    vkGetPhysicalDeviceMemoryProperties(s.physical, &s.memory);

    // The queue family: graphics and compute (the network converts the FP16 frame with a blit, which
    // needs graphics). One queue serves the frames and the network's build thread, which uploads the
    // weights; submits on it are taken in turn under queue_mutex.
    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> families(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(s.physical, &qcount, families.data());
    s.family = qcount;
    for (uint32_t i = 0; i < qcount && s.family == qcount; ++i)
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            s.family = i;
    if (s.family == qcount) {
        *why = "no graphics and compute queue family on " + s.gpu_name;
        return nullptr;
    }

    // Features: what the network needs, and nothing the device lacks (nrvk.hpp Context::create).
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceShaderFloat8FeaturesEXT fp8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR wml{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &coop; coop.pNext = &fp8; fp8.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13; f13.pNext = &wml;
    vkGetPhysicalDeviceFeatures2(s.physical, &query);
    const struct { bool have; const char* name; } required[] = {
        {bool(coop.cooperativeMatrix), "cooperativeMatrix"},
        {NR_ARCH_RDNA3 || bool(fp8.shaderFloat8), "shaderFloat8"},
        {NR_ARCH_RDNA3 || bool(fp8.shaderFloat8CooperativeMatrix), "shaderFloat8CooperativeMatrix"},
        {bool(f12.storageBuffer8BitAccess), "storageBuffer8BitAccess"},
        {bool(f12.shaderFloat16), "shaderFloat16"},
        {bool(f12.shaderInt8), "shaderInt8"},
        {bool(f12.vulkanMemoryModel), "vulkanMemoryModel"},
        {bool(f12.timelineSemaphore), "timelineSemaphore"},
        {bool(f13.subgroupSizeControl), "subgroupSizeControl"},
        {bool(f11.storageBuffer16BitAccess), "storageBuffer16BitAccess"},
    };
    for (const auto& r : required)
        if (!r.have) {
            *why = std::string("the Vulkan driver lacks the device feature ") + r.name + " on " + s.gpu_name;
            return nullptr;
        }
    const bool have_wml = wml.workgroupMemoryExplicitLayout && wml.workgroupMemoryExplicitLayout8BitAccess &&
                          wml.workgroupMemoryExplicitLayout16BitAccess;

    coop = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    coop.cooperativeMatrix = VK_TRUE;
    fp8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    fp8.shaderFloat8 = fp8.shaderFloat8CooperativeMatrix = NR_ARCH_RDNA3 ? VK_FALSE : VK_TRUE;
    f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.storageBuffer16BitAccess = VK_TRUE;
    f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.storageBuffer8BitAccess = VK_TRUE; f12.shaderFloat16 = VK_TRUE; f12.shaderInt8 = VK_TRUE;
    f12.vulkanMemoryModel = VK_TRUE; f12.timelineSemaphore = VK_TRUE;
    f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.subgroupSizeControl = VK_TRUE; f13.synchronization2 = VK_TRUE;
    wml.workgroupMemoryExplicitLayoutScalarBlockLayout = VK_FALSE;
    wml.pNext = nullptr;
    coop.pNext = &fp8; fp8.pNext = &f11; f11.pNext = &f12; f12.pNext = &f13;
    if (have_wml) f13.pNext = &wml;

    std::vector<const char*> extensions = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME};
    if (!NR_ARCH_RDNA3) extensions.push_back(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
    if (have_wml) extensions.push_back(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
    if (options.external_win32) {
        extensions.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
        extensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
    }
    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(s.physical, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> available(extension_count);
    vkEnumerateDeviceExtensionProperties(s.physical, nullptr, &extension_count, available.data());
    for (const char* wanted : extensions) {
        bool found = false;
        for (const auto& e : available) found |= !std::strcmp(e.extensionName, wanted);
        if (!found) {
            *why = std::string("the Vulkan driver lacks the device extension ") + wanted + " on " + s.gpu_name;
            return nullptr;
        }
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queues{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queues.queueFamilyIndex = s.family; queues.queueCount = 1; queues.pQueuePriorities = &priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &coop;
    di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &queues;
    di.enabledExtensionCount = uint32_t(extensions.size()); di.ppEnabledExtensionNames = extensions.data();
    {
        const VkResult r = vkCreateDevice(s.physical, &di, nullptr, &s.device);
        if (r != VK_SUCCESS) {
            *why = "vkCreateDevice failed on " + s.gpu_name + " (VkResult " + std::to_string(int(r)) + ")";
            return nullptr;
        }
    }
    vkGetDeviceQueue(s.device, s.family, 0, &s.frame_queue);
    if (options.external_win32) {
        s.import_semaphore = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
            vkGetDeviceProcAddr(s.device, "vkImportSemaphoreWin32HandleKHR"));
        s.memory_handle_properties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
            vkGetDeviceProcAddr(s.device, "vkGetMemoryWin32HandlePropertiesKHR"));
        if (!s.import_semaphore) {
            *why = "vkImportSemaphoreWin32HandleKHR is not exported by the driver";
            return nullptr;
        }
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = s.family;
    LEG_TRY_NULL(vkCreateCommandPool(s.device, &pci, nullptr, &s.pool));
    for (auto& slot : s.slots) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = s.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        LEG_TRY_NULL(vkAllocateCommandBuffers(s.device, &ai, &slot.cmd));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        LEG_TRY_NULL(vkCreateFence(s.device, &fi, nullptr, &slot.fence));
    }

    // The network, as a Vulkan game's upscaler would drive it: the session builds it on a thread of
    // its own, on its own queue, and frames pass through until it is ready.
#if NR_ARCH_RDNA3
    // The AMD Windows driver's compiler crashes on the persistent kernels (fswinp*) of the RDNA3 network even once
    // their loops are unrolled (windows/build/build_network.py); layer by layer it compiles and runs. This leg is the
    // Windows route, so the network is built without them unless the player says otherwise.
    if (!std::getenv("NR_NO_PERSIST")) _putenv("NR_NO_PERSIST=1");
#endif
    s.handles.instance = s.instance;
    s.handles.physical = s.physical;
    s.handles.device = s.device;
    s.session = std::make_unique<nr::pe::Session>(std::string());
    s.session->set_native_compose(true);
    s.session->set_vulkan_queue(s.frame_queue, s.family);
    Impl* impl = &s;
    s.session->set_vulkan_queue_lock([impl] { impl->queue_mutex.lock(); }, [impl] { impl->queue_mutex.unlock(); });
    log("[amdnr] Vulkan leg on %s, queue family %u (%s)", s.gpu_name.c_str(), s.family,
        options.external_win32 ? "D3D12 handles imported" : "plain buffers");
    return leg;
}

bool VulkanLeg::make_buffer(VkDeviceSize size, void* shared, LegBuffer* out, std::string* why) {
    Impl& s = *impl_;
    VkExternalMemoryBufferCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, shared ? &external : nullptr};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    LegBuffer made{};
    made.size = size;
    made.external = shared != nullptr;
    LEG_TRY(vkCreateBuffer(s.device, &bi, nullptr, &made.buffer));
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(s.device, made.buffer, &req);
    uint32_t bits = req.memoryTypeBits;
    VkResult r = VK_SUCCESS;
    if (shared) {
        if (!s.external_win32) {
            vkDestroyBuffer(s.device, made.buffer, nullptr);
            *why = "the leg was created without the Win32 import extensions";
            return false;
        }
        if (s.memory_handle_properties) {
            VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
            if (s.memory_handle_properties(s.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, shared, &hp) ==
                    VK_SUCCESS &&
                hp.memoryTypeBits)
                bits &= hp.memoryTypeBits;
        }
        const uint32_t type = s.memory_type(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const uint32_t any = type != ~0u ? type : s.memory_type(bits, 0);
        if (any == ~0u) {
            vkDestroyBuffer(s.device, made.buffer, nullptr);
            *why = "no memory type fits the imported D3D12 buffer";
            return false;
        }
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.buffer = made.buffer;
        VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated};
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        import.handle = shared;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = any;
        r = vkAllocateMemory(s.device, &ai, nullptr, &made.memory);
    } else {
        const uint32_t type = s.memory_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (type == ~0u) {
            vkDestroyBuffer(s.device, made.buffer, nullptr);
            *why = "no host-visible memory type";
            return false;
        }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        r = vkAllocateMemory(s.device, &ai, nullptr, &made.memory);
    }
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(s.device, made.buffer, nullptr);
        *why = std::string(shared ? "importing the D3D12 buffer" : "allocating the buffer") + " failed (VkResult " +
               std::to_string(int(r)) + ")";
        return false;
    }
    if (vkBindBufferMemory(s.device, made.buffer, made.memory, 0) != VK_SUCCESS) {
        vkFreeMemory(s.device, made.memory, nullptr);
        vkDestroyBuffer(s.device, made.buffer, nullptr);
        *why = "vkBindBufferMemory failed";
        return false;
    }
    if (!shared && vkMapMemory(s.device, made.memory, 0, VK_WHOLE_SIZE, 0, &made.mapped) != VK_SUCCESS) {
        vkFreeMemory(s.device, made.memory, nullptr);
        vkDestroyBuffer(s.device, made.buffer, nullptr);
        *why = "vkMapMemory failed";
        return false;
    }
    *out = made;
    return true;
}

void VulkanLeg::destroy_buffer(LegBuffer& buffer) {
    Impl& s = *impl_;
    if (buffer.buffer) vkDestroyBuffer(s.device, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(s.device, buffer.memory, nullptr);
    buffer = {};
}

VkSemaphore VulkanLeg::make_timeline(void* shared, std::string* why) {
    Impl& s = *impl_;
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;
    VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
    VkSemaphore semaphore{};
    const VkResult r = vkCreateSemaphore(s.device, &ci, nullptr, &semaphore);
    if (r != VK_SUCCESS) {
        *why = "vkCreateSemaphore failed (VkResult " + std::to_string(int(r)) + ")";
        return VK_NULL_HANDLE;
    }
    if (!shared) return semaphore;
    if (!s.import_semaphore) {
        vkDestroySemaphore(s.device, semaphore, nullptr);
        *why = "the leg was created without the Win32 import extensions";
        return VK_NULL_HANDLE;
    }
    VkImportSemaphoreWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
    import.semaphore = semaphore;
    import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    import.handle = shared;
    const VkResult ir = s.import_semaphore(s.device, &import);
    if (ir != VK_SUCCESS) {
        vkDestroySemaphore(s.device, semaphore, nullptr);
        *why = "importing the D3D12 fence as a timeline semaphore failed (VkResult " + std::to_string(int(ir)) + ")";
        return VK_NULL_HANDLE;
    }
    return semaphore;
}

void VulkanLeg::destroy_timeline(VkSemaphore semaphore) {
    if (semaphore) vkDestroySemaphore(impl_->device, semaphore, nullptr);
}

bool VulkanLeg::signal_timeline(VkSemaphore semaphore, uint64_t value) {
    VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    si.semaphore = semaphore;
    si.value = value;
    return vkSignalSemaphore(impl_->device, &si) == VK_SUCCESS;
}

bool VulkanLeg::wait_timeline(VkSemaphore semaphore, uint64_t value, uint64_t timeout_ns) {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &semaphore;
    wi.pValues = &value;
    return vkWaitSemaphores(impl_->device, &wi, timeout_ns) == VK_SUCCESS;
}

namespace {

bool make_image(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, uint32_t width, uint32_t height,
                VkFormat format, VkImageUsageFlags usage, VkImage* image, VkDeviceMemory* backing, std::string* why) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    LEG_TRY(vkCreateImage(device, &ci, nullptr, image));
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, *image, &req);
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < memory.memoryTypeCount && type == ~0u; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = i;
    if (type == ~0u) {
        vkDestroyImage(device, *image, nullptr);
        *image = VK_NULL_HANDLE;
        *why = "no device-local memory type for an image";
        return false;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    const VkResult r = vkAllocateMemory(device, &ai, nullptr, backing);
    if (r != VK_SUCCESS || vkBindImageMemory(device, *image, *backing, 0) != VK_SUCCESS) {
        vkDestroyImage(device, *image, nullptr);
        *image = VK_NULL_HANDLE;
        if (r == VK_SUCCESS) vkFreeMemory(device, *backing, nullptr);
        *backing = VK_NULL_HANDLE;
        *why = "allocating image memory failed (VkResult " + std::to_string(int(r)) + ")";
        return false;
    }
    return true;
}

}  // namespace

LegExchange* VulkanLeg::make_exchange(uint32_t width, uint32_t height, uint32_t motion_width,
                                                      uint32_t motion_height, VkDeviceSize colour_pitch,
                                                      VkDeviceSize motion_pitch, VkDeviceSize out_pitch,
                                                      const LegBuffer& colour_in, const LegBuffer& motion_in,
                                                      const LegBuffer& out, std::string* why) {
    Impl& s = *impl_;
    if (colour_pitch != out_pitch || colour_pitch % kColourTexel || motion_pitch % kMotionTexel) {
        *why = "the buffers' row pitches do not fit the image copies";
        return nullptr;
    }
    std::unique_ptr<LegExchange> x(new LegExchange());
    x->width = width; x->height = height;
    x->motion_width = motion_in.buffer ? motion_width : 0; x->motion_height = motion_in.buffer ? motion_height : 0;
    x->colour_pitch = colour_pitch; x->motion_pitch = motion_pitch; x->out_pitch = out_pitch;
    x->colour_in = colour_in; x->motion_in = motion_in; x->out = out;
    const VkImageUsageFlags rw = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    bool ok = make_image(s.device, s.memory, width, height, kColourFormat, rw, &x->colour, &x->colour_memory, why) &&
              make_image(s.device, s.memory, width, height, kColourFormat, rw, &x->answer, &x->answer_memory, why);
    if (ok && motion_in.buffer)
        ok = make_image(s.device, s.memory, motion_width, motion_height, kMotionFormat,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &x->motion, &x->motion_memory, why);
    if (!ok) {
        destroy_exchange(x.release());
        return nullptr;
    }
    x->feature = s.session->create_feature();
    return x.release();
}

void VulkanLeg::destroy_exchange(LegExchange* exchange) {
    if (!exchange) return;
    std::unique_ptr<LegExchange> x(exchange);
    Impl& s = *impl_;
    vkDeviceWaitIdle(s.device);
    if (x->feature) s.session->release_feature(x->feature);
    for (VkImage i : {x->colour, x->motion, x->answer})
        if (i) vkDestroyImage(s.device, i, nullptr);
    for (VkDeviceMemory m : {x->colour_memory, x->motion_memory, x->answer_memory})
        if (m) vkFreeMemory(s.device, m, nullptr);
}

namespace {

nr::Controls network_controls(const nr::dlssnr::Controls6& in) {
    nr::Controls c;
    c.enabled = true;
    c.intensity = in.intensity;
    c.style = in.style;
    c.local_structure = in.local_structure;
    c.local_tone = in.local_tone;
    c.skin_structure = in.skin_structure;
    c.automatic_mask = in.use_auto_mask != 0;
    return c;
}

VkImageMemoryBarrier image_barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src; b.dstAccessMask = dst;
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

}  // namespace

bool VulkanLeg::submit(const LegJob& job, std::string* why) {
    Impl& s = *impl_;
    LegExchange& x = *job.exchange;
    Impl::Slot& slot = s.slots[s.next_slot++ % kSlots];
    {
        const VkResult w = vkWaitForFences(s.device, 1, &slot.fence, VK_TRUE, 5'000'000'000ull);
        if (w != VK_SUCCESS) {
            *why = "the command buffer from three frames ago never finished (VkResult " + std::to_string(int(w)) + ")";
            return false;
        }
    }
    LEG_TRY(vkResetFences(s.device, 1, &slot.fence));
    LEG_TRY(vkResetCommandBuffer(slot.cmd, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    LEG_TRY(vkBeginCommandBuffer(slot.cmd, &begin));
    const VkCommandBuffer cmd = slot.cmd;
    const bool motion = job.use_motion && x.motion_in.buffer;

    // Ownership of the shared buffers comes from the D3D12 side for the frame and goes back after.
    auto buffer_barrier = [&](const LegBuffer& b, VkAccessFlags src, VkAccessFlags dst, bool acquire) {
        VkBufferMemoryBarrier m{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        m.srcAccessMask = src; m.dstAccessMask = dst;
        m.srcQueueFamilyIndex = !b.external ? VK_QUEUE_FAMILY_IGNORED : acquire ? VK_QUEUE_FAMILY_EXTERNAL : s.family;
        m.dstQueueFamilyIndex = !b.external ? VK_QUEUE_FAMILY_IGNORED : acquire ? s.family : VK_QUEUE_FAMILY_EXTERNAL;
        m.buffer = b.buffer; m.offset = 0; m.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd, acquire ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
                             acquire ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             1, &m, 0, nullptr);
    };
    buffer_barrier(x.colour_in, 0, VK_ACCESS_TRANSFER_READ_BIT, true);
    if (motion) buffer_barrier(x.motion_in, 0, VK_ACCESS_TRANSFER_READ_BIT, true);
    buffer_barrier(x.out, 0, VK_ACCESS_TRANSFER_WRITE_BIT, true);

    // The buffers into images. Their old contents are the previous frame's, so UNDEFINED.
    {
        VkImageMemoryBarrier b[2];
        uint32_t n = 0;
        b[n++] = image_barrier(x.colour, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        if (motion) b[n++] = image_barrier(x.motion, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, n, b);
    }
    VkBufferImageCopy copy{};
    copy.bufferRowLength = uint32_t(x.colour_pitch / kColourTexel);
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {x.width, x.height, 1};
    vkCmdCopyBufferToImage(cmd, x.colour_in.buffer, x.colour, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    if (motion) {
        VkBufferImageCopy mcopy{};
        mcopy.bufferRowLength = uint32_t(x.motion_pitch / kMotionTexel);
        mcopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        mcopy.imageExtent = {x.motion_width, x.motion_height, 1};
        vkCmdCopyBufferToImage(cmd, x.motion_in.buffer, x.motion, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &mcopy);
    }
    {
        // Where the network reads: both in GENERAL, and the answer image ready to be written.
        const VkAccessFlags all = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VkImageMemoryBarrier b[3];
        uint32_t n = 0;
        b[n++] = image_barrier(x.colour, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, all);
        if (motion) b[n++] = image_barrier(x.motion, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, all);
        b[n++] = image_barrier(x.answer, x.answer_used ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                               VK_IMAGE_LAYOUT_GENERAL, all, all);
        x.answer_used = true;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, n, b);
    }

    nr::pe::Session::VulkanFrame frame{};
    frame.feature = x.feature;
    frame.colour = x.colour; frame.colour_layout = VK_IMAGE_LAYOUT_GENERAL; frame.colour_format = kColourFormat;
    frame.width = x.width; frame.height = x.height;
    frame.colour_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    if (motion) {
        frame.motion = x.motion; frame.motion_layout = VK_IMAGE_LAYOUT_GENERAL; frame.motion_format = kMotionFormat;
        frame.motion_width = x.motion_width; frame.motion_height = x.motion_height;
        // The host's scale takes the vectors to pixels of the motion image; the network wants them as
        // a fraction of its extent.
        frame.motion_scale_x = x.motion_width ? job.motion_scale_x / float(x.motion_width) : job.motion_scale_x;
        frame.motion_scale_y = x.motion_height ? job.motion_scale_y / float(x.motion_height) : job.motion_scale_y;
    }
    frame.reset = job.reset;
    frame.output = x.answer; frame.output_layout = VK_IMAGE_LAYOUT_GENERAL;
    frame.output_usage = frame.colour_usage;
    const nr::Controls controls = network_controls(job.controls);
    VkImage answer = s.session->run_vulkan(s.handles, cmd, frame, controls);
    s.last_ran = answer != VK_NULL_HANDLE;

    if (answer) {
        // Whatever image holds the answer (ours, or the session's own), to the output buffer.
        const VkAccessFlags all = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        VkImageMemoryBarrier to_src = image_barrier(answer, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all,
                                                    VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_src);
        VkBufferImageCopy back{};
        back.bufferRowLength = uint32_t(x.out_pitch / kColourTexel);
        back.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        back.imageExtent = {x.width, x.height, 1};
        vkCmdCopyImageToBuffer(cmd, answer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, x.out.buffer, 1, &back);
        VkImageMemoryBarrier to_general = image_barrier(answer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                                        VK_ACCESS_TRANSFER_READ_BIT, all);
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_general);
    } else {
        // The network is not ready: the host gets its own frame back, which is an edit of zero.
        VkBufferCopy whole{0, 0, std::min(x.colour_in.size, x.out.size)};
        vkCmdCopyBuffer(cmd, x.colour_in.buffer, x.out.buffer, 1, &whole);
    }
    buffer_barrier(x.colour_in, VK_ACCESS_TRANSFER_READ_BIT, 0, false);
    if (motion) buffer_barrier(x.motion_in, VK_ACCESS_TRANSFER_READ_BIT, 0, false);
    buffer_barrier(x.out, VK_ACCESS_TRANSFER_WRITE_BIT, 0, false);
    LEG_TRY(vkEndCommandBuffer(cmd));

    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount = 1; timeline.pWaitSemaphoreValues = &job.wait_value;
    timeline.signalSemaphoreValueCount = 1; timeline.pSignalSemaphoreValues = &job.signal_value;
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &timeline};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &job.timeline; si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &job.timeline;
    VkResult submitted;
    {
        std::lock_guard<std::mutex> guard(s.queue_mutex);
        submitted = vkQueueSubmit(s.frame_queue, 1, &si, slot.fence);
    }
    if (submitted != VK_SUCCESS) {
        *why = "vkQueueSubmit failed (VkResult " + std::to_string(int(submitted)) + ")";
        return false;
    }
    return true;
}

bool VulkanLeg::network_ran() const { return impl_->last_ran; }
void VulkanLeg::wait_idle() { if (impl_->device) vkDeviceWaitIdle(impl_->device); }
float VulkanLeg::gpu_ms() const { return impl_->session ? impl_->session->gpu_ms() : 0.0f; }
bool VulkanLeg::session_failed() const { return impl_->session && impl_->session->failed(); }
std::string VulkanLeg::session_status() const { return impl_->session ? impl_->session->status() : std::string(); }

}  // namespace nr::amdnr
