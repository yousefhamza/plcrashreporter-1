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
#include "KSObjCApple.h"

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
#include <CoreFoundation/CFBase.h>
#include <CoreGraphics/CGBase.h>
#include <inttypes.h>
#include <objc/runtime.h>


#define kMaxNameLength 128

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
    bool (*isValidObject)(const void* object);
    int (*description)(const void* object, char* buffer, int bufferLength);
    const void* class;
} ClassData;


//======================================================================
#pragma mark - Globals -
//======================================================================

// Forward references
static bool taggedObjectIsValid(const void* object);
static bool taggedDateIsValid(const void* object);
static bool taggedNumberIsValid(const void* object);
static bool taggedStringIsValid(const void* object);

static int taggedObjectDescription(const void* object, char* buffer, int bufferLength);
static int taggedDateDescription(const void* object, char* buffer, int bufferLength);
static int taggedNumberDescription(const void* object, char* buffer, int bufferLength);
static int taggedStringDescription(const void* object, char* buffer, int bufferLength);

static ClassData g_taggedClassData[] =
{
    {"NSAtom",               PLObjCClassTypeUnknown, ClassSubtypeNone,             false, taggedObjectIsValid, taggedObjectDescription},
    {NULL,                   PLObjCClassTypeUnknown, ClassSubtypeNone,             false, taggedObjectIsValid, taggedObjectDescription},
    {"NSString",             PLObjCClassTypeString,  ClassSubtypeNone,             false, taggedStringIsValid, taggedStringDescription},
    {"NSNumber",             PLObjCClassTypeNumber,  ClassSubtypeNone,             false, taggedNumberIsValid, taggedNumberDescription},
    {"NSIndexPath",          PLObjCClassTypeUnknown, ClassSubtypeNone,             false, taggedObjectIsValid, taggedObjectDescription},
    {"NSManagedObjectID",    PLObjCClassTypeUnknown, ClassSubtypeNone,             false, taggedObjectIsValid, taggedObjectDescription},
    {"NSDate",               PLObjCClassTypeDate,    ClassSubtypeNone,             false, taggedDateIsValid,   taggedDateDescription},
    {NULL,                   PLObjCClassTypeUnknown, ClassSubtypeNone,             false, taggedObjectIsValid, taggedObjectDescription},
};
static int g_taggedClassDataCount = sizeof(g_taggedClassData) / sizeof(*g_taggedClassData);

static const char* g_blockBaseClassName = "NSBlock";


//======================================================================
#pragma mark - Utility -
//======================================================================

#if SUPPORT_TAGGED_POINTERS
static bool isTaggedPointer(const void* pointer) {return (((uintptr_t)pointer) & TAG_MASK) != 0; }
static int getTaggedSlot(const void* pointer) { return (int)((((uintptr_t)pointer) >> TAG_SLOT_SHIFT) & TAG_SLOT_MASK); }
static uintptr_t getTaggedPayload(const void* pointer) { return (((uintptr_t)pointer) << TAG_PAYLOAD_LSHIFT) >> TAG_PAYLOAD_RSHIFT; }
#else
static bool isTaggedPointer(__unused const void* pointer) { return false; }
static int getTaggedSlot(__unused const void* pointer) { return 0; }
static uintptr_t getTaggedPayload(const void* pointer) { return (uintptr_t)pointer; }
#endif

/** Get class data for a tagged pointer.
 *
 * @param object The tagged pointer.
 * @return The class data.
 */
static const ClassData* getClassDataFromTaggedPointer(const void* const object)
{
    int slot = getTaggedSlot(object);
    return &g_taggedClassData[slot];
}

static bool isValidTaggedPointer(const void* object)
{
    if(isTaggedPointer(object))
    {
        if(getTaggedSlot(object) <= g_taggedClassDataCount)
        {
            const ClassData* classData = getClassDataFromTaggedPointer(object);
            return classData->type != PLObjCClassTypeUnknown;
        }
    }
    return false;
}

static const struct class_t* decodeIsaPointer(const void* const isaPointer)
{
#if ISA_TAG_MASK
    uintptr_t isa = (uintptr_t)isaPointer;
    if(isa & ISA_TAG_MASK)
    {
#if defined(__arm64__)
        if (floor(kCFCoreFoundationVersionNumber) <= kCFCoreFoundationVersionNumber_iOS_8_x_Max) {
            return (const struct class_t*)(isa & ISA_MASK_OLD);
        }
        return (const struct class_t*)(isa & ISA_MASK);
#else
        return (const struct class_t*)(isa & ISA_MASK);
#endif
    }
#endif
    return (const struct class_t*)isaPointer;
}

static const void* getIsaPointer(const void* const objectOrClassPtr)
{
    // This is wrong. Should not get class data here.
//    if(plobjc_isTaggedPointer(objectOrClassPtr))
//    {
//        return getClassDataFromTaggedPointer(objectOrClassPtr)->class;
//    }
    
    const struct class_t* ptr = objectOrClassPtr;
    return decodeIsaPointer(ptr->isa);
}

static inline struct class_rw_t* getClassRW(const struct class_t* const class)
{
    uintptr_t ptr = class->data_NEVER_USE & (~WORD_MASK);
    return (struct class_rw_t*)ptr;
}

static inline const struct class_ro_t* getClassRO(const struct class_t* const class)
{
    return getClassRW(class)->ro;
}

static inline bool isMetaClass(const void* const classPtr)
{
    return (getClassRO(classPtr)->flags & RO_META) != 0;
}

static inline bool isRootClass(const void* const classPtr)
{
    return (getClassRO(classPtr)->flags & RO_ROOT) != 0;
}

static inline const char* getClassName(const void* classPtr)
{
    const struct class_ro_t* ro = getClassRO(classPtr);
    return ro->name;
}

/** Check if a tagged pointer is a number.
 *
 * @param object The object to query.
 * @return true if the tagged pointer is an NSNumber.
 */
static bool isTaggedPointerNSNumber(const void* const object)
{
    return getTaggedSlot(object) == OBJC_TAG_NSNumber;
}

/** Check if a tagged pointer is a string.
 *
 * @param object The object to query.
 * @return true if the tagged pointer is an NSString.
 */
static bool isTaggedPointerNSString(const void* const object)
{
    return getTaggedSlot(object) == OBJC_TAG_NSString;
}

/** Check if a tagged pointer is a date.
 *
 * @param object The object to query.
 * @return true if the tagged pointer is an NSDate.
 */
static bool isTaggedPointerNSDate(const void* const object)
{
    return getTaggedSlot(object) == OBJC_TAG_NSDate;
}

/** Extract an integer from a tagged NSNumber.
 *
 * @param object The NSNumber object (must be a tagged pointer).
 * @return The integer value.
 */
static int64_t extractTaggedNSNumber(const void* const object)
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

static int getTaggedNSStringLength(const void* const object)
{
    uintptr_t payload = getTaggedPayload(object);
    return (int)(payload & 0xf);
}

static int extractTaggedNSString(const void* const object, char* buffer, int bufferLength)
{
    int length = getTaggedNSStringLength(object);
    int copyLength = ((length + 1) > bufferLength) ? (bufferLength - 1) : length;
    uintptr_t payload = getTaggedPayload(object);
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
static CFAbsoluteTime extractTaggedNSDate(const void* const object)
{
    uintptr_t payload = getTaggedPayload(object);
    // Payload is a 60-bit float. Fortunately we can just cast across from
    // an integer pointer after shifting out the upper 4 bits.
    payload <<= 4;
    CFAbsoluteTime value = *((CFAbsoluteTime*)&payload);
    return value;
}

static int stringPrintf(char* buffer, int bufferLength, const char* fmt, ...)
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
#pragma mark - Validation -
//======================================================================

// Lookup table for validating class/ivar names and objc @encode types.
// An ivar name must start with a letter, and can contain letters & numbers.
// An ivar type can in theory be any combination of numbers, letters, and symbols
// in the ASCII range (0x21-0x7e).
#define INV 0 // Invalid.
#define N_C 5 // Name character: Valid for anything except the first letter of a name.
#define N_S 7 // Name start character: Valid for anything.
#define T_C 4 // Type character: Valid for types only.

static const unsigned int g_nameChars[] =
{
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, T_C, T_C, T_C, T_C, T_C, T_C, T_C, T_C, T_C, T_C, T_C, T_C, T_C, T_C, T_C,
    N_C, N_C, N_C, N_C, N_C, N_C, N_C, N_C, N_C, N_C, T_C, T_C, T_C, T_C, T_C, T_C,
    T_C, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S,
    N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, T_C, T_C, T_C, T_C, N_S,
    T_C, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S,
    N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, N_S, T_C, T_C, T_C, T_C, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
    INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV, INV,
};

#define VALID_NAME_CHAR(A) ((g_nameChars[(uint8_t)(A)] & 1) != 0)
#define VALID_NAME_START_CHAR(A) ((g_nameChars[(uint8_t)(A)] & 2) != 0)
#define VALID_TYPE_CHAR(A) ((g_nameChars[(uint8_t)(A)] & 7) != 0)

static bool isValidName(const char* const name, const int maxLength)
{
    if((uintptr_t)name + (unsigned)maxLength < (uintptr_t)name)
    {
        // Wrapped around address space.
        return false;
    }

    char buffer[maxLength];
    int length = plmem_copyMaxPossible(name, buffer, maxLength);
    if(length == 0 || !VALID_NAME_START_CHAR(name[0]))
    {
        return false;
    }
    for(int i = 1; i < length; i++)
    {
        unlikely_if(!VALID_NAME_CHAR(name[i]))
        {
            if(name[i] == 0)
            {
                return true;
            }
            return false;
        }
    }
    return false;
}

static bool isValidIvarType(const char* const type)
{
    char buffer[100];
    const int maxLength = sizeof(buffer);

    if((uintptr_t)type + maxLength < (uintptr_t)type)
    {
        // Wrapped around address space.
        return false;
    }

    int length = plmem_copyMaxPossible(type, buffer, maxLength);
    if(length == 0 || !VALID_TYPE_CHAR(type[0]))
    {
        return false;
    }
    for(int i = 0; i < length; i++)
    {
        unlikely_if(!VALID_TYPE_CHAR(type[i]))
        {
            if(type[i] == 0)
            {
                return true;
            }
        }
    }
    return false;
}

static bool containsValidROData(const void* const classPtr)
{
    const struct class_t* const class = classPtr;
    if(!plmem_isMemoryReadable(class, sizeof(*class)))
    {
        return false;
    }
    class_rw_t* rw = getClassRW(class);
    if(!plmem_isMemoryReadable(rw, sizeof(*rw)))
    {
        return false;
    }
    const class_ro_t* ro = getClassRO(class);
    if(!plmem_isMemoryReadable(ro, sizeof(*ro)))
    {
        return false;
    }
    return true;
}

static bool containsValidIvarData(const void* const classPtr)
{
    const struct class_ro_t* ro = getClassRO(classPtr);
    const struct ivar_list_t* ivars = ro->ivars;
    if(ivars == NULL)
    {
        return true;
    }
    if(!plmem_isMemoryReadable(ivars, sizeof(*ivars)))
    {
        return false;
    }
    
    if(ivars->count > 0)
    {
        struct ivar_t ivar;
        uint8_t* ivarPtr = (uint8_t*)(&ivars->first) + ivars->entsizeAndFlags;
        for(uint32_t i = 1; i < ivars->count; i++)
        {
            if(!plmem_copySafely(ivarPtr, &ivar, sizeof(ivar)))
            {
                return false;
            }
            if(!plmem_isMemoryReadable(ivarPtr, (int)ivars->entsizeAndFlags))
            {
                return false;
            }
            if(!plmem_isMemoryReadable(ivar.offset, sizeof(*ivar.offset)))
            {
                return false;
            }
            if(!isValidName(ivar.name, kMaxNameLength))
            {
                return false;
            }
            if(!isValidIvarType(ivar.type))
            {
                return false;
            }
            ivarPtr += ivars->entsizeAndFlags;
        }
    }
    return true;
}

static bool containsValidClassName(const void* const classPtr)
{
    const struct class_ro_t* ro = getClassRO(classPtr);
    return isValidName(ro->name, kMaxNameLength);
}

static bool hasValidIsaPointer(const void* object) {
    const struct class_t* isaPtr = getIsaPointer(object);
    return plmem_isMemoryReadable(isaPtr, sizeof(*isaPtr));
}

static inline bool isValidClass(const void* classPtr)
{
    const class_t* class = classPtr;
    if(!plmem_isMemoryReadable(class, sizeof(*class)))
    {
        return false;
    }
    if(!containsValidROData(class))
    {
        return false;
    }
    if(!containsValidClassName(class))
    {
        return false;
    }
    if(!containsValidIvarData(class))
    {
        return false;
    }
    return true;
}

static inline bool isValidObject(const void* objectPtr)
{
    if(isTaggedPointer(objectPtr))
    {
        return isValidTaggedPointer(objectPtr);
    }
    const class_t* object = objectPtr;
    if(!plmem_isMemoryReadable(object, sizeof(*object)))
    {
        return false;
    }
    if(!hasValidIsaPointer(object))
    {
        return false;
    }
    if(!isValidClass(getIsaPointer(object)))
    {
        return false;
    }
    return true;
}



//======================================================================
#pragma mark - Basic Objective-C Queries -
//======================================================================


static const void* plobjc_baseClass(const void* const classPtr)
{
    const struct class_t* superClass = classPtr;
    const struct class_t* subClass = classPtr;
    
    for(int i = 0; i < 20; i++)
    {
        if(isRootClass(superClass))
        {
            return subClass;
        }
        subClass = superClass;
        superClass = superClass->superclass;
        if(!containsValidROData(superClass))
        {
            return NULL;
        }
    }
    return NULL;
}

static inline bool isBlockClass(const void* class)
{
    const void* baseClass = plobjc_baseClass(class);
    if(baseClass == NULL)
    {
        return false;
    }
    const char* name = getClassName(baseClass);
    if(name == NULL)
    {
        return false;
    }
    return strcmp(name, g_blockBaseClassName) == 0;
}

PLObjCType plobjc_objectType(const void* objectOrClassPtr)
{
    if(objectOrClassPtr == NULL)
    {
        return PLObjCTypeUnknown;
    }

    if(isTaggedPointer(objectOrClassPtr))
    {
        return PLObjCTypeObject;
    }
    
    if(!isValidObject(objectOrClassPtr))
    {
        return PLObjCTypeUnknown;
    }
    
    if(!isValidClass(objectOrClassPtr))
    {
        return PLObjCTypeUnknown;
    }
    
    const struct class_t* isa = getIsaPointer(objectOrClassPtr);

    if(isBlockClass(isa))
    {
        return PLObjCTypeBlock;
    }
    if(!isMetaClass(isa))
    {
        return PLObjCTypeObject;
    }
    
    return PLObjCTypeClass;
}


//======================================================================
#pragma mark - Unknown Object -
//======================================================================

static bool taggedObjectIsValid(const void* object)
{
    return isValidTaggedPointer(object);
}

static int taggedObjectDescription(const void* object, char* buffer, int bufferLength)
{
    const ClassData* data = getClassDataFromTaggedPointer(object);
    const char* name = data->name;
    uintptr_t objPointer = (uintptr_t)object;
    const char* fmt = sizeof(uintptr_t) == sizeof(uint32_t) ? "<%s: 0x%08x>" : "<%s: 0x%016x>";
    return stringPrintf(buffer, bufferLength, fmt, name, objPointer);
}


//======================================================================
#pragma mark - NSString -
//======================================================================

static bool taggedStringIsValid(const void* const object)
{
    return isValidTaggedPointer(object) && isTaggedPointerNSString(object);
}

static int taggedStringDescription(const void* object, char* buffer, __unused int bufferLength)
{
    return extractTaggedNSString(object, buffer, bufferLength);
}

//======================================================================
#pragma mark - NSDate -
//======================================================================

static bool taggedDateIsValid(const void* const datePtr)
{
    return isValidTaggedPointer(datePtr) && isTaggedPointerNSDate(datePtr);
}

static int taggedDateDescription(const void* object, char* buffer, int bufferLength)
{
    char* pBuffer = buffer;
    char* pEnd = buffer + bufferLength;

    CFAbsoluteTime time = extractTaggedNSDate(object);
    pBuffer += taggedObjectDescription(object, pBuffer, (int)(pEnd - pBuffer));
    pBuffer += stringPrintf(pBuffer, (int)(pEnd - pBuffer), ": %f", time);

    return (int)(pBuffer - buffer);
}


//======================================================================
#pragma mark - NSNumber -
//======================================================================

#define NSNUMBER_CASE(CFTYPE, RETURN_TYPE, CAST_TYPE, DATA) \
    case CFTYPE: \
    { \
        RETURN_TYPE result; \
        memcpy(&result, DATA, sizeof(result)); \
        return (CAST_TYPE)result; \
    }

#define EXTRACT_AND_RETURN_NSNUMBER(OBJECT, RETURN_TYPE) \
    if(isValidTaggedPointer(object)) \
    { \
        return extractTaggedNSNumber(object); \
    } \
    const struct __CFNumber* number = OBJECT; \
    CFNumberType cftype = CFNumberGetType((CFNumberRef)OBJECT); \
    const void *data = &(number->_pad); \
    switch(cftype) \
    { \
        NSNUMBER_CASE( kCFNumberSInt8Type,     int8_t,    RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberSInt16Type,    int16_t,   RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberSInt32Type,    int32_t,   RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberSInt64Type,    int64_t,   RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberFloat32Type,   Float32,   RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberFloat64Type,   Float64,   RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberCharType,      char,      RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberShortType,     short,     RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberIntType,       int,       RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberLongType,      long,      RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberLongLongType,  long long, RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberFloatType,     float,     RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberDoubleType,    double,    RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberCFIndexType,   CFIndex,   RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberNSIntegerType, NSInteger, RETURN_TYPE, data ) \
        NSNUMBER_CASE( kCFNumberCGFloatType,   CGFloat,   RETURN_TYPE, data ) \
    }

static bool taggedNumberIsValid(const void* const object)
{
    return isValidTaggedPointer(object) && isTaggedPointerNSNumber(object);
}

static int taggedNumberDescription(const void* object, char* buffer, int bufferLength)
{
    char* pBuffer = buffer;
    char* pEnd = buffer + bufferLength;

    int64_t value = extractTaggedNSNumber(object);
    pBuffer += taggedObjectDescription(object, pBuffer, (int)(pEnd - pBuffer));
    pBuffer += stringPrintf(pBuffer, (int)(pEnd - pBuffer), ": %" PRId64, value);

    return (int)(pBuffer - buffer);
}

//======================================================================
#pragma mark - General Queries -
//======================================================================

bool plobjc_isTaggedPointer(const void* const pointer)
{
    return isTaggedPointer(pointer);
}

bool plobjc_isValidTaggedPointer(const void* const pointer)
{
    return isValidTaggedPointer(pointer);
}
