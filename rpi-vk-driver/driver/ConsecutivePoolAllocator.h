#pragma once

#if defined (__cplusplus)
extern "C" {
#endif

#include "CustomAssert.h"

#include <stdint.h>

typedef struct ConsecutivePoolAllocator
{
	void* buf; //preallocated buffer
	void* nextFreeBlock;
	unsigned blockSize;
	unsigned size; //size is exact multiple of block size
	unsigned numFreeBlocks;
} ConsecutivePoolAllocator;

ConsecutivePoolAllocator createConsecutivePoolAllocator(void* b, unsigned bs, unsigned s);
void destroyConsecutivePoolAllocator(ConsecutivePoolAllocator* pa);
uint32_t consecutivePoolAllocate(ConsecutivePoolAllocator* pa, uint32_t numBlocks);
void consecutivePoolFree(ConsecutivePoolAllocator* pa, void* p, uint32_t numBlocks);
uint32_t consecutivePoolReAllocate(ConsecutivePoolAllocator* pa, void* currentMem, uint32_t currNumBlocks, uint32_t newNumBlocks);
void CPAdebugPrint(ConsecutivePoolAllocator* pa);
void* getCPAptrFromOffset(ConsecutivePoolAllocator* pa, uint32_t offset);

#if defined (__cplusplus)
}
#endif
