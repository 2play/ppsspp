#include <cstdint>
#include "base/logging.h"

#include "Common/Vulkan/VulkanContext.h"
#include "thin3d/VulkanRenderManager.h"
#include "thread/threadutil.h"

#if 0 // def _DEBUG
#define VLOG ILOG
#else
#define VLOG(...)
#endif

// TODO: Using a thread here is unfinished and does not work correctly.
const bool useThread = false;

#ifndef UINT64_MAX
#define UINT64_MAX 0xFFFFFFFFFFFFFFFFULL
#endif


void CreateImage(VulkanContext *vulkan, VkCommandBuffer cmd, VKRImage &img, int width, int height, VkFormat format, VkImageLayout initialLayout, bool color) {
	VkImageCreateInfo ici{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	ici.arrayLayers = 1;
	ici.mipLevels = 1;
	ici.extent.width = width;
	ici.extent.height = height;
	ici.extent.depth = 1;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.format = format;
	// Strictly speaking we don't yet need VK_IMAGE_USAGE_SAMPLED_BIT for depth buffers since we do not yet sample depth buffers.
	ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	if (color) {
		ici.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	} else {
		ici.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	}

	vkCreateImage(vulkan->GetDevice(), &ici, nullptr, &img.image);

	// TODO: If available, use nVidia's VK_NV_dedicated_allocation for framebuffers

	VkMemoryRequirements memreq;
	vkGetImageMemoryRequirements(vulkan->GetDevice(), img.image, &memreq);

	VkMemoryAllocateInfo alloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	alloc.allocationSize = memreq.size;
	vulkan->MemoryTypeFromProperties(memreq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &alloc.memoryTypeIndex);
	VkResult res = vkAllocateMemory(vulkan->GetDevice(), &alloc, nullptr, &img.memory);
	assert(res == VK_SUCCESS);
	res = vkBindImageMemory(vulkan->GetDevice(), img.image, img.memory, 0);
	assert(res == VK_SUCCESS);

	VkImageAspectFlags aspects = color ? VK_IMAGE_ASPECT_COLOR_BIT : (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);

	VkImageViewCreateInfo ivci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	ivci.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };
	ivci.format = ici.format;
	ivci.image = img.image;
	ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ivci.subresourceRange.aspectMask = aspects;
	ivci.subresourceRange.layerCount = 1;
	ivci.subresourceRange.levelCount = 1;
	res = vkCreateImageView(vulkan->GetDevice(), &ivci, nullptr, &img.imageView);
	assert(res == VK_SUCCESS);

	VkPipelineStageFlags dstStage;
	VkAccessFlagBits dstAccessMask;
	switch (initialLayout) {
	case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
		dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		break;
	case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
		dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		break;
	case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		break;
	default:
		Crash();
		break;
	}

	TransitionImageLayout2(cmd, img.image, aspects,
		VK_IMAGE_LAYOUT_UNDEFINED, initialLayout,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, dstStage,
		0, dstAccessMask);
	img.layout = initialLayout;

	img.format = format;
}

VulkanRenderManager::VulkanRenderManager(VulkanContext *vulkan) : vulkan_(vulkan), queueRunner_(vulkan) {
	VkSemaphoreCreateInfo semaphoreCreateInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	semaphoreCreateInfo.flags = 0;
	VkResult res = vkCreateSemaphore(vulkan_->GetDevice(), &semaphoreCreateInfo, nullptr, &acquireSemaphore_);
	assert(res == VK_SUCCESS);
	res = vkCreateSemaphore(vulkan_->GetDevice(), &semaphoreCreateInfo, nullptr, &renderingCompleteSemaphore_);
	assert(res == VK_SUCCESS);

	for (int i = 0; i < vulkan_->GetInflightFrames(); i++) {
		VkCommandPoolCreateInfo cmd_pool_info = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
		cmd_pool_info.queueFamilyIndex = vulkan_->GetGraphicsQueueFamilyIndex();
		cmd_pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VkResult res = vkCreateCommandPool(vulkan_->GetDevice(), &cmd_pool_info, nullptr, &frameData_[i].cmdPoolInit);
		assert(res == VK_SUCCESS);
		res = vkCreateCommandPool(vulkan_->GetDevice(), &cmd_pool_info, nullptr, &frameData_[i].cmdPoolMain);
		assert(res == VK_SUCCESS);

		VkCommandBufferAllocateInfo cmd_alloc = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
		cmd_alloc.commandPool = frameData_[i].cmdPoolInit;
		cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmd_alloc.commandBufferCount = 1;

		res = vkAllocateCommandBuffers(vulkan_->GetDevice(), &cmd_alloc, &frameData_[i].initCmd);
		assert(res == VK_SUCCESS);
		cmd_alloc.commandPool = frameData_[i].cmdPoolMain;
		res = vkAllocateCommandBuffers(vulkan_->GetDevice(), &cmd_alloc, &frameData_[i].mainCmd);
		assert(res == VK_SUCCESS);
		frameData_[i].fence = vulkan_->CreateFence(true);  // So it can be instantly waited on
	}

	queueRunner_.CreateDeviceObjects();
}

void VulkanRenderManager::CreateBackbuffers() {
	VkResult res = vkGetSwapchainImagesKHR(vulkan_->GetDevice(), vulkan_->GetSwapchain(), &swapchainImageCount_, nullptr);
	assert(res == VK_SUCCESS);

	VkImage* swapchainImages = new VkImage[swapchainImageCount_];
	res = vkGetSwapchainImagesKHR(vulkan_->GetDevice(), vulkan_->GetSwapchain(), &swapchainImageCount_, swapchainImages);
	assert(res == VK_SUCCESS);

	VkCommandBuffer cmdInit = GetInitCmd();

	for (uint32_t i = 0; i < swapchainImageCount_; i++) {
		SwapchainImageData sc_buffer;
		sc_buffer.image = swapchainImages[i];

		VkImageViewCreateInfo color_image_view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		color_image_view.format = vulkan_->GetSwapchainFormat();
		color_image_view.components.r = VK_COMPONENT_SWIZZLE_R;
		color_image_view.components.g = VK_COMPONENT_SWIZZLE_G;
		color_image_view.components.b = VK_COMPONENT_SWIZZLE_B;
		color_image_view.components.a = VK_COMPONENT_SWIZZLE_A;
		color_image_view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		color_image_view.subresourceRange.baseMipLevel = 0;
		color_image_view.subresourceRange.levelCount = 1;
		color_image_view.subresourceRange.baseArrayLayer = 0;
		color_image_view.subresourceRange.layerCount = 1;
		color_image_view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		color_image_view.flags = 0;
		color_image_view.image = sc_buffer.image;

		// Pre-set them to PRESENT_SRC_KHR, as the first thing we do after acquiring
		// in image to render to will be to transition them away from that.
		TransitionImageLayout2(cmdInit, sc_buffer.image,
			VK_IMAGE_ASPECT_COLOR_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
			0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);

		res = vkCreateImageView(vulkan_->GetDevice(), &color_image_view, nullptr, &sc_buffer.view);
		swapchainImages_.push_back(sc_buffer);
		assert(res == VK_SUCCESS);
	}
	delete[] swapchainImages;

	InitDepthStencilBuffer(cmdInit);  // Must be before InitBackbufferRenderPass.
	InitBackbufferFramebuffers(vulkan_->GetBackbufferWidth(), vulkan_->GetBackbufferHeight());
	curWidth_ = -1;
	curHeight_ = -1;

	// Start the thread.
	if (useThread) {
		run_ = true;
		thread_ = std::thread(&VulkanRenderManager::ThreadFunc, this);
	}
}

void VulkanRenderManager::DestroyBackbuffers() {
	if (useThread) {
		run_ = false;
		// Stop the thread.
		for (int i = 0; i < vulkan_->GetInflightFrames(); i++) {
			{
				std::unique_lock<std::mutex> lock(frameData_[i].push_mutex);
				frameData_[i].push_condVar.notify_all();
			}
			{
				std::unique_lock<std::mutex> lock(frameData_[i].pull_mutex);
				frameData_[i].pull_condVar.notify_all();
			}
		}
		thread_.join();
	}
	VkDevice device = vulkan_->GetDevice();
	for (uint32_t i = 0; i < swapchainImageCount_; i++) {
		vulkan_->Delete().QueueDeleteImageView(swapchainImages_[i].view);
	}
	vulkan_->Delete().QueueDeleteImageView(depth_.view);
	vulkan_->Delete().QueueDeleteImage(depth_.image);
	vulkan_->Delete().QueueDeleteDeviceMemory(depth_.mem);
	for (uint32_t i = 0; i < framebuffers_.size(); i++) {
		assert(framebuffers_[i] != VK_NULL_HANDLE);
		vulkan_->Delete().QueueDeleteFramebuffer(framebuffers_[i]);
	}
	framebuffers_.clear();

	swapchainImages_.clear();
}

VulkanRenderManager::~VulkanRenderManager() {
	run_ = false;
	VkDevice device = vulkan_->GetDevice();
	vulkan_->WaitUntilQueueIdle();
	vkDestroySemaphore(device, acquireSemaphore_, nullptr);
	vkDestroySemaphore(device, renderingCompleteSemaphore_, nullptr);
	for (int i = 0; i < vulkan_->GetInflightFrames(); i++) {
		VkCommandBuffer cmdBuf[2]{ frameData_[i].mainCmd, frameData_[i].initCmd };
		vkFreeCommandBuffers(device, frameData_[i].cmdPoolInit, 1, &frameData_[i].initCmd);
		vkFreeCommandBuffers(device, frameData_[i].cmdPoolMain, 1, &frameData_[i].mainCmd);
		vkDestroyCommandPool(device, frameData_[i].cmdPoolInit, nullptr);
		vkDestroyCommandPool(device, frameData_[i].cmdPoolMain, nullptr);
		vkDestroyFence(device, frameData_[i].fence, nullptr);
	}
	queueRunner_.DestroyDeviceObjects();
}

// TODO: Activate this code.
void VulkanRenderManager::ThreadFunc() {
	setCurrentThreadName("RenderMan");
	int threadFrame = -1;  // Increment first, start at 0.
	while (run_) {
		{
			threadFrame++;
			if (threadFrame >= vulkan_->GetInflightFrames())
				threadFrame = 0;
			FrameData &frameData = frameData_[threadFrame];
			std::unique_lock<std::mutex> lock(frameData.pull_mutex);
			while (!frameData.readyForRun && run_) {
				VLOG("PULL: Waiting for frame[%d].readyForRun", threadFrame);
				frameData.pull_condVar.wait(lock);
			}
			VLOG("PULL: frame[%d].readyForRun = false", threadFrame);
			frameData.readyForRun = false;
			if (!run_)  // quick exit if bailing.
				return;
		}
		VLOG("PULL: Running frame %d", threadFrame);
		Run(threadFrame);
	}
}

void VulkanRenderManager::BeginFrame() {
	VkDevice device = vulkan_->GetDevice();

	int curFrame = vulkan_->GetCurFrame();
	FrameData &frameData = frameData_[curFrame];

	// Make sure the very last command buffer from the frame before the previous has been fully executed.
	if (useThread) {
		std::unique_lock<std::mutex> lock(frameData.push_mutex);
		while (!frameData.readyForFence) {
			VLOG("PUSH: Waiting for frame[%d].readyForFence = 1", curFrame);
			frameData.push_condVar.wait(lock);
		}
		frameData.readyForFence = false;
	}

	VLOG("PUSH: Fencing %d", curFrame);
	vkWaitForFences(device, 1, &frameData.fence, true, UINT64_MAX);
	vkResetFences(device, 1, &frameData.fence);

	// Must be after the fence - this performs deletes.
	VLOG("PUSH: BeginFrame %d", curFrame);
	vulkan_->BeginFrame();

	insideFrame_ = true;
}

VkCommandBuffer VulkanRenderManager::GetInitCmd() {
	int curFrame = vulkan_->GetCurFrame();
	FrameData &frameData = frameData_[curFrame];
	if (!frameData.hasInitCommands) {
		VkCommandBufferBeginInfo begin = {
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			nullptr,
			VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
		};
		VkResult res = vkBeginCommandBuffer(frameData.initCmd, &begin);
		assert(res == VK_SUCCESS);
		frameData.hasInitCommands = true;
	}
	return frameData_[curFrame].initCmd;
}

void VulkanRenderManager::BindFramebufferAsRenderTarget(VKRFramebuffer *fb, VKRRenderPassAction color, VKRRenderPassAction depth, uint32_t clearColor, float clearDepth, uint8_t clearStencil) {
	// Eliminate dupes.
	if (steps_.size() && steps_.back()->stepType == VKRStepType::RENDER && steps_.back()->render.framebuffer == fb) {
		if (color != VKRRenderPassAction::CLEAR && depth != VKRRenderPassAction::CLEAR) {
			// We don't move to a new step, this bind was unnecessary.
			return;
		}
	}

	VKRStep *step = new VKRStep{ VKRStepType::RENDER };
	// This is what queues up new passes, and can end previous ones.
	step->render.framebuffer = fb;
	step->render.color = color;
	step->render.depthStencil = depth;
	step->render.clearColor = clearColor;
	step->render.clearDepth = clearDepth;
	step->render.clearStencil = clearStencil;
	step->render.numDraws = 0;
	step->render.finalColorLayout = !fb ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	steps_.push_back(step);

	curRenderStep_ = step;
	curWidth_ = fb ? fb->width : vulkan_->GetBackbufferWidth();
	curHeight_ = fb ? fb->height : vulkan_->GetBackbufferHeight();
}

void VulkanRenderManager::CopyFramebufferToMemorySync(VKRFramebuffer *src, int aspectBits, int x, int y, int w, int h, Draw::DataFormat destFormat, uint8_t *pixels, int pixelStride) {
	VKRStep *step = new VKRStep{ VKRStepType::READBACK };
	step->readback.aspectMask = aspectBits;
	step->readback.src = src;
	step->readback.srcRect.offset = { x, y };
	step->readback.srcRect.extent = { (uint32_t)w, (uint32_t)h };
	steps_.push_back(step);

	curRenderStep_ = nullptr;

	FlushSync();

	// Need to call this after FlushSyfnc so the pixels are guaranteed to be ready in CPU-accessible VRAM.
	queueRunner_.CopyReadbackBuffer(w, h, destFormat, pixelStride, pixels);
}

void VulkanRenderManager::InitBackbufferFramebuffers(int width, int height) {
	VkResult U_ASSERT_ONLY res;
	// We share the same depth buffer but have multiple color buffers, see the loop below.
	VkImageView attachments[2] = { VK_NULL_HANDLE, depth_.view };

	VLOG("InitFramebuffers: %dx%d", width, height);
	VkFramebufferCreateInfo fb_info = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
	fb_info.renderPass = queueRunner_.GetBackbufferRenderPass();
	fb_info.attachmentCount = 2;
	fb_info.pAttachments = attachments;
	fb_info.width = width;
	fb_info.height = height;
	fb_info.layers = 1;

	framebuffers_.resize(swapchainImageCount_);

	for (uint32_t i = 0; i < swapchainImageCount_; i++) {
		attachments[0] = swapchainImages_[i].view;
		res = vkCreateFramebuffer(vulkan_->GetDevice(), &fb_info, nullptr, &framebuffers_[i]);
		assert(res == VK_SUCCESS);
	}
}

void VulkanRenderManager::InitDepthStencilBuffer(VkCommandBuffer cmd) {
	VkResult U_ASSERT_ONLY res;
	bool U_ASSERT_ONLY pass;

	const VkFormat depth_format = vulkan_->GetDeviceInfo().preferredDepthStencilFormat;
	int aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	VkImageCreateInfo image_info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	image_info.imageType = VK_IMAGE_TYPE_2D;
	image_info.format = depth_format;
	image_info.extent.width = vulkan_->GetBackbufferWidth();
	image_info.extent.height = vulkan_->GetBackbufferHeight();
	image_info.extent.depth = 1;
	image_info.mipLevels = 1;
	image_info.arrayLayers = 1;
	image_info.samples = VK_SAMPLE_COUNT_1_BIT;
	image_info.queueFamilyIndexCount = 0;
	image_info.pQueueFamilyIndices = nullptr;
	image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	image_info.flags = 0;

	VkMemoryAllocateInfo mem_alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	mem_alloc.allocationSize = 0;
	mem_alloc.memoryTypeIndex = 0;

	VkMemoryRequirements mem_reqs;

	depth_.format = depth_format;

	VkDevice device = vulkan_->GetDevice();
	res = vkCreateImage(device, &image_info, NULL, &depth_.image);
	assert(res == VK_SUCCESS);

	vkGetImageMemoryRequirements(device, depth_.image, &mem_reqs);

	mem_alloc.allocationSize = mem_reqs.size;
	// Use the memory properties to determine the type of memory required
	pass = vulkan_->MemoryTypeFromProperties(mem_reqs.memoryTypeBits,
		0, /* No requirements */
		&mem_alloc.memoryTypeIndex);
	assert(pass);

	res = vkAllocateMemory(device, &mem_alloc, NULL, &depth_.mem);
	assert(res == VK_SUCCESS);

	res = vkBindImageMemory(device, depth_.image, depth_.mem, 0);
	assert(res == VK_SUCCESS);

	TransitionImageLayout2(cmd, depth_.image,
		aspectMask,
		VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,
		0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

	VkImageViewCreateInfo depth_view_info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	depth_view_info.image = depth_.image;
	depth_view_info.format = depth_format;
	depth_view_info.components.r = VK_COMPONENT_SWIZZLE_R;
	depth_view_info.components.g = VK_COMPONENT_SWIZZLE_G;
	depth_view_info.components.b = VK_COMPONENT_SWIZZLE_B;
	depth_view_info.components.a = VK_COMPONENT_SWIZZLE_A;
	depth_view_info.subresourceRange.aspectMask = aspectMask;
	depth_view_info.subresourceRange.baseMipLevel = 0;
	depth_view_info.subresourceRange.levelCount = 1;
	depth_view_info.subresourceRange.baseArrayLayer = 0;
	depth_view_info.subresourceRange.layerCount = 1;
	depth_view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	depth_view_info.flags = 0;

	res = vkCreateImageView(device, &depth_view_info, NULL, &depth_.view);
	assert(res == VK_SUCCESS);
}

void VulkanRenderManager::Clear(uint32_t clearColor, float clearZ, int clearStencil, int clearMask) {
	_dbg_assert_(G3D, curRenderStep_ && curRenderStep_->stepType == VKRStepType::RENDER);
	// If this is the first drawing command, merge it into the pass.
	if (curRenderStep_->render.numDraws == 0) {
		curRenderStep_->render.clearColor = clearColor;
		curRenderStep_->render.clearDepth = clearZ;
		curRenderStep_->render.clearStencil = clearStencil;
		curRenderStep_->render.color = (clearMask & VK_IMAGE_ASPECT_COLOR_BIT) ? VKRRenderPassAction::CLEAR : VKRRenderPassAction::KEEP;
		curRenderStep_->render.depthStencil = (clearMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) ? VKRRenderPassAction::CLEAR : VKRRenderPassAction::KEEP;
	} else {
		VkRenderData data{ VKRRenderCommand::CLEAR };
		data.clear.clearColor = clearColor;
		data.clear.clearZ = clearZ;
		data.clear.clearStencil = clearStencil;
		data.clear.clearMask = clearMask;
		curRenderStep_->commands.push_back(data);
	}
}

void VulkanRenderManager::CopyFramebuffer(VKRFramebuffer *src, VkRect2D srcRect, VKRFramebuffer *dst, VkOffset2D dstPos, int aspectMask) {
	_dbg_assert_msg_(G3D, srcRect.offset.x >= 0, "srcrect offset x < 0");
	_dbg_assert_msg_(G3D, srcRect.offset.y >= 0, "srcrect offset y < 0");
	_dbg_assert_msg_(G3D, srcRect.offset.x + srcRect.extent.width <= (uint32_t)src->width, "srcrect offset x + extent > width");
	_dbg_assert_msg_(G3D, srcRect.offset.y + srcRect.extent.height <= (uint32_t)src->height, "srcrect offset y + extent > height");
	_dbg_assert_msg_(G3D, dstPos.x >= 0, "dstPos offset x < 0");
	_dbg_assert_msg_(G3D, dstPos.y >= 0, "dstPos offset y < 0");
	_dbg_assert_msg_(G3D, dstPos.x + srcRect.extent.width <= (uint32_t)dst->width, "dstPos + extent x > width");
	_dbg_assert_msg_(G3D, dstPos.y + srcRect.extent.height <= (uint32_t)dst->height, "dstPos + extent y > height");

	VKRStep *step = new VKRStep{ VKRStepType::COPY };

	step->copy.aspectMask = aspectMask;
	step->copy.src = src;
	step->copy.srcRect = srcRect;
	step->copy.dst = dst;
	step->copy.dstPos = dstPos;

	std::unique_lock<std::mutex> lock(mutex_);
	steps_.push_back(step);
	curRenderStep_ = nullptr;
}

void VulkanRenderManager::BlitFramebuffer(VKRFramebuffer *src, VkRect2D srcRect, VKRFramebuffer *dst, VkRect2D dstRect, int aspectMask, VkFilter filter) {
	_dbg_assert_msg_(G3D, srcRect.offset.x >= 0, "srcrect offset x < 0");
	_dbg_assert_msg_(G3D, srcRect.offset.y >= 0, "srcrect offset y < 0");
	_dbg_assert_msg_(G3D, srcRect.offset.x + srcRect.extent.width <= (uint32_t)src->width, "srcrect offset x + extent > width");
	_dbg_assert_msg_(G3D, srcRect.offset.y + srcRect.extent.height <= (uint32_t)src->height, "srcrect offset y + extent > height");

	_dbg_assert_msg_(G3D, dstRect.offset.x >= 0, "dstrect offset x < 0");
	_dbg_assert_msg_(G3D, dstRect.offset.y >= 0, "dstrect offset y < 0");
	_dbg_assert_msg_(G3D, dstRect.offset.x + dstRect.extent.width <= (uint32_t)dst->width, "dstrect offset x + extent > width");
	_dbg_assert_msg_(G3D, dstRect.offset.y + dstRect.extent.height <= (uint32_t)dst->height, "dstrect offset y + extent > height");

	VKRStep *step = new VKRStep{ VKRStepType::BLIT };

	step->blit.aspectMask = aspectMask;
	step->blit.src = src;
	step->blit.srcRect = srcRect;
	step->blit.dst = dst;
	step->blit.dstRect = dstRect;
	step->blit.filter = filter;

	std::unique_lock<std::mutex> lock(mutex_);
	steps_.push_back(step);
	curRenderStep_ = nullptr;
}

VkImageView VulkanRenderManager::BindFramebufferAsTexture(VKRFramebuffer *fb, int binding, int aspectBit, int attachment) {
	// Should just mark the dependency and return the image.
	for (int i = (int)steps_.size() - 1; i >= 0; i--) {
		if (steps_[i]->stepType == VKRStepType::RENDER && steps_[i]->render.framebuffer == fb) {
			// If this framebuffer was rendered to earlier in this frame, make sure to pre-transition it to the correct layout.
			if (steps_[i]->render.finalColorLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
				steps_[i]->render.finalColorLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				break;
			}
			else if (steps_[i]->render.finalColorLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
				Crash();
				// May need to shadow the framebuffer if we re-order passes later.
			}
		}
	}

	curRenderStep_->preTransitions.push_back({ fb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
	return fb->color.imageView;
}

void VulkanRenderManager::Finish() {
	curRenderStep_ = nullptr;
	int curFrame = vulkan_->GetCurFrame();
	FrameData &frameData = frameData_[curFrame];
	if (!useThread) {
		frameData.steps = std::move(steps_);
		Run(curFrame);
	} else {
		std::unique_lock<std::mutex> lock(frameData.pull_mutex);
		VLOG("PUSH: Frame[%d].readyForRun = true", curFrame);
		frameData.steps = std::move(steps_);
		frameData.readyForRun = true;
		frameData.pull_condVar.notify_all();
	}
	vulkan_->EndFrame();
}

// Can be called multiple times with no bad side effects. This is so that we can either begin a frame the normal way,
void VulkanRenderManager::BeginSubmitFrame(int frame) {
	FrameData &frameData = frameData_[frame];
	if (!frameData.hasBegun) {
		// Get the index of the next available swapchain image, and a semaphore to block command buffer execution on.
		// Now, I wonder if we should do this early in the frame or late? Right now we do it early, which should be fine.
		VkResult res = vkAcquireNextImageKHR(vulkan_->GetDevice(), vulkan_->GetSwapchain(), UINT64_MAX, acquireSemaphore_, (VkFence)VK_NULL_HANDLE, &frameData.curSwapchainImage);
		assert(res == VK_SUCCESS);
		// TODO: Deal with the VK_SUBOPTIMAL_KHR and VK_ERROR_OUT_OF_DATE_KHR
		// return codes

		VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		res = vkBeginCommandBuffer(frameData.mainCmd, &begin);

		assert(res == VK_SUCCESS);

		// TODO: Is it best to do this here, or combine with some other transition, or just do it right before the backbuffer bind-for-render?
		TransitionFromPresent(frameData.mainCmd, swapchainImages_[frameData.curSwapchainImage].image);

		queueRunner_.SetBackbuffer(framebuffers_[frameData.curSwapchainImage]);

		frameData.hasBegun = true;
	}
}

void VulkanRenderManager::Submit(int frame, bool triggerFence) {
	FrameData &frameData = frameData_[frame];
	if (frameData.hasInitCommands) {
		VkResult res = vkEndCommandBuffer(frameData.initCmd);
		assert(res == VK_SUCCESS);
	}

	VkResult res = vkEndCommandBuffer(frameData.mainCmd);
	assert(res == VK_SUCCESS);

	int numCmdBufs = 0;
	std::vector<VkCommandBuffer> cmdBufs;
	cmdBufs.reserve(2);
	if (frameData.hasInitCommands) {
		cmdBufs.push_back(frameData.initCmd);
		frameData.hasInitCommands = false;
	}

	cmdBufs.push_back(frameData.mainCmd);

	VkSubmitInfo submit_info = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	if (triggerFence) {
		submit_info.waitSemaphoreCount = 1;
		submit_info.pWaitSemaphores = &acquireSemaphore_;
	}

	VkPipelineStageFlags waitStage[1] = { VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT };
	submit_info.pWaitDstStageMask = waitStage;
	submit_info.commandBufferCount = (uint32_t)cmdBufs.size();
	submit_info.pCommandBuffers = cmdBufs.data();
	if (triggerFence) {
		submit_info.signalSemaphoreCount = 1;
		submit_info.pSignalSemaphores = &renderingCompleteSemaphore_;
	}
	res = vkQueueSubmit(vulkan_->GetGraphicsQueue(), 1, &submit_info, triggerFence ? frameData.fence : VK_NULL_HANDLE);
	assert(res == VK_SUCCESS);

	if (useThread) {
		VLOG("PULL: Frame %d.readyForFence = true", frame);
		std::unique_lock<std::mutex> lock(frameData.push_mutex);
		frameData.readyForFence = true;
		frameData.push_condVar.notify_all();
	}
}

void VulkanRenderManager::EndSubmitFrame(int frame) {
	FrameData &frameData = frameData_[frame];
	frameData.hasBegun = false;
	insideFrame_ = false;

	TransitionToPresent(frameData.mainCmd, swapchainImages_[frameData.curSwapchainImage].image);

	Submit(frame, true);

	VkSwapchainKHR swapchain = vulkan_->GetSwapchain();
	VkPresentInfoKHR present = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	present.swapchainCount = 1;
	present.pSwapchains = &swapchain;
	present.pImageIndices = &frameData.curSwapchainImage;
	present.pWaitSemaphores = &renderingCompleteSemaphore_;
	present.waitSemaphoreCount = 1;
	present.pResults = nullptr;

	VkResult res = vkQueuePresentKHR(vulkan_->GetGraphicsQueue(), &present);
	// TODO: Deal with the VK_SUBOPTIMAL_WSI and VK_ERROR_OUT_OF_DATE_WSI
	// return codes
	if (res == VK_ERROR_OUT_OF_DATE_KHR) {
		// ignore, it'll be fine. this happens sometimes during resizes, and we do make sure to recreate the swap chain.
	} else {
		assert(res == VK_SUCCESS);
	}
}

void VulkanRenderManager::Run(int frame) {
	VkDevice device = vulkan_->GetDevice();

	BeginSubmitFrame(frame);

	FrameData &frameData = frameData_[frame];
	auto &stepsOnThread = frameData_[frame].steps;
	VkCommandBuffer cmd = frameData.mainCmd;
	queueRunner_.RunSteps(cmd, stepsOnThread);
	stepsOnThread.clear();

	EndSubmitFrame(frame);


	VLOG("PULL: Finished running frame %d", frame);
}

void VulkanRenderManager::FlushSync() {
	int frame = vulkan_->GetCurFrame();
	BeginSubmitFrame(frame);

	FrameData &frameData = frameData_[frame];
	frameData.steps = std::move(steps_);

	auto &stepsOnThread = frameData_[frame].steps;
	VkCommandBuffer cmd = frameData.mainCmd;
	queueRunner_.RunSteps(cmd, stepsOnThread);
	stepsOnThread.clear();

	Submit(frame, false);

	vkDeviceWaitIdle(vulkan_->GetDevice());

	// At this point we can resume filling the command buffers for the current frame since
	// we know the device is idle - and thus all previously enqueued command buffers have been processed.
	// No need to switch to the next frame number.
	VkCommandBufferBeginInfo begin{
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		nullptr,
		VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};
	VkResult res = vkBeginCommandBuffer(frameData.mainCmd, &begin);
	assert(res == VK_SUCCESS);
}
