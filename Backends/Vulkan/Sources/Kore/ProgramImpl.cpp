#include "pch.h"
#include <Kore/Graphics/Shader.h>
#include <Kore/Graphics/Graphics.h>
#include <Kore/Log.h>
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>

using namespace Kore;

extern VkDevice device;
extern VkFormat format;
extern VkFormat depth_format;
extern VkRenderPass render_pass;
extern VkCommandBuffer draw_cmd;
extern VkDescriptorSet desc_set;
extern VkDescriptorPool desc_pool;

bool memory_type_from_properties(uint32_t typeBits, VkFlags requirements_mask, uint32_t *typeIndex);

Program* ProgramImpl::current;

namespace {
	void parseShader(Shader* shader, std::map<std::string, u32>& locations) {
		u32* spirv = (u32*)shader->source;
		int spirvsize = shader->length / 4;
		int index = 0;

		unsigned magicNumber = spirv[index++];
		unsigned version = spirv[index++];
		unsigned generator = spirv[index++];
		unsigned bound = spirv[index++];
		index++;

		std::map<u32, std::string> names;
		std::map<u32, u32> locs;

		while (index < spirvsize) {
			int wordCount = spirv[index] >> 16;
			u32 opcode = spirv[index] & 0xffff;

			u32* operands = wordCount > 1 ? &spirv[index + 1] : nullptr;
			u32 length = wordCount - 1;
			
			switch (opcode) {
			case 5: { // OpName
				u32 id = operands[0];
				char* string = (char*)&operands[1];
				names[id] = string;
				break;
			}
			case 71: { // OpDecorate
				u32 id = operands[0];
				u32 decoration = operands[1];
				if (decoration == 30) { // location
					u32 location = operands[2];
					locs[id] = location;
				}
				break;
			}
			}

			index += wordCount;
		}

		for (std::map<u32, u32>::iterator it = locs.begin(); it != locs.end(); ++it) {
			locations[names[it->first]] = it->second;
		}
	}

	VkShaderModule demo_prepare_shader_module(const void *code, size_t size) {
		VkShaderModuleCreateInfo moduleCreateInfo;
		VkShaderModule module;
		VkResult err;

		moduleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		moduleCreateInfo.pNext = NULL;

		moduleCreateInfo.codeSize = size;
		moduleCreateInfo.pCode = (const uint32_t*)code;
		moduleCreateInfo.flags = 0;
		err = vkCreateShaderModule(device, &moduleCreateInfo, NULL, &module);
		assert(!err);

		return module;
	}

	VkShaderModule demo_prepare_vs(VkShaderModule& vert_shader_module, Shader* vertexShader) {
		vert_shader_module = demo_prepare_shader_module(vertexShader->source, vertexShader->length);
		return vert_shader_module;
	}

	VkShaderModule demo_prepare_fs(VkShaderModule& frag_shader_module, Shader* fragmentShader) {
		frag_shader_module = demo_prepare_shader_module(fragmentShader->source, fragmentShader->length);
		return frag_shader_module;
	}

	void createUniformBuffer(VkBuffer& buf, VkMemoryAllocateInfo& mem_alloc, VkDeviceMemory& mem, VkDescriptorBufferInfo& buffer_info) {
		VkMemoryRequirements mem_reqs;
		bool pass;
		
		VkBufferCreateInfo buf_info;
		memset(&buf_info, 0, sizeof(buf_info));
		buf_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		buf_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		buf_info.size = sizeof(float) * 256;
		VkResult err = vkCreateBuffer(device, &buf_info, NULL, &buf);
		assert(!err);

		vkGetBufferMemoryRequirements(device, buf, &mem_reqs);

		mem_alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		mem_alloc.pNext = NULL;
		mem_alloc.allocationSize = mem_reqs.size;
		mem_alloc.memoryTypeIndex = 0;

		pass = memory_type_from_properties(mem_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &mem_alloc.memoryTypeIndex);
		assert(pass);

		err = vkAllocateMemory(device, &mem_alloc, NULL, &mem);
		assert(!err);

		err = vkBindBufferMemory(device, buf, mem, 0);
		assert(!err);

		buffer_info.buffer = buf;
		buffer_info.offset = 0;
		buffer_info.range = sizeof(float) * 256;
	}
}

namespace Kore {
	bool programUsesTesselation = false;
}

ProgramImpl::ProgramImpl() : textureCount(0), vertexShader(nullptr), fragmentShader(nullptr), geometryShader(nullptr), tesselationEvaluationShader(nullptr), tesselationControlShader(nullptr) {
	textures = new const char*[16];
	textureValues = new int[16];
}

Program::Program() {
	
}

ProgramImpl::~ProgramImpl() {

}

void Program::setVertexShader(Shader* shader) {
	vertexShader = shader;
}

void Program::setFragmentShader(Shader* shader) {
	fragmentShader = shader;
}

void Program::setGeometryShader(Shader* shader) {
	geometryShader = shader;
}

void Program::setTesselationControlShader(Shader* shader) {
	tesselationControlShader = shader;
}

void Program::setTesselationEvaluationShader(Shader* shader) {
	tesselationEvaluationShader = shader;
}

void Program::link(VertexStructure** structures, int count) {
	parseShader(vertexShader, vertexLocations);

	VkDescriptorSetLayoutBinding layout_binding = {};
	/*layout_binding.binding = 0;
	layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	layout_binding.descriptorCount = 0; // DEMO_TEXTURE_COUNT;
	layout_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	layout_binding.pImmutableSamplers = NULL;*/
	layout_binding.binding = 0;
	layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	layout_binding.descriptorCount = 1;
	layout_binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	layout_binding.pImmutableSamplers = nullptr;
	
	VkDescriptorSetLayoutCreateInfo descriptor_layout = {};
	descriptor_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	descriptor_layout.pNext = NULL;
	descriptor_layout.bindingCount = 1;
	descriptor_layout.pBindings = &layout_binding;

	VkResult err = vkCreateDescriptorSetLayout(device, &descriptor_layout, NULL, &desc_layout);
	assert(!err);
	
	VkDescriptorPoolSize type_count = {};
	type_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; //VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
	type_count.descriptorCount = 1;
	
	VkDescriptorPoolCreateInfo descriptor_pool = {};
	descriptor_pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	descriptor_pool.pNext = NULL;
	descriptor_pool.maxSets = 1;
	descriptor_pool.poolSizeCount = 1;
	descriptor_pool.pPoolSizes = &type_count;
	
	err = vkCreateDescriptorPool(device, &descriptor_pool, NULL, &desc_pool);
	assert(!err);

	//VkDescriptorImageInfo tex_descs[DEMO_TEXTURE_COUNT];
	VkDescriptorBufferInfo buffer_descs[1];
	
	VkDescriptorSetAllocateInfo alloc_info = {};
	alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	alloc_info.pNext = NULL;
	alloc_info.descriptorPool = desc_pool;
	alloc_info.descriptorSetCount = 1;
	alloc_info.pSetLayouts = &desc_layout;
	err = vkAllocateDescriptorSets(device, &alloc_info, &desc_set);
	assert(!err);

	createUniformBuffer(buf, mem_alloc, mem, buffer_info);

	/*memset(&tex_descs, 0, sizeof(tex_descs));
	for (i = 0; i < 1; i++) {
		tex_descs[i].sampler = demo->textures[i].sampler;
		tex_descs[i].imageView = demo->textures[i].view;
		tex_descs[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}*/
	memset(&buffer_descs, 0, sizeof(buffer_descs));
	for (uint32_t i = 0; i < 1; ++i) {
		buffer_descs[i].buffer = buf;
		buffer_descs[i].offset = 0;
		buffer_descs[i].range = 1 * sizeof(float);
	}

	VkWriteDescriptorSet write;
	memset(&write, 0, sizeof(write));
	write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	write.dstSet = desc_set;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; // VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	//write.pImageInfo = tex_descs;
	write.pBufferInfo = buffer_descs;

	vkUpdateDescriptorSets(device, 1, &write, 0, NULL);

	//

	VkPipelineLayoutCreateInfo pPipelineLayoutCreateInfo = {};
	pPipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pPipelineLayoutCreateInfo.pNext = NULL;
	pPipelineLayoutCreateInfo.setLayoutCount = 1;
	pPipelineLayoutCreateInfo.pSetLayouts = &desc_layout;

	err = vkCreatePipelineLayout(device, &pPipelineLayoutCreateInfo, NULL, &pipeline_layout);
	assert(!err);

	//

	VkGraphicsPipelineCreateInfo pipeline_info = {};
	VkPipelineCacheCreateInfo pipelineCache_info = {};

	VkPipelineInputAssemblyStateCreateInfo ia = {};
	VkPipelineRasterizationStateCreateInfo rs = {};
	VkPipelineColorBlendStateCreateInfo cb = {};
	VkPipelineDepthStencilStateCreateInfo ds = {};
	VkPipelineViewportStateCreateInfo vp = {};
	VkPipelineMultisampleStateCreateInfo ms = {};
	VkDynamicState dynamicStateEnables[VK_DYNAMIC_STATE_RANGE_SIZE];
	VkPipelineDynamicStateCreateInfo dynamicState = {};

	memset(dynamicStateEnables, 0, sizeof dynamicStateEnables);
	memset(&dynamicState, 0, sizeof dynamicState);
	dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
	dynamicState.pDynamicStates = dynamicStateEnables;

	memset(&pipeline_info, 0, sizeof(pipeline_info));
	pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
	pipeline_info.layout = pipeline_layout;

	uint32_t stride = 0;
	for (int i = 0; i < structures[0]->size; ++i) {
		VertexElement element = structures[0]->elements[i];
		switch (element.data) {
		case ColorVertexData:
			stride += 1 * 4;
			break;
		case Float1VertexData:
			stride += 1 * 4;
			break;
		case Float2VertexData:
			stride += 2 * 4;
			break;
		case Float3VertexData:
			stride += 3 * 4;
			break;
		case Float4VertexData:
			stride += 4 * 4;
			break;
		case Float4x4VertexData:
			stride += 4 * 4 * 4;
			break;
		}
	}

	VkVertexInputBindingDescription vi_bindings[1];
	VkVertexInputAttributeDescription* vi_attrs = (VkVertexInputAttributeDescription*)alloca(sizeof(VkVertexInputAttributeDescription) * structures[0]->size);

	VkPipelineVertexInputStateCreateInfo vi = {};
	vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
	vi.pNext = NULL;
	vi.vertexBindingDescriptionCount = 1;
	vi.pVertexBindingDescriptions = vi_bindings;
	vi.vertexAttributeDescriptionCount = structures[0]->size;
	vi.pVertexAttributeDescriptions = vi_attrs;

	vi_bindings[0].binding = 0;
	vi_bindings[0].stride = stride;
	vi_bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

	uint32_t offset = 0;
	for (int i = 0; i < structures[0]->size; ++i) {
		VertexElement element = structures[0]->elements[i];
		switch (element.data) {
		case ColorVertexData:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = vertexLocations[element.name];
			vi_attrs[i].format = VK_FORMAT_R32_UINT;
			vi_attrs[i].offset = offset;
			offset += 1 * 4;
			break;
		case Float1VertexData:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = vertexLocations[element.name];
			vi_attrs[i].format = VK_FORMAT_R32_SFLOAT;
			vi_attrs[i].offset = offset;
			offset += 1 * 4;
			break;
		case Float2VertexData:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = vertexLocations[element.name];
			vi_attrs[i].format = VK_FORMAT_R32G32_SFLOAT;
			vi_attrs[i].offset = offset;
			offset += 2 * 4;
			break;
		case Float3VertexData:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = vertexLocations[element.name];
			vi_attrs[i].format = VK_FORMAT_R32G32B32_SFLOAT;
			vi_attrs[i].offset = offset;
			offset += 3 * 4;
			break;
		case Float4VertexData:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = vertexLocations[element.name];
			vi_attrs[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
			vi_attrs[i].offset = offset;
			offset += 4 * 4;
			break;
		case Float4x4VertexData:
			vi_attrs[i].binding = 0;
			vi_attrs[i].location = vertexLocations[element.name];
			vi_attrs[i].format = VK_FORMAT_R32G32B32A32_SFLOAT; // TODO
			vi_attrs[i].offset = offset;
			offset += 4 * 4 * 4;
			break;
		}
	}

	memset(&ia, 0, sizeof(ia));
	ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

	memset(&rs, 0, sizeof(rs));
	rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
	rs.depthClampEnable = VK_FALSE;
	rs.rasterizerDiscardEnable = VK_FALSE;
	rs.depthBiasEnable = VK_FALSE;

	memset(&cb, 0, sizeof(cb));
	cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
	VkPipelineColorBlendAttachmentState att_state[1];
	memset(att_state, 0, sizeof(att_state));
	att_state[0].colorWriteMask = 0xf;
	att_state[0].blendEnable = VK_FALSE;
	cb.attachmentCount = 1;
	cb.pAttachments = att_state;

	memset(&vp, 0, sizeof(vp));
	vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
	vp.viewportCount = 1;
	dynamicStateEnables[dynamicState.dynamicStateCount++] =
		VK_DYNAMIC_STATE_VIEWPORT;
	vp.scissorCount = 1;
	dynamicStateEnables[dynamicState.dynamicStateCount++] =
		VK_DYNAMIC_STATE_SCISSOR;

	memset(&ds, 0, sizeof(ds));
	ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	ds.depthTestEnable = VK_TRUE;
	ds.depthWriteEnable = VK_TRUE;
	ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
	ds.depthBoundsTestEnable = VK_FALSE;
	ds.back.failOp = VK_STENCIL_OP_KEEP;
	ds.back.passOp = VK_STENCIL_OP_KEEP;
	ds.back.compareOp = VK_COMPARE_OP_ALWAYS;
	ds.stencilTestEnable = VK_FALSE;
	ds.front = ds.back;

	memset(&ms, 0, sizeof(ms));
	ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
	ms.pSampleMask = NULL;
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

	// Two stages: vs and fs
	pipeline_info.stageCount = 2;
	VkPipelineShaderStageCreateInfo shaderStages[2];
	memset(&shaderStages, 0, 2 * sizeof(VkPipelineShaderStageCreateInfo));

	shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	shaderStages[0].module = demo_prepare_vs(vert_shader_module, vertexShader);
	shaderStages[0].pName = "main";

	shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	shaderStages[1].module = demo_prepare_fs(frag_shader_module, fragmentShader);
	shaderStages[1].pName = "main";

	pipeline_info.pVertexInputState = &vi;
	pipeline_info.pInputAssemblyState = &ia;
	pipeline_info.pRasterizationState = &rs;
	pipeline_info.pColorBlendState = &cb;
	pipeline_info.pMultisampleState = &ms;
	pipeline_info.pViewportState = &vp;
	pipeline_info.pDepthStencilState = &ds;
	pipeline_info.pStages = shaderStages;
	pipeline_info.renderPass = render_pass;
	pipeline_info.pDynamicState = &dynamicState;

	memset(&pipelineCache_info, 0, sizeof(pipelineCache_info));
	pipelineCache_info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

	err = vkCreatePipelineCache(device, &pipelineCache_info, NULL, &pipelineCache);
	assert(!err);
	err = vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipeline_info, NULL, &pipeline);
	assert(!err);

	vkDestroyPipelineCache(device, pipelineCache, NULL);

	vkDestroyShaderModule(device, frag_shader_module, NULL);
	vkDestroyShaderModule(device, vert_shader_module, NULL);
}

void Program::set() {
	current = this;
	
	uint8_t* data;
	VkResult err = vkMapMemory(device, mem, 0, mem_alloc.allocationSize, 0, (void**)&data);
	assert(!err);
	memcpy(data, &uniformData, sizeof(uniformData));
	vkUnmapMemory(device, mem);

	vkCmdBindPipeline(draw_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets(draw_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &desc_set, 0, NULL);
}

ConstantLocation Program::getConstantLocation(const char* name) {
	ConstantLocation location;
	location.location = 0;
	return location;
}

TextureUnit Program::getTextureUnit(const char* name) {
	TextureUnit unit;
	unit.unit = 0;
	return unit;
}
