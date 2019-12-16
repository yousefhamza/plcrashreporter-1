//
//  KSMemory.c
//
//  Created by Karl Stenerud on 2012-01-29.
//
//  Copyright (c) 2012 Karl Stenerud. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall remain in place
// in this source code.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//


#include "PLMemory.h"

#include <mach/mach.h>


static inline int plmem_copy_safely_internal(const void* restrict const src, void* restrict const dst, const int byteCount)
{
    vm_size_t bytesCopied = 0;
    kern_return_t result = vm_read_overwrite(mach_task_self(),
                                             (vm_address_t)src,
                                             (vm_size_t)byteCount,
                                             (vm_address_t)dst,
                                             &bytesCopied);
    if(result != KERN_SUCCESS)
    {
        return 0;
    }
    return (int)bytesCopied;
}

static inline int plmem_copy_max_possible_internal(const void* restrict const src, void* restrict const dst, const int byteCount)
{
    const uint8_t* pSrc = src;
    const uint8_t* pSrcMax = (uint8_t*)src + byteCount;
    const uint8_t* pSrcEnd = (uint8_t*)src + byteCount;
    uint8_t* pDst = dst;
    
    int bytesCopied = 0;
    
    // Short-circuit if no memory is readable
    if(plmem_copy_safely_internal(src, dst, 1) != 1)
    {
        return 0;
    }
    else if(byteCount <= 1)
    {
        return byteCount;
    }
    
    for(;;)
    {
        int copyLength = (int)(pSrcEnd - pSrc);
        if(copyLength <= 0)
        {
            break;
        }
        
        if(plmem_copy_safely_internal(pSrc, pDst, copyLength) == copyLength)
        {
            bytesCopied += copyLength;
            pSrc += copyLength;
            pDst += copyLength;
            pSrcEnd = pSrc + (pSrcMax - pSrc) / 2;
        }
        else
        {
            if(copyLength <= 1)
            {
                break;
            }
            pSrcMax = pSrcEnd;
            pSrcEnd = pSrc + copyLength / 2;
        }
    }
    return bytesCopied;
}

static char g_memory_test_buffer[10240];
static inline bool plmem_is_memory_readable_internal(const void* const memory, const int byteCount)
{
    const int testBufferSize = sizeof(g_memory_test_buffer);
    int bytesRemaining = byteCount;
    
    while(bytesRemaining > 0)
    {
        int bytesToCopy = bytesRemaining > testBufferSize ? testBufferSize : bytesRemaining;
        if(plmem_copy_safely_internal(memory, g_memory_test_buffer, bytesToCopy) != bytesToCopy)
        {
            break;
        }
        bytesRemaining -= bytesToCopy;
    }
    return bytesRemaining == 0;
}

bool plmem_is_memory_readable(const void* const memory, const int byteCount)
{
    return plmem_is_memory_readable_internal(memory, byteCount);
}

int plmem_copy_max_possible(const void* restrict const src, void* restrict const dst, const int byteCount)
{
    return plmem_copy_max_possible_internal(src, dst, byteCount);
}

bool plmem_copy_safely(const void* restrict const src, void* restrict const dst, const int byteCount)
{
    return plmem_copy_safely_internal(src, dst, byteCount);
}
