#include "common.h"

#include "declarations.h"

//TODO
//compute shaders need kernel support

VKAPI_ATTR VkResult VKAPI_CALL RPIFUNC(vkCreateComputePipelines)(
	VkDevice                                    device,
	VkPipelineCache                             pipelineCache,
	uint32_t                                    createInfoCount,
	const VkComputePipelineCreateInfo*          pCreateInfos,
	const VkAllocationCallbacks*                pAllocator,
	VkPipeline*                                 pPipelines)
{
	UNSUPPORTED(vkCreateComputePipelines);
	return UNSUPPORTED_RETURN;
}

VKAPI_ATTR void VKAPI_CALL RPIFUNC(vkCmdDispatchIndirect)(
	VkCommandBuffer                             commandBuffer,
	VkBuffer                                    buffer,
	VkDeviceSize                                offset)
{
	UNSUPPORTED(vkCmdDispatchIndirect);
}

VKAPI_ATTR void VKAPI_CALL RPIFUNC(vkCmdDispatch)(
	VkCommandBuffer                             commandBuffer,
	uint32_t                                    groupCountX,
	uint32_t                                    groupCountY,
	uint32_t                                    groupCountZ)
{
	UNSUPPORTED(vkCmdDispatch);
}

VKAPI_ATTR void VKAPI_CALL RPIFUNC(vkCmdDispatchBase)(
	VkCommandBuffer                             commandBuffer,
	uint32_t                                    baseGroupX,
	uint32_t                                    baseGroupY,
	uint32_t                                    baseGroupZ,
	uint32_t                                    groupCountX,
	uint32_t                                    groupCountY,
	uint32_t                                    groupCountZ)
{
	UNSUPPORTED(vkCmdDispatchBase);
}
