#include "vantix/vulkan_backend.hpp"
#include "vantix/resources.hpp"

#include <volk.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace vantix {
namespace {
constexpr std::uint32_t group_size = 64;
constexpr VkDeviceSize input_bytes = group_size * 128;
constexpr VkDeviceSize output_bytes = group_size * 32;

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: VkResult " +
                                 std::to_string(result));
}

std::string uuid_string(const std::uint8_t* data, std::size_t length = VK_UUID_SIZE,
                        const char* prefix = "gpu:") {
    std::ostringstream out;
    out << prefix << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < length; ++i)
        out << std::setw(2) << static_cast<unsigned>(data[i]);
    return out.str();
}

std::vector<std::uint32_t> read_shader() {
    std::ifstream file(resource_path("gpu/vulkan/derive.spv"),
                       std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Vulkan SPIR-V shader is missing");
    const auto length = file.tellg();
    if (length <= 0 || length % 4 != 0)
        throw std::runtime_error("Invalid Vulkan SPIR-V shader length");
    std::vector<std::uint32_t> code(static_cast<std::size_t>(length) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), length);
    if (!file) throw std::runtime_error("Cannot read Vulkan SPIR-V shader");
    return code;
}

struct Instance {
    VkInstance handle = VK_NULL_HANDLE;
    ~Instance() { if (handle) vkDestroyInstance(handle, nullptr); }
};

std::shared_ptr<Instance> make_instance() {
    static std::once_flag init;
    static VkResult init_result = VK_ERROR_INITIALIZATION_FAILED;
    std::call_once(init, [] { init_result = volkInitialize(); });
    check(init_result, "Vulkan loader initialization");
    auto result = std::make_shared<Instance>();
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "VANTIX";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &app;
    check(vkCreateInstance(&create, nullptr, &result->handle), "vkCreateInstance");
    volkLoadInstance(result->handle);
    return result;
}

struct Buffer {
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};
}

struct VulkanBackend::Impl {
    std::shared_ptr<Instance> instance;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    Buffer input, output, params;
    VkPhysicalDeviceMemoryProperties memory_properties{};
    std::uint32_t queue_family = 0;
    WalletVersion version = WalletVersion::v5r1;
    std::string hardware_id;
    std::string device_name;

    ~Impl() {
        if (!device) return;
        vkDeviceWaitIdle(device);
        for (auto* buffer : {&input, &output, &params}) {
            if (buffer->mapped) {
                OPENSSL_cleanse(buffer->mapped, static_cast<std::size_t>(buffer->size));
                vkUnmapMemory(device, buffer->memory);
            }
            if (buffer->handle) vkDestroyBuffer(device, buffer->handle, nullptr);
            if (buffer->memory) vkFreeMemory(device, buffer->memory, nullptr);
        }
        if (fence) vkDestroyFence(device, fence, nullptr);
        if (command_pool) vkDestroyCommandPool(device, command_pool, nullptr);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        if (descriptor_layout) vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
        vkDestroyDevice(device, nullptr);
    }

    void create_buffer(Buffer& buffer, VkDeviceSize size, VkBufferUsageFlags usage) {
        buffer.size = size;
        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.size = size;
        create.usage = usage;
        create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &create, nullptr, &buffer.handle), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer.handle, &requirements);
        std::uint32_t type = UINT32_MAX;
        for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
            const auto flags = memory_properties.memoryTypes[i].propertyFlags;
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                type = i;
                break;
            }
        }
        if (type == UINT32_MAX)
            throw std::runtime_error("GPU has no host-coherent storage memory");
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = type;
        check(vkAllocateMemory(device, &allocate, nullptr, &buffer.memory), "vkAllocateMemory");
        check(vkBindBufferMemory(device, buffer.handle, buffer.memory, 0), "vkBindBufferMemory");
        check(vkMapMemory(device, buffer.memory, 0, size, 0, &buffer.mapped), "vkMapMemory");
        std::memset(buffer.mapped, 0, static_cast<std::size_t>(size));
    }

    void initialize() {
        vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
        float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_create{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_create.queueFamilyIndex = queue_family;
        queue_create.queueCount = 1;
        queue_create.pQueuePriorities = &priority;
        VkDeviceCreateInfo device_create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        device_create.queueCreateInfoCount = 1;
        device_create.pQueueCreateInfos = &queue_create;
        VkPhysicalDeviceFeatures enabled_features{};
        enabled_features.shaderInt64 = VK_TRUE;
        device_create.pEnabledFeatures = &enabled_features;
        check(vkCreateDevice(physical, &device_create, nullptr, &device), "vkCreateDevice");
        volkLoadDevice(device);
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        create_buffer(input, input_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        create_buffer(output, output_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        create_buffer(params, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        for (std::uint32_t i = 0; i < bindings.size(); ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layout_create{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout_create.bindingCount = static_cast<std::uint32_t>(bindings.size());
        layout_create.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &layout_create, nullptr,
                                          &descriptor_layout), "vkCreateDescriptorSetLayout");
        std::array<VkDescriptorPoolSize, 2> pool_sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}
        }};
        VkDescriptorPoolCreateInfo pool_create{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool_create.maxSets = 1;
        pool_create.poolSizeCount = 2;
        pool_create.pPoolSizes = pool_sizes.data();
        check(vkCreateDescriptorPool(device, &pool_create, nullptr, &descriptor_pool),
              "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo set_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        set_allocate.descriptorPool = descriptor_pool;
        set_allocate.descriptorSetCount = 1;
        set_allocate.pSetLayouts = &descriptor_layout;
        check(vkAllocateDescriptorSets(device, &set_allocate, &descriptors),
              "vkAllocateDescriptorSets");
        std::array<VkDescriptorBufferInfo, 3> buffer_info{{
            {input.handle, 0, input.size}, {output.handle, 0, output.size},
            {params.handle, 0, params.size}
        }};
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (std::uint32_t i = 0; i < writes.size(); ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = descriptors;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = bindings[i].descriptorType;
            writes[i].pBufferInfo = &buffer_info[i];
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);

        VkPipelineLayoutCreateInfo pipeline_layout_create{
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipeline_layout_create.setLayoutCount = 1;
        pipeline_layout_create.pSetLayouts = &descriptor_layout;
        check(vkCreatePipelineLayout(device, &pipeline_layout_create, nullptr,
                                     &pipeline_layout), "vkCreatePipelineLayout");
        const auto code = read_shader();
        VkShaderModuleCreateInfo module_create{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module_create.codeSize = code.size() * sizeof(std::uint32_t);
        module_create.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        check(vkCreateShaderModule(device, &module_create, nullptr, &module),
              "vkCreateShaderModule");
        VkComputePipelineCreateInfo pipeline_create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        pipeline_create.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeline_create.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeline_create.stage.module = module;
        pipeline_create.stage.pName = "main";
        pipeline_create.layout = pipeline_layout;
        const auto pipeline_result = vkCreateComputePipelines(
            device, VK_NULL_HANDLE, 1, &pipeline_create, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        check(pipeline_result, "vkCreateComputePipelines");
        VkCommandPoolCreateInfo command_create{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        command_create.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        command_create.queueFamilyIndex = queue_family;
        check(vkCreateCommandPool(device, &command_create, nullptr, &command_pool),
              "vkCreateCommandPool");
        VkCommandBufferAllocateInfo command_allocate{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        command_allocate.commandPool = command_pool;
        command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocate.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &command_allocate, &command),
              "vkAllocateCommandBuffers");
        VkFenceCreateInfo fence_create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(device, &fence_create, nullptr, &fence), "vkCreateFence");
    }

    std::vector<Bytes32> derive(const std::vector<std::string>& phrases) {
        if (phrases.empty() || phrases.size() > group_size)
            throw std::invalid_argument("Vulkan batch must contain 1..64 candidates");
        std::memset(input.mapped, 0, static_cast<std::size_t>(input.size));
        std::memset(output.mapped, 0, static_cast<std::size_t>(output.size));
        for (std::size_t i = 0; i < phrases.size(); ++i) {
            if (phrases[i].size() > 128)
                throw std::invalid_argument("Mnemonic exceeds Vulkan input stride");
            std::memcpy(static_cast<std::uint8_t*>(input.mapped) + i * 128,
                        phrases[i].data(), phrases[i].size());
        }
        const auto count = static_cast<std::uint32_t>(phrases.size());
        std::memcpy(params.mapped, &count, sizeof(count));
        check(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
                                0, 1, &descriptors, 0, nullptr);
        vkCmdDispatch(command, 1, 1, 1);
        check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
        check(vkResetFences(device, 1, &fence), "vkResetFences");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        check(vkQueueSubmit(queue, 1, &submit, fence), "vkQueueSubmit");
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, 120'000'000'000ULL),
              "vkWaitForFences");
        std::vector<Bytes32> result(phrases.size());
        for (std::size_t i = 0; i < result.size(); ++i)
            std::memcpy(result[i].data(),
                        static_cast<const std::uint8_t*>(output.mapped) + i * 32, 32);
        OPENSSL_cleanse(input.mapped, static_cast<std::size_t>(input.size));
        OPENSSL_cleanse(output.mapped, static_cast<std::size_t>(output.size));
        return result;
    }
};

VulkanBackend::VulkanBackend(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
VulkanBackend::~VulkanBackend() = default;

std::vector<std::shared_ptr<VulkanBackend>> VulkanBackend::create_all(
    WalletVersion version, MnemonicScheme scheme) {
    if (scheme != MnemonicScheme::multichain12) return {};
    auto instance = make_instance();
    std::uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance->handle, &count, nullptr),
          "vkEnumeratePhysicalDevices");
    std::vector<VkPhysicalDevice> physical(count);
    if (count)
        check(vkEnumeratePhysicalDevices(instance->handle, &count, physical.data()),
              "vkEnumeratePhysicalDevices");
    std::vector<std::shared_ptr<VulkanBackend>> result;
    for (const auto candidate : physical) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        VkPhysicalDeviceFeatures features{};
        vkGetPhysicalDeviceFeatures(candidate, &features);
        if (!features.shaderInt64) {
            std::cerr << "Vulkan " << properties.deviceName
                      << " lacks shaderInt64; CPU remains available\n";
            continue;
        }
        std::uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        const auto found = std::find_if(families.begin(), families.end(), [](const auto& family) {
            return family.queueCount && (family.queueFlags & VK_QUEUE_COMPUTE_BIT);
        });
        if (found == families.end()) continue;
        auto impl = std::make_unique<Impl>();
        impl->instance = instance;
        impl->physical = candidate;
        impl->queue_family = static_cast<std::uint32_t>(found - families.begin());
        impl->version = version;
        impl->device_name = properties.deviceName;
        VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &ids;
        vkGetPhysicalDeviceProperties2(candidate, &props);
#ifdef _WIN32
        impl->hardware_id = ids.deviceLUIDValid &&
            std::any_of(std::begin(ids.deviceLUID), std::end(ids.deviceLUID),
                        [](auto byte) { return byte != 0; })
            ? uuid_string(ids.deviceLUID, VK_LUID_SIZE, "luid:")
            : uuid_string(ids.deviceUUID);
#else
        impl->hardware_id = uuid_string(ids.deviceUUID);
#endif
        try {
            impl->initialize();
            result.push_back(std::shared_ptr<VulkanBackend>(new VulkanBackend(std::move(impl))));
        } catch (const std::exception& error) {
            std::cerr << "Vulkan " << properties.deviceName << ": "
                      << error.what() << '\n';
        }
    }
    return result;
}

BackendInfo VulkanBackend::info() const {
    return {"Vulkan " + impl_->device_name, ComputeApi::vulkan,
            impl_->hardware_id, __DATE__ " " __TIME__};
}

bool VulkanBackend::self_test() {
    const std::string phrase =
        "abandon abandon abandon abandon abandon abandon abandon abandon "
        "abandon abandon abandon about";
    try {
        auto seed = impl_->derive({phrase}).front();
        const auto gpu_public = public_key_from_ed25519_seed(seed);
        const auto cpu_public = multichain_public_key(phrase);
        OPENSSL_cleanse(seed.data(), seed.size());
        return gpu_public == cpu_public;
    } catch (...) {
        return false;
    }
}

std::uint64_t VulkanBackend::run_batch(
    std::uint32_t size, const SearchPattern& pattern,
    const std::function<void(WalletCandidate&&)>& on_match) {
    std::uint64_t checked = 0;
    while (checked < size) {
        const auto count = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(group_size, size - checked));
        std::vector<std::string> phrases;
        phrases.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
            phrases.push_back(generate_multichain_mnemonic());
        auto seeds = impl_->derive(phrases);
        for (std::size_t i = 0; i < phrases.size(); ++i) {
            WalletCandidate candidate{};
            candidate.version = impl_->version;
            candidate.scheme = MnemonicScheme::multichain12;
            candidate.public_key = public_key_from_ed25519_seed(seeds[i]);
            candidate.address_hash = impl_->version == WalletVersion::v5r1
                ? v5r1_address_hash(candidate.public_key)
                : v4r2_address_hash(candidate.public_key);
            candidate.address = friendly_address(candidate.address_hash);
            if (pattern.matches(candidate.address)) {
                // A GPU result is never exported without independent CPU verification.
                if (multichain_public_key(phrases[i]) != candidate.public_key)
                    throw std::runtime_error("Vulkan candidate failed CPU verification");
                candidate.mnemonic = phrases[i];
                on_match(std::move(candidate));
                OPENSSL_cleanse(candidate.mnemonic.data(), candidate.mnemonic.size());
            }
            OPENSSL_cleanse(seeds[i].data(), seeds[i].size());
            OPENSSL_cleanse(phrases[i].data(), phrases[i].size());
        }
        checked += count;
    }
    return checked;
}
} // namespace vantix
