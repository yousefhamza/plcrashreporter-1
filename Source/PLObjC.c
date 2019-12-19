//
//  KSObjC.c
//
//  Created by Karl Stenerud on 2012-08-30.
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


#include "PLObjC.h"
#include "PLObjCApple.h"

#include "PLMemory.h"

//#include "KSLogger.h"

#if __IPHONE_OS_VERSION_MAX_ALLOWED > 70000
#include <objc/NSObjCRuntime.h>
#else
#if __LP64__ || (TARGET_OS_EMBEDDED && !TARGET_OS_IPHONE) || TARGET_OS_WIN32 || NS_BUILD_32_LIKE_64
typedef long NSInteger;
typedef unsigned long NSUInteger;
#else
typedef int NSInteger;
typedef unsigned int NSUInteger;
#endif
#endif

#include <CoreFoundation/CoreFoundation.h>
#include <CoreFoundation/CFBase.h>
#include <CoreGraphics/CGBase.h>
#include <inttypes.h>
#include <objc/runtime.h>
#include <objc/objc.h>


#define kPLMaxNameLength 128

//======================================================================
#pragma mark - Macros -
//======================================================================

// Compiler hints for "if" statements
#define likely_if(x) if(__builtin_expect(x,1))
#define unlikely_if(x) if(__builtin_expect(x,0))


//======================================================================
#pragma mark - Types -
//======================================================================

typedef enum
{
    ClassSubtypeNone = 0,
    ClassSubtypeCFArray,
    ClassSubtypeNSArrayMutable,
    ClassSubtypeNSArrayImmutable,
    ClassSubtypeCFString,
} ClassSubtype;

typedef struct
{
    const char* name;
    PLObjCClassType type;
    ClassSubtype subtype;
    bool isMutable;
    bool (*plobjc_is_valid_object)(const void* object);
    int (*description)(const void* object, char* buffer, int bufferLength);
    const void* class;
} ClassData;


//======================================================================
#pragma mark - Globals -
//======================================================================

// Forward references
static bool plobjc_tagged_object_is_valid(const void* object);
static bool plobjc_tagged_date_is_valid(const void* object);
static bool plobjc_tagged_number_is_valid(const void* object);
static bool plobjc_tagged_string_is_valid(const void* object);

static int plobjc_tagged_object_description(const void* object, char* buffer, int bufferLength);
static int plobjc_tagged_date_description(const void* object, char* buffer, int bufferLength);
static int plobjc_tagged_number_description(const void* object, char* buffer, int bufferLength);
static int plobjc_tagged_string_description(const void* object, char* buffer, int bufferLength);

static ClassData g_tagged_class_data[] =
{
    {"NSAtom",               PLObjCClassTypeUnknown, ClassSubtypeNone,             false, plobjc_tagged_object_is_valid, plobjc_tagged_object_description},
    {NULL,                   PLObjCClassTypeUnknown, ClassSubtypeNone,             false, plobjc_tagged_object_is_valid, plobjc_tagged_object_description},
    {"NSString",             PLObjCClassTypeString,  ClassSubtypeNone,             false, plobjc_tagged_string_is_valid, plobjc_tagged_string_description},
    {"NSNumber",             PLObjCClassTypeNumber,  ClassSubtypeNone,             false, plobjc_tagged_number_is_valid, plobjc_tagged_number_description},
    {"NSIndexPath",          PLObjCClassTypeUnknown, ClassSubtypeNone,             false, plobjc_tagged_object_is_valid, plobjc_tagged_object_description},
    {"NSManagedObjectID",    PLObjCClassTypeUnknown, ClassSubtypeNone,             false, plobjc_tagged_object_is_valid, plobjc_tagged_object_description},
    {"NSDate",               PLObjCClassTypeDate,    ClassSubtypeNone,             false, plobjc_tagged_date_is_valid,   plobjc_tagged_date_description},
    {NULL,                   PLObjCClassTypeUnknown, ClassSubtypeNone,             false, plobjc_tagged_object_is_valid, plobjc_tagged_object_description},
};
static int g_tagged_class_data_count = sizeof(g_tagged_class_data) / sizeof(*g_tagged_class_data);


//======================================================================
#pragma mark - Utility -
//======================================================================

#if SUPPORT_TAGGED_POINTERS
static bool plobjc_is_tagged_pointer_internal(const void* pointer) {return (((uintptr_t)pointer) & TAG_MASK) != 0; }
static int  plobjc_get_tagged_slot(const void* pointer) { return (int)((((uintptr_t)pointer) >> TAG_SLOT_SHIFT) & TAG_SLOT_MASK); }
static uintptr_t  plobjc_get_tagged_payload(const void* pointer) { return (((uintptr_t)pointer) << TAG_PAYLOAD_LSHIFT) >> TAG_PAYLOAD_RSHIFT; }
#else
static bool plobjc_is_tagged_pointer_internal(__unused const void* pointer) { return false; }
static int  plobjc_get_tagged_slot(__unused const void* pointer) { return 0; }
static uintptr_t  plobjc_get_tagged_payload(const void* pointer) { return (uintptr_t)pointer; }
#endif

/** Get class data for a tagged pointer.
 *
 * @param object The tagged pointer.
 * @return The class data.
 */
static const ClassData* plobjc_get_class_data_from_tagged_pointer(const void* const object)
{
    int slot =  plobjc_get_tagged_slot(object);
    return &g_tagged_class_data[slot];
}

static bool plobjc_is_valid_tagged_pointer_internal(const void* object)
{
    if(plobjc_is_tagged_pointer_internal(object))
    {
        if( plobjc_get_tagged_slot(object) <= g_tagged_class_data_count)
        {
            const ClassData* classData = plobjc_get_class_data_from_tagged_pointer(object);
            return classData->type != PLObjCClassTypeUnknown;
        }
    }
    return false;
}

/** Check if a tagged pointer is a number.
 *
 * @param object The object to query.
 * @return true if the tagged pointer is an NSNumber.
 */
static bool plobjc_is_tagged_pointer_internalNSNumber(const void* const object)
{
    return  plobjc_get_tagged_slot(object) == OBJC_TAG_NSNumber;
}

/** Check if a tagged pointer is a string.
 *
 * @param object The object to query.
 * @return true if the tagged pointer is an NSString.
 */
static bool plobjc_is_tagged_pointer_internalNSString(const void* const object)
{
    return  plobjc_get_tagged_slot(object) == OBJC_TAG_NSString;
}

/** Check if a tagged pointer is a date.
 *
 * @param object The object to query.
 * @return true if the tagged pointer is an NSDate.
 */
static bool plobjc_is_tagged_pointer_internalNSDate(const void* const object)
{
    return  plobjc_get_tagged_slot(object) == OBJC_TAG_NSDate;
}

/** Extract an integer from a tagged NSNumber.
 *
 * @param object The NSNumber object (must be a tagged pointer).
 * @return The integer value.
 */
static int64_t plobjc_extract_tagged_NSNumber(const void* const object)
{
    intptr_t signedPointer = (intptr_t)object;
#if SUPPORT_TAGGED_POINTERS
    intptr_t value = (signedPointer << TAG_PAYLOAD_LSHIFT) >> TAG_PAYLOAD_RSHIFT;
#else
    intptr_t value = signedPointer & 0;
#endif
    
    // The lower 4 bits encode type information so shift them out.
    return (int64_t)(value >> 4);
}

static int plobjc_get_tagged_NSString_length(const void* const object)
{
    uintptr_t payload =  plobjc_get_tagged_payload(object);
    return (int)(payload & 0xf);
}

static int plobjc_extract_tagged_NSString(const void* const object, char* buffer, int bufferLength)
{
    int length = plobjc_get_tagged_NSString_length(object);
    int copyLength = ((length + 1) > bufferLength) ? (bufferLength - 1) : length;
    uintptr_t payload =  plobjc_get_tagged_payload(object);
    uintptr_t value = payload >> 4;
    static char* alphabet = "eilotrm.apdnsIc ufkMShjTRxgC4013bDNvwyUL2O856P-B79AFKEWV_zGJ/HYX";
    if(length <=7)
    {
        for(int i = 0; i < copyLength; i++)
        {
            // ASCII case, limit to bottom 7 bits just in case
            buffer[i] = (char)(value & 0x7f);
            value >>= 8;
        }
    }
    else if(length <= 9)
    {
        for(int i = 0; i < copyLength; i++)
        {
            uintptr_t index = (value >> ((length - 1 - i) * 6)) & 0x3f;
            buffer[i] = alphabet[index];
        }
    }
    else if(length <= 11)
    {
        for(int i = 0; i < copyLength; i++)
        {
            uintptr_t index = (value >> ((length - 1 - i) * 5)) & 0x1f;
            buffer[i] = alphabet[index];
        }
    }
    else
    {
        buffer[0] = 0;
    }
    buffer[length] = 0;

    return length;
}

/** Extract a tagged NSDate's time value as an absolute time.
 *
 * @param object The NSDate object (must be a tagged pointer).
 * @return The date's absolute time.
 */
static CFAbsoluteTime plobjc_extract_tagged_NSDate(const void* const object)
{
    uintptr_t payload =  plobjc_get_tagged_payload(object);
    // Payload is a 60-bit float. Fortunately we can just cast across from
    // an integer pointer after shifting out the upper 4 bits.
    payload <<= 4;
    CFAbsoluteTime value = *((CFAbsoluteTime*)&payload);
    return value;
}

static int plobjc_string_printf(char* buffer, int bufferLength, const char* fmt, ...)
{
    unlikely_if(bufferLength == 0)
    {
        return 0;
    }
    
    va_list args;
    va_start(args,fmt);
    int printLength = vsnprintf(buffer, bufferLength, fmt, args);
    va_end(args);
    
    unlikely_if(printLength < 0)
    {
        *buffer = 0;
        return 0;
    }
    unlikely_if(printLength > bufferLength)
    {
        return bufferLength-1;
    }
    return printLength;
}

//======================================================================
#pragma mark - Unknown Object -
//======================================================================

static bool plobjc_tagged_object_is_valid(const void* object)
{
    return plobjc_is_valid_tagged_pointer_internal(object);
}

static int plobjc_tagged_object_description(const void* object, char* buffer, int bufferLength)
{
    const ClassData* data = plobjc_get_class_data_from_tagged_pointer(object);
    const char* name = data->name;
    uintptr_t objPointer = (uintptr_t)object;
    const char* fmt = sizeof(uintptr_t) == sizeof(uint32_t) ? "<%s: 0x%08x>" : "<%s: 0x%016x>";
    return plobjc_string_printf(buffer, bufferLength, fmt, name, objPointer);
}


//======================================================================
#pragma mark - NSString -
//======================================================================

static bool plobjc_tagged_string_is_valid(const void* const object)
{
    return plobjc_is_valid_tagged_pointer_internal(object) && plobjc_is_tagged_pointer_internalNSString(object);
}

static int plobjc_tagged_string_description(const void* object, char* buffer, __unused int bufferLength)
{
    return plobjc_extract_tagged_NSString(object, buffer, bufferLength);
}

//======================================================================
#pragma mark - NSDate -
//======================================================================

static bool plobjc_tagged_date_is_valid(const void* const datePtr)
{
    return plobjc_is_valid_tagged_pointer_internal(datePtr) && plobjc_is_tagged_pointer_internalNSDate(datePtr);
}

static int plobjc_tagged_date_description(const void* object, char* buffer, int bufferLength)
{
    char* pBuffer = buffer;
    char* pEnd = buffer + bufferLength;

    CFAbsoluteTime time = plobjc_extract_tagged_NSDate(object);
    pBuffer += plobjc_tagged_object_description(object, pBuffer, (int)(pEnd - pBuffer));
    pBuffer += plobjc_string_printf(pBuffer, (int)(pEnd - pBuffer), ": %f", time);

    return (int)(pBuffer - buffer);
}


//======================================================================
#pragma mark - NSNumber -
//======================================================================

static bool plobjc_tagged_number_is_valid(const void* const object)
{
    return plobjc_is_valid_tagged_pointer_internal(object) && plobjc_is_tagged_pointer_internalNSNumber(object);
}

static int plobjc_tagged_number_description(const void* object, char* buffer, int bufferLength)
{
    char* pBuffer = buffer;
    char* pEnd = buffer + bufferLength;

    int64_t value = plobjc_extract_tagged_NSNumber(object);
    pBuffer += plobjc_tagged_object_description(object, pBuffer, (int)(pEnd - pBuffer));
    pBuffer += plobjc_string_printf(pBuffer, (int)(pEnd - pBuffer), ": %" PRId64, value);

    return (int)(pBuffer - buffer);
}

//======================================================================
#pragma mark - General Queries -
//======================================================================


bool plobjc_is_tagged_pointer(const void* const pointer)
{
    return plobjc_is_tagged_pointer_internal(pointer);
}

bool plobjc_is_valid_tagged_pointer(const void* const pointer)
{
    return plobjc_is_valid_tagged_pointer_internal(pointer);
}
