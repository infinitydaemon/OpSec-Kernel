#include "common.h"

#include "declarations.h"

#include "kernel/vc4_packet.h"
#include "../QPUassembler/qpu_assembler.h"

/*
 * https://www.khronos.org/registry/vulkan/specs/1.1-extensions/html/vkspec.html#vkCmdBindPipeline
 */
void RPIFUNC(vkCmdBindPipeline)(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline)
{
	PROFILESTART(RPIFUNC(vkCmdBindPipeline));

	assert(commandBuffer);

	_commandBuffer* cb = commandBuffer;
	if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS)
	{
		cb->graphicsPipeline = pipeline;
	}
	else if(pipelineBindPoint == VK_PIPELINE_BIND_POINT_COMPUTE)
	{
		cb->computePipeline = pipeline;
	}

	//TODO check that dynamic states are respected around the driver

	PROFILEEND(RPIFUNC(vkCmdBindPipeline));
}

/**
//multiple attachments
void patchShaderDepthStencilBlending(uint64_t** instructions, uint32_t* size, const VkPipelineDepthStencilStateCreateInfo* dsi, const VkPipelineColorBlendAttachmentState* bas, const VkAllocationCallbacks* pAllocator)
{
	assert(instructions);
	assert(size);
	assert(dsi);

	uint32_t numExtraInstructions = 0;
	numExtraInstructions += dsi->depthWriteEnable || dsi->stencilTestEnable;

	uint32_t values[3];
	uint32_t numValues;
	encodeStencilValue(values, &numValues, dsi->front, dsi->back, dsi->stencilTestEnable);

	numExtraInstructions += numValues * 2;

	uint32_t newSize = *size + numExtraInstructions * sizeof(uint64_t);
	uint64_t* tmp = ALLOCATE(newSize, 1, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
	memset(tmp, 0, newSize);
	memcpy(tmp + numExtraInstructions, *instructions, *size);

	///"sig_load_imm ; r0 = load32.always(0xF497EEFF‬) ; nop = load32() ;" //stencil setup state
	///"sig_none ; tlb_stencil_setup = or.always(r0, r0) ; nop = nop(r0, r0) ;"
	for(uint32_t c = 0; c < numValues; ++c)
	{
		tmp[c] = encode_load_imm(0, 0, 1, 0, 0, 0, 32 + c, 39, values[c]); //r0 = load32.always(values[c])
		tmp[numValues + c] = encode_alu(1, 0, 0, 0, 1, 0, 0, 0, 43, 39, 21, 0, 0, 0, c, c, 0, 0); //tlb_stencil_setup = or.always(r0, r0)
	}

	///"sig_none ; tlb_z = or.always(b, b, nop, rb15) ; nop = nop(r0, r0) ;"
	if(dsi->depthWriteEnable || dsi->stencilTestEnable)
	{
		tmp[numValues*2] = encode_alu(1, 0, 0, 0, 1, 0, 0, 0, 44, 39, 21, 0, 0, 15, 7, 7, 0, 0);
	}


	//account for MSAA state!
	//patch blending
	//optimise

	if(bas->blendEnable)
	{
		/// find last instruction that wrote to tlb_color_all

		/// patch shader so that r0 will contain whatever would be written to tlb_color_all
		/// r0 contains sRGBA
		//"sig_none ; r0 = or.always(a, a, uni, nop) ; nop = nop(r0, r0) ;"

		uint64_t instruction;

		/// load dRGBA to r1
		/// load tbl color dRGBA to r4
		assemble_qpu_asm("sig_color_load ; nop = nop(r0, r0) ; nop = nop(r0, r0) ;", &instruction);
		assemble_qpu_asm("sig_none ; r1 = or.always(r4, r4) ; nop = nop(r0, r0) ;", &instruction);

		//if factors are not separate
		if(bas->srcAlphaBlendFactor == bas->srcColorBlendFactor &&
		   bas->dstAlphaBlendFactor == bas->dstColorBlendFactor)
		{
			switch(bas->srcAlphaBlendFactor)
			{
			case VK_BLEND_FACTOR_ZERO:
				assemble_qpu_asm("sig_small_imm ; r2 = or.always(b, b, nop, 0) ; nop = nop(r0, r0) ;", &instruction);
				break;
			case VK_BLEND_FACTOR_ONE:
				assemble_qpu_asm("sig_small_imm ; r2 = or.always(b, b, nop, -1) ; nop = nop(r0, r0) ;", &instruction);
				break;
			case VK_BLEND_FACTOR_SRC_COLOR:
				assemble_qpu_asm("sig_none ; r2 = or.always(r0, r0) ; nop = nop(r0, r0) ;", &instruction);
				break;
			case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
				assemble_qpu_asm("sig_none ; r2 = not.always(r0, r0) ; nop = nop(r0, r0) ;", &instruction);
				break;
			case VK_BLEND_FACTOR_DST_COLOR:
				assemble_qpu_asm("sig_none ; r2 = or.always(r1, r1) ; nop = nop(r0, r0) ;", &instruction);
				break;
			case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
				assemble_qpu_asm("sig_none ; r2 = not.always(r1, r1) ; nop = nop(r0, r0) ;", &instruction);
				break;
			case VK_BLEND_FACTOR_SRC_ALPHA:
				assemble_qpu_asm("sig_none ; r2.8888 = or.always.8d(r0, r0) ; nop = nop(r0, r0) ;", &instruction);
				break;
			case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
				assemble_qpu_asm("sig_none ; r2.8888 = or.always.8d(r0, r0) ; nop = nop(r0, r0) ;", &instruction);
				assemble_qpu_asm("sig_none ; r2 = not.always(r2, r2) ; nop = nop(r0, r0) ;", &instruction);
			case VK_BLEND_FACTOR_DST_ALPHA:
				assemble_qpu_asm("sig_none ; r2.8888 = or.always.8d(r1, r1) ; nop = nop(r0, r0) ;", &instruction);
				break;
			case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
				assemble_qpu_asm("sig_none ; r2.8888 = or.always.8d(r1, r1) ; nop = nop(r0, r0) ;", &instruction);
				assemble_qpu_asm("sig_none ; r2 = not.always(r2, r2) ; nop = nop(r0, r0) ;", &instruction);
				break;
			case VK_BLEND_FACTOR_CONSTANT_COLOR:
			case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
			case VK_BLEND_FACTOR_CONSTANT_ALPHA:
			case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
				assemble_qpu_asm("sig_load_imm ; r2 = load32.always(0xffffffff) ; nop = load32() ;", &instruction);
				break;
			case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
				assemble_qpu_asm("sig_none ; r2.8888 = or.always.8d(r0, r0) ; nop = nop(r0, r0) ;", &instruction); //sAAAA
				assemble_qpu_asm("sig_none ; r3.8888 = or.always.8d(r1, r1) ; nop = nop(r0, r0) ;", &instruction); //dAAAA
				assemble_qpu_asm("sig_none ; r3 = not.always(r3, r3) ; nop = nop(r0, r0) ;", &instruction); //1-dAAAA
				assemble_qpu_asm("sig_none ; nop = nop(r0, r0) ; r2 = v8min.always(r2, r3) ;", &instruction); //min(sAAAA, 1-dAAAA)
				assemble_qpu_asm("sig_load_imm ; r3 = load32.always(0xff000000) ; nop = load32() ;", &instruction); //load alpha = 1
				assemble_qpu_asm("sig_small_imm ; r2 = or.always(r2, r3) ; nop = nop(r0, r0) ;", &instruction); //set alpha to 1
				break;
			}

			/// Multiply sRGBA and source factor
			assemble_qpu_asm("sig_none ; nop = nop(r0, r0) ; r0 = v8muld.always(r0, r2) ;", &instruction);

			///repeat for
			//bas->dstAlphaBlendFactor

			/// Multiply dRGBA and destination factor
			assemble_qpu_asm("sig_none ; nop = nop(r0, r0) ; r1 = v8muld.always(r1, r2) ;", &instruction);
		}
		else //separate factors
		{
			//
		}

		switch(bas->alphaBlendOp)
		{
		case VK_BLEND_OP_ADD:
			assemble_qpu_asm("sig_none ; nop = nop(r0, r0) ; tlb_color_all = v8adds.always(r0, r1) ;", &instruction);
			break;
		case VK_BLEND_OP_SUBTRACT:
			assemble_qpu_asm("sig_none ; nop = nop(r0, r0) ; tlb_color_all = v8subs.always(r0, r1) ;", &instruction);
			break;
		case VK_BLEND_OP_REVERSE_SUBTRACT:
			assemble_qpu_asm("sig_none ; nop = nop(r0, r0) ; tlb_color_all = v8subs.always(r1, r0) ;", &instruction);
			break;
		case VK_BLEND_OP_MIN:
			assemble_qpu_asm("sig_none ; nop = nop(r0, r0) ; tlb_color_all = v8min.always(r0, r1) ;", &instruction);
			break;
		case VK_BLEND_OP_MAX:
			assemble_qpu_asm("sig_none ; nop = nop(r0, r0) ; tlb_color_all = v8max.always(r0, r1) ;", &instruction);
			break;
		}
	}

	//replace instructions pointer
	FREE(*instructions);
	*instructions = tmp;
	*size = newSize;
}
/**/

/*
 * https://www.khronos.org/registry/vulkan/specs/1.1-extensions/html/vkspec.html#vkCreateGraphicsPipelines
 */
VkResult RPIFUNC(vkCreateGraphicsPipelines)(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines)
{
	PROFILESTART(RPIFUNC(vkCreateGraphicsPipelines));

	assert(device);
	assert(createInfoCount > 0);
	assert(pCreateInfos);
	assert(pPipelines);

	if(pipelineCache)
	{
		UNSUPPORTED(pipelineCache);
	}

	//TODO flags

	for(uint32_t c = 0; c < createInfoCount; ++c)
	{
		_pipeline* pip = ALLOCATE(sizeof(_pipeline), 1, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
		if(!pip)
		{
			PROFILEEND(RPIFUNC(vkCreateGraphicsPipelines));
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		memset(pip->names, 0, sizeof(char*)*6);
		memset(pip->modules, 0, sizeof(_shaderModule*)*6);

		for(uint32_t d = 0; d < pCreateInfos[c].stageCount; ++d)
		{
			uint32_t idx = ulog2(pCreateInfos[c].pStages[d].stage);
			pip->modules[idx] = pCreateInfos[c].pStages[d].module;

			_shaderModule* s = pip->modules[idx];

			pip->names[idx] = ALLOCATE(strlen(pCreateInfos[c].pStages[d].pName)+1, 1, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
			if(!pip->names[idx])
			{
				PROFILEEND(RPIFUNC(vkCreateGraphicsPipelines));
				return VK_ERROR_OUT_OF_HOST_MEMORY;
			}

			memcpy(pip->names[idx], pCreateInfos[c].pStages[d].pName, strlen(pCreateInfos[c].pStages[d].pName)+1);

			//patch fragment shader
//			if(pCreateInfos[c].pStages[d].stage & VK_SHADER_STAGE_FRAGMENT_BIT)
//			{
//				//we could patch the fragment shader, but it would have a lot of edge cases
//				//since the user is writing assembly we can just let them have full control
//				//patchShaderDepthStencilBlending(&s->instructions[RPI_ASSEMBLY_TYPE_FRAGMENT], &s->sizes[RPI_ASSEMBLY_TYPE_FRAGMENT], pCreateInfos[c].pDepthStencilState, pCreateInfos[c].pColorBlendState->pAttachments, pAllocator);

//				//if debug...
//				for(uint64_t e = 0; e < s->sizes[RPI_ASSEMBLY_TYPE_FRAGMENT] / 8; ++e)
//				{
//					printf("%#llx ", s->instructions[RPI_ASSEMBLY_TYPE_FRAGMENT][e]);
//					disassemble_qpu_asm(s->instructions[RPI_ASSEMBLY_TYPE_FRAGMENT][e]);
//				}
//				printf("\n");

//				s->bos[RPI_ASSEMBLY_TYPE_FRAGMENT] = vc4_bo_alloc_shader(controlFd, s->instructions[RPI_ASSEMBLY_TYPE_FRAGMENT], &s->sizes[RPI_ASSEMBLY_TYPE_FRAGMENT]);
//			}

//			if(pCreateInfos[c].pStages[d].stage & VK_SHADER_STAGE_VERTEX_BIT)
//			{
//				//if debug...
//				for(uint64_t e = 0; e < s->sizes[RPI_ASSEMBLY_TYPE_VERTEX] / 8; ++e)
//				{
//					printf("%#llx ", s->instructions[RPI_ASSEMBLY_TYPE_VERTEX][e]);
//					disassemble_qpu_asm(s->instructions[RPI_ASSEMBLY_TYPE_VERTEX][e]);
//				}
//				printf("\n");

//				for(uint64_t e = 0; e < s->sizes[RPI_ASSEMBLY_TYPE_COORDINATE] / 8; ++e)
//				{
//					printf("%#llx ", s->instructions[RPI_ASSEMBLY_TYPE_COORDINATE][e]);
//					disassemble_qpu_asm(s->instructions[RPI_ASSEMBLY_TYPE_COORDINATE][e]);
//				}
//				printf("\n");

//				s->bos[RPI_ASSEMBLY_TYPE_COORDINATE] = vc4_bo_alloc_shader(controlFd, s->instructions[RPI_ASSEMBLY_TYPE_COORDINATE], &s->sizes[RPI_ASSEMBLY_TYPE_COORDINATE]);
//				s->bos[RPI_ASSEMBLY_TYPE_VERTEX] = vc4_bo_alloc_shader(controlFd, s->instructions[RPI_ASSEMBLY_TYPE_VERTEX], &s->sizes[RPI_ASSEMBLY_TYPE_VERTEX]);
//			}
		}

		pip->vertexAttributeDescriptionCount = pCreateInfos[c].pVertexInputState->vertexAttributeDescriptionCount;
		pip->vertexAttributeDescriptions = ALLOCATE(sizeof(VkVertexInputAttributeDescription) * pip->vertexAttributeDescriptionCount, 1, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
		if(!pip->vertexAttributeDescriptions)
		{
			PROFILEEND(RPIFUNC(vkCreateGraphicsPipelines));
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		memcpy(pip->vertexAttributeDescriptions, pCreateInfos[c].pVertexInputState->pVertexAttributeDescriptions, sizeof(VkVertexInputAttributeDescription) * pip->vertexAttributeDescriptionCount);

		pip->vertexBindingDescriptionCount = pCreateInfos[c].pVertexInputState->vertexBindingDescriptionCount;
		pip->vertexBindingDescriptions = ALLOCATE(sizeof(VkVertexInputBindingDescription) * pip->vertexBindingDescriptionCount, 1, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
		if(!pip->vertexBindingDescriptions)
		{
			PROFILEEND(RPIFUNC(vkCreateGraphicsPipelines));
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		memcpy(pip->vertexBindingDescriptions, pCreateInfos[c].pVertexInputState->pVertexBindingDescriptions, sizeof(VkVertexInputBindingDescription) * pip->vertexBindingDescriptionCount);

		pip->topology = pCreateInfos[c].pInputAssemblyState->topology;
		pip->primitiveRestartEnable = pCreateInfos[c].pInputAssemblyState->primitiveRestartEnable;

		//tessellation ignored

		uint32_t ignoreViewports = 0,
				ignoreScissors = 0,
				ignoreLineWidth = 0,
				ignoreDepthBias = 0,
				ignoreBlendConstants = 0,
				ignoreDepthBounds = 0,
				ignoreStencilCompareMask = 0,
				ignoreStencilWriteMask = 0,
				ignoreStencilReference = 0
				;
		if(pCreateInfos[c].pDynamicState)
		{
			for(uint32_t d = 0; d < pCreateInfos[c].pDynamicState->dynamicStateCount; ++d)
			{
				if(pCreateInfos[c].pDynamicState->pDynamicStates[d] == VK_DYNAMIC_STATE_VIEWPORT)
				{
					ignoreViewports = 1;
				}
				else if(pCreateInfos[c].pDynamicState->pDynamicStates[d] == VK_DYNAMIC_STATE_SCISSOR)
				{
					ignoreScissors = 1;
				}
				else if(pCreateInfos[c].pDynamicState->pDynamicStates[d] == VK_DYNAMIC_STATE_LINE_WIDTH)
				{
					ignoreLineWidth = 1;
				}
				else if(pCreateInfos[c].pDynamicState->pDynamicStates[d] == VK_DYNAMIC_STATE_DEPTH_BIAS)
				{
					ignoreDepthBias = 1;
				}
				else if(pCreateInfos[c].pDynamicState->pDynamicStates[d] == VK_DYNAMIC_STATE_BLEND_CONSTANTS)
				{
					ignoreBlendConstants = 1;
				}
				else if(pCreateInfos[c].pDynamicState->pDynamicStates[d] == VK_DYNAMIC_STATE_DEPTH_BOUNDS)
				{
					ignoreDepthBounds = 1;
				}
				else if(pCreateInfos[c].pDynamicState->pDynamicStates[d] == VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)
				{
					ignoreStencilCompareMask = 1;
				}
				else if(pCreateInfos[c].pDynamicState->pDynamicStates[d] == VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)
				{
					ignoreStencilWriteMask = 1;
				}
				else if(pCreateInfos[c].pDynamicState->pDynamicStates[d] == VK_DYNAMIC_STATE_STENCIL_REFERENCE)
				{
					ignoreStencilReference = 1;
				}
			}
		}

		pip->viewportCount = pCreateInfos[c].pViewportState->viewportCount;
		pip->viewports = ALLOCATE(sizeof(VkViewport) * pip->viewportCount, 1, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
		if(!pip->viewports)
		{
			PROFILEEND(RPIFUNC(vkCreateGraphicsPipelines));
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		if(!ignoreViewports)
		{
			memcpy(pip->viewports, pCreateInfos[c].pViewportState->pViewports, sizeof(VkViewport) * pip->viewportCount);
		}

		pip->scissorCount = pCreateInfos[c].pViewportState->scissorCount;
		pip->scissors = ALLOCATE(sizeof(VkRect2D) * pip->viewportCount, 1, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

		if(!pip->scissors)
		{
			PROFILEEND(RPIFUNC(vkCreateGraphicsPipelines));
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		if(!ignoreScissors)
		{
			memcpy(pip->scissors, pCreateInfos[c].pViewportState->pScissors, sizeof(VkRect2D) * pip->scissorCount);
		}

		pip->depthClampEnable = pCreateInfos[c].pRasterizationState->depthClampEnable;
		pip->rasterizerDiscardEnable = pCreateInfos[c].pRasterizationState->rasterizerDiscardEnable;
		pip->polygonMode = pCreateInfos[c].pRasterizationState->polygonMode;
		pip->cullMode = pCreateInfos[c].pRasterizationState->cullMode;
		pip->frontFace = pCreateInfos[c].pRasterizationState->frontFace;
		pip->depthBiasEnable = pCreateInfos[c].pRasterizationState->depthBiasEnable;
		if(!ignoreDepthBias)
		{
			pip->depthBiasConstantFactor = pCreateInfos[c].pRasterizationState->depthBiasConstantFactor;
			pip->depthBiasClamp = pCreateInfos[c].pRasterizationState->depthBiasClamp;
			pip->depthBiasSlopeFactor = pCreateInfos[c].pRasterizationState->depthBiasSlopeFactor;
		}
		if(!ignoreLineWidth)
		{
			pip->lineWidth = pCreateInfos[c].pRasterizationState->lineWidth;
		}

		pip->rasterizationSamples = pCreateInfos[c].pMultisampleState->rasterizationSamples;
		pip->sampleShadingEnable = pCreateInfos[c].pMultisampleState->sampleShadingEnable;
		pip->minSampleShading = pCreateInfos[c].pMultisampleState->minSampleShading;
		if(pCreateInfos[c].pMultisampleState->pSampleMask)
		{
			pip->sampleMask = *pCreateInfos[c].pMultisampleState->pSampleMask;
		}
		else
		{
			pip->sampleMask = 0;
		}
		pip->alphaToCoverageEnable = pCreateInfos[c].pMultisampleState->alphaToCoverageEnable;
		pip->alphaToOneEnable = pCreateInfos[c].pMultisampleState->alphaToOneEnable;

		pip->depthTestEnable = pCreateInfos[c].pDepthStencilState->depthTestEnable;
		pip->depthWriteEnable = pCreateInfos[c].pDepthStencilState->depthWriteEnable;
		pip->depthCompareOp = pCreateInfos[c].pDepthStencilState->depthCompareOp;
		pip->depthBoundsTestEnable = pCreateInfos[c].pDepthStencilState->depthBoundsTestEnable;
		pip->stencilTestEnable = pCreateInfos[c].pDepthStencilState->stencilTestEnable;
		pip->front.compareOp = pCreateInfos[c].pDepthStencilState->front.compareOp;
		pip->front.depthFailOp = pCreateInfos[c].pDepthStencilState->front.depthFailOp;
		pip->front.failOp = pCreateInfos[c].pDepthStencilState->front.failOp;
		pip->front.passOp = pCreateInfos[c].pDepthStencilState->front.passOp;
		pip->back.compareOp = pCreateInfos[c].pDepthStencilState->back.compareOp;
		pip->back.depthFailOp = pCreateInfos[c].pDepthStencilState->back.depthFailOp;
		pip->back.failOp = pCreateInfos[c].pDepthStencilState->back.failOp;
		pip->back.passOp = pCreateInfos[c].pDepthStencilState->back.passOp;
		if(!ignoreStencilCompareMask)
		{
			pip->front.compareMask = pCreateInfos[c].pDepthStencilState->front.compareMask;
			pip->back.compareMask = pCreateInfos[c].pDepthStencilState->back.compareMask;
		}
		if(!ignoreStencilWriteMask)
		{
			pip->front.writeMask = pCreateInfos[c].pDepthStencilState->front.writeMask;
			pip->back.writeMask = pCreateInfos[c].pDepthStencilState->back.writeMask;
		}
		if(!ignoreStencilReference)
		{
			pip->front.reference = pCreateInfos[c].pDepthStencilState->front.reference;
			pip->back.reference = pCreateInfos[c].pDepthStencilState->back.reference;
		}
		if(!ignoreDepthBounds)
		{
			pip->minDepthBounds = pCreateInfos[c].pDepthStencilState->minDepthBounds;
			pip->maxDepthBounds = pCreateInfos[c].pDepthStencilState->maxDepthBounds;
		}

		pip->logicOpEnable = pCreateInfos[c].pColorBlendState->logicOpEnable;
		pip->logicOp = pCreateInfos[c].pColorBlendState->logicOp;
		pip->attachmentCount = pCreateInfos[c].pColorBlendState->attachmentCount;
		pip->attachmentBlendStates = ALLOCATE(sizeof(VkPipelineColorBlendAttachmentState) * pip->attachmentCount, 1, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
		if(!pip->attachmentBlendStates)
		{
			PROFILEEND(RPIFUNC(vkCreateGraphicsPipelines));
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		memcpy(pip->attachmentBlendStates, pCreateInfos[c].pColorBlendState->pAttachments, sizeof(VkPipelineColorBlendAttachmentState) * pip->attachmentCount);

		if(!ignoreBlendConstants)
		{
			memcpy(pip->blendConstants, pCreateInfos[c].pColorBlendState, sizeof(float)*4);
		}

		if(pCreateInfos[c].pDynamicState)
		{
			pip->dynamicStateCount = pCreateInfos[c].pDynamicState->dynamicStateCount;
			pip->dynamicStates = ALLOCATE(sizeof(VkDynamicState)*pip->dynamicStateCount, 1, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
			if(!pip->dynamicStates)
			{
				PROFILEEND(RPIFUNC(vkCreateGraphicsPipelines));
				return VK_ERROR_OUT_OF_HOST_MEMORY;
			}

			memcpy(pip->dynamicStates, pCreateInfos[c].pDynamicState->pDynamicStates, sizeof(VkDynamicState)*pip->dynamicStateCount);
		}
		else
		{
			pip->dynamicStateCount = 0;
			pip->dynamicStates = 0;
		}

		pip->layout = pCreateInfos[c].layout;
		pip->renderPass = pCreateInfos[c].renderPass;
		pip->subpass = pCreateInfos[c].subpass;

		//TODO derivative pipelines ignored

		pPipelines[c] = pip;
	}

	PROFILEEND(RPIFUNC(vkCreateGraphicsPipelines));
	return VK_SUCCESS;
}

void RPIFUNC(vkDestroyPipeline)(VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* pAllocator)
{
	PROFILESTART(RPIFUNC(vkDestroyPipeline));

	assert(device);

	_pipeline* pip = pipeline;

	if(pip)
	{
		FREE(pip->dynamicStates);
		FREE(pip->attachmentBlendStates);
		FREE(pip->scissors);
		FREE(pip->viewports);
		FREE(pip->vertexBindingDescriptions);
		FREE(pip->vertexAttributeDescriptions);

		for(int c = 0; c < 6; ++c)
		{
			FREE(pip->names[c]);
		}
		FREE(pip);
	}

	PROFILEEND(RPIFUNC(vkDestroyPipeline));
}

VKAPI_ATTR VkResult VKAPI_CALL RPIFUNC(vkMergePipelineCaches)(
	VkDevice                                    device,
	VkPipelineCache                             dstCache,
	uint32_t                                    srcCacheCount,
	const VkPipelineCache*                      pSrcCaches)
{
	UNSUPPORTED(vkMergePipelineCaches);
	return UNSUPPORTED_RETURN;
}

VKAPI_ATTR VkResult VKAPI_CALL RPIFUNC(vkGetPipelineCacheData)(
	VkDevice                                    device,
	VkPipelineCache                             pipelineCache,
	size_t*                                     pDataSize,
	void*                                       pData)
{
	UNSUPPORTED(vkGetPipelineCacheData);
	return UNSUPPORTED_RETURN;
}

VKAPI_ATTR void VKAPI_CALL RPIFUNC(vkDestroyPipelineCache)(
	VkDevice                                    device,
	VkPipelineCache                             pipelineCache,
	const VkAllocationCallbacks*                pAllocator)
{
	UNSUPPORTED(vkDestroyPipelineCache);
}

VKAPI_ATTR VkResult VKAPI_CALL RPIFUNC(vkCreatePipelineLayout)(
	VkDevice                                    device,
	const VkPipelineLayoutCreateInfo*           pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkPipelineLayout*                           pPipelineLayout)
{
	PROFILESTART(RPIFUNC(vkCreatePipelineLayout));

	assert(device);
	assert(pCreateInfo);
	assert(pPipelineLayout);

	_pipelineLayout* pl = ALLOCATE(sizeof(_pipelineLayout), 1, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);

	if(!pl)
	{
		PROFILEEND(RPIFUNC(vkCreatePipelineLayout));
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	pl->setLayoutCount = pCreateInfo->setLayoutCount;
	pl->pushConstantRangeCount = pCreateInfo->pushConstantRangeCount;

	if(pCreateInfo->setLayoutCount > 0 && pCreateInfo->pSetLayouts)
	{
		pl->setLayouts = ALLOCATE(sizeof(VkDescriptorSetLayout)*pCreateInfo->setLayoutCount, 1, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
		if(!pl->setLayouts)
		{
			FREE(pl);
			PROFILEEND(RPIFUNC(vkCreatePipelineLayout));
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		memcpy(pl->setLayouts, pCreateInfo->pSetLayouts, sizeof(VkDescriptorSetLayout)*pCreateInfo->setLayoutCount);
	}

	if(pCreateInfo->pushConstantRangeCount > 0 && pCreateInfo->pPushConstantRanges)
	{
		pl->pushConstantRanges = ALLOCATE(sizeof(VkPushConstantRange)*pCreateInfo->pushConstantRangeCount, 1, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
		if(!pl->pushConstantRanges)
		{
			FREE(pl);
			PROFILEEND(RPIFUNC(vkCreatePipelineLayout));
			return VK_ERROR_OUT_OF_HOST_MEMORY;
		}

		memcpy(pl->pushConstantRanges, pCreateInfo->pPushConstantRanges, sizeof(VkPushConstantRange)*pCreateInfo->pushConstantRangeCount);
	}

	pl->descriptorSetBindingMap = createMap(ALLOCATE(sizeof(_descriptorSet*)*pCreateInfo->setLayoutCount, 1, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT), pCreateInfo->setLayoutCount);

	*pPipelineLayout = pl;

	PROFILEEND(RPIFUNC(vkCreatePipelineLayout));
	return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL RPIFUNC(vkDestroyPipelineLayout)(
	VkDevice                                    device,
	VkPipelineLayout                            pipelineLayout,
	const VkAllocationCallbacks*                pAllocator)
{
	PROFILESTART(RPIFUNC(vkDestroyPipelineLayout));

	assert(device);
	assert(pipelineLayout);

	_pipelineLayout* pl = pipelineLayout;

	FREE(pl->descriptorSetBindingMap.elements);
	FREE(pl->pushConstantRanges);
	FREE(pl->setLayouts);

	FREE(pl);

	PROFILEEND(RPIFUNC(vkDestroyPipelineLayout));
}

VKAPI_ATTR VkResult VKAPI_CALL RPIFUNC(vkCreatePipelineCache)(
	VkDevice                                    device,
	const VkPipelineCacheCreateInfo*            pCreateInfo,
	const VkAllocationCallbacks*                pAllocator,
	VkPipelineCache*                            pPipelineCache)
{
	UNSUPPORTED(vkCreatePipelineCache);
	return UNSUPPORTED_RETURN;
}
