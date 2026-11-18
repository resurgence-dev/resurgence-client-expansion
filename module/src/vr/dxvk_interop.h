#pragma once

#include <cstdint>

#include <d3d9.h>

#include "openvr.h"

/*
	DXVK's Direct3D 9 interop interfaces (src/d3d9/d3d9_interfaces.h in DXVK, zlib license),
	declared here with the few Vulkan types they use, so the module needs no Vulkan SDK. A game
	running on DXVK answers QueryInterface for ID3D9VkInteropDevice; native Direct3D 9 doesn't.
*/
namespace rcx::vr::dxvk
{
	struct VkImage_T;
	using VkImage = VkImage_T*;

	enum VkImageLayout : int32_t
	{
		VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL = 6,
		VK_IMAGE_LAYOUT_MAX_ENUM = 0x7fffffff
	};

	struct VkImageCreateInfo;

	struct VkImageSubresourceRange
	{
		uint32_t aspectMask = 0;
		uint32_t baseMipLevel = 0;
		uint32_t levelCount = 0;
		uint32_t baseArrayLayer = 0;
		uint32_t layerCount = 0;
	};

	constexpr uint32_t kImageAspectColor = 0x1;
	constexpr uint32_t kFormatB8G8R8A8Unorm = 44;

	struct __declspec(uuid("d56344f5-8d35-46fd-806d-94c351b472c1")) ID3D9VkInteropTexture : public IUnknown
	{
		virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* handle, VkImageLayout* layout, VkImageCreateInfo* info) = 0;
	};

	struct __declspec(uuid("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")) ID3D9VkInteropDevice : public IUnknown
	{
		virtual void STDMETHODCALLTYPE GetVulkanHandles(::VkInstance_T** instance, ::VkPhysicalDevice_T** physicalDevice, ::VkDevice_T** device) = 0;
		virtual void STDMETHODCALLTYPE GetSubmissionQueue(::VkQueue_T** queue, uint32_t* queueIndex, uint32_t* queueFamilyIndex) = 0;
		virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture* texture, const VkImageSubresourceRange* subresources, VkImageLayout oldLayout, VkImageLayout newLayout) = 0;
		virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
		virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
		virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
		virtual void STDMETHODCALLTYPE LockDevice() = 0;
		virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
	};
}
