#pragma once

// Android-only: Vulkan entry points resolved at runtime (volk-style).
//
// VulkanAPI.h defines VK_NO_PROTOTYPES on Android, so vulkan.h declares no
// functions - instead, every entry point below is an inline pointer variable
// with the exact same name, and existing call sites compile unchanged into
// calls through these pointers. vk::load_dynamic_symbols() points them at
// either the system libvulkan or a custom GPU driver handle provided by
// rpcsx-ui-android via adrenotools (_rpcsx_setCustomDriver).
//
// Hidden visibility keeps these variables out of the dynamic symbol table so
// they can never interpose or be interposed by libvulkan.so's real symbols.
//
// The list below is every Vulkan function the code base references; a newly
// used function shows up as a linker error pointing here.

#ifdef ANDROID

#include <dlfcn.h>

#define VK_FOREACH_DYNAMIC_SYMBOL(V) \
	V(vkAcquireNextImageKHR) \
	V(vkAllocateCommandBuffers) \
	V(vkAllocateDescriptorSets) \
	V(vkAllocateMemory) \
	V(vkBeginCommandBuffer) \
	V(vkBindBufferMemory) \
	V(vkBindBufferMemory2) \
	V(vkBindImageMemory) \
	V(vkBindImageMemory2) \
	V(vkCmdBeginQuery) \
	V(vkCmdBeginRenderPass) \
	V(vkCmdBindDescriptorSets) \
	V(vkCmdBindIndexBuffer) \
	V(vkCmdBindPipeline) \
	V(vkCmdBindVertexBuffers) \
	V(vkCmdBlitImage) \
	V(vkCmdClearAttachments) \
	V(vkCmdClearColorImage) \
	V(vkCmdClearDepthStencilImage) \
	V(vkCmdCopyBuffer) \
	V(vkCmdCopyBufferToImage) \
	V(vkCmdCopyImage) \
	V(vkCmdCopyImageToBuffer) \
	V(vkCmdCopyQueryPoolResults) \
	V(vkCmdDispatch) \
	V(vkCmdDraw) \
	V(vkCmdDrawIndexed) \
	V(vkCmdEndQuery) \
	V(vkCmdEndRenderPass) \
	V(vkCmdFillBuffer) \
	V(vkCmdPipelineBarrier) \
	V(vkCmdPushConstants) \
	V(vkCmdResetQueryPool) \
	V(vkCmdSetBlendConstants) \
	V(vkCmdSetDepthBias) \
	V(vkCmdSetDepthBounds) \
	V(vkCmdSetEvent) \
	V(vkCmdSetLineWidth) \
	V(vkCmdSetScissor) \
	V(vkCmdSetStencilCompareMask) \
	V(vkCmdSetStencilReference) \
	V(vkCmdSetStencilWriteMask) \
	V(vkCmdSetViewport) \
	V(vkCmdUpdateBuffer) \
	V(vkCmdWaitEvents) \
	V(vkCreateAndroidSurfaceKHR) \
	V(vkCreateBuffer) \
	V(vkCreateBufferView) \
	V(vkCreateCommandPool) \
	V(vkCreateComputePipelines) \
	V(vkCreateDescriptorPool) \
	V(vkCreateDescriptorSetLayout) \
	V(vkCreateDevice) \
	V(vkCreateEvent) \
	V(vkCreateFence) \
	V(vkCreateFramebuffer) \
	V(vkCreateGraphicsPipelines) \
	V(vkCreateImage) \
	V(vkCreateImageView) \
	V(vkCreateInstance) \
	V(vkCreatePipelineCache) \
	V(vkCreatePipelineLayout) \
	V(vkCreateQueryPool) \
	V(vkCreateRenderPass) \
	V(vkCreateSampler) \
	V(vkCreateSemaphore) \
	V(vkCreateShaderModule) \
	V(vkDestroyBuffer) \
	V(vkDestroyBufferView) \
	V(vkDestroyCommandPool) \
	V(vkDestroyDescriptorPool) \
	V(vkDestroyDescriptorSetLayout) \
	V(vkDestroyDevice) \
	V(vkDestroyEvent) \
	V(vkDestroyFence) \
	V(vkDestroyFramebuffer) \
	V(vkDestroyImage) \
	V(vkDestroyImageView) \
	V(vkDestroyInstance) \
	V(vkDestroyPipeline) \
	V(vkDestroyPipelineCache) \
	V(vkDestroyPipelineLayout) \
	V(vkDestroyQueryPool) \
	V(vkDestroyRenderPass) \
	V(vkDestroySampler) \
	V(vkDestroySemaphore) \
	V(vkDestroyShaderModule) \
	V(vkDestroySurfaceKHR) \
	V(vkDeviceWaitIdle) \
	V(vkEndCommandBuffer) \
	V(vkEnumerateDeviceExtensionProperties) \
	V(vkEnumerateInstanceExtensionProperties) \
	V(vkEnumeratePhysicalDevices) \
	V(vkFlushMappedMemoryRanges) \
	V(vkFreeCommandBuffers) \
	V(vkFreeMemory) \
	V(vkGetBufferMemoryRequirements) \
	V(vkGetBufferMemoryRequirements2) \
	V(vkGetDeviceProcAddr) \
	V(vkGetDeviceQueue) \
	V(vkGetEventStatus) \
	V(vkGetFenceStatus) \
	V(vkGetImageMemoryRequirements) \
	V(vkGetImageMemoryRequirements2) \
	V(vkGetImageSubresourceLayout) \
	V(vkGetInstanceProcAddr) \
	V(vkGetPipelineCacheData) \
	V(vkGetPhysicalDeviceFeatures) \
	V(vkGetPhysicalDeviceFeatures2) \
	V(vkGetPhysicalDeviceFormatProperties) \
	V(vkGetPhysicalDeviceMemoryProperties) \
	V(vkGetPhysicalDeviceMemoryProperties2) \
	V(vkGetPhysicalDeviceProperties) \
	V(vkGetPhysicalDeviceProperties2) \
	V(vkGetPhysicalDeviceQueueFamilyProperties) \
	V(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
	V(vkGetPhysicalDeviceSurfaceFormatsKHR) \
	V(vkGetPhysicalDeviceSurfacePresentModesKHR) \
	V(vkGetPhysicalDeviceSurfaceSupportKHR) \
	V(vkGetQueryPoolResults) \
	V(vkInvalidateMappedMemoryRanges) \
	V(vkMapMemory) \
	V(vkQueueSubmit) \
	V(vkResetCommandBuffer) \
	V(vkResetDescriptorPool) \
	V(vkResetEvent) \
	V(vkResetFences) \
	V(vkSetEvent) \
	V(vkUnmapMemory) \
	V(vkUpdateDescriptorSets) \
	V(vkWaitForFences)

#define VK_DECLARE_DYNAMIC_SYMBOL(x) \
	inline __attribute__((visibility("hidden"))) PFN_##x x = nullptr;
VK_FOREACH_DYNAMIC_SYMBOL(VK_DECLARE_DYNAMIC_SYMBOL)
#undef VK_DECLARE_DYNAMIC_SYMBOL

namespace vk
{
	inline __attribute__((visibility("hidden"))) void* g_dynamic_loader = nullptr;

	// Re-points every entry point at the given loader handle (nullptr means
	// the system libvulkan). Returns the previously active handle.
	inline void* load_dynamic_symbols(void* loader)
	{
		if (loader == nullptr)
		{
			loader = ::dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
		}

		void* const prev = g_dynamic_loader;
		g_dynamic_loader = loader;

#define VK_LOAD_DYNAMIC_SYMBOL(x) x = reinterpret_cast<PFN_##x>(::dlsym(loader, #x));
		VK_FOREACH_DYNAMIC_SYMBOL(VK_LOAD_DYNAMIC_SYMBOL)
#undef VK_LOAD_DYNAMIC_SYMBOL

		return prev;
	}

	inline void ensure_dynamic_symbols()
	{
		if (g_dynamic_loader == nullptr)
		{
			load_dynamic_symbols(nullptr);
		}
	}
} // namespace vk

#endif // ANDROID
