//
//  PLCrashRegisterContent.c
//  CrashReporter-MacOSX
//
//  Created by Yousef Hamza on 11/14/19.
//

#include "PLCrashRegisterContent.h"
#include "PLObjC.h"
#include "PLMemory.h"
#include "PLString.h"
#include "KSObjCApple.h"

typedef struct
{
    const void* object;
    const char* className;
} PLZombie;

static volatile PLZombie* g_zombieCache;
static unsigned g_zombieHashMask;

static inline bool plregister_is_valid_pointer(const uintptr_t address)
{
    if(address == (uintptr_t)NULL)
    {
        return false;
    }

    if(plobjc_isTaggedPointer((const void*)address))
    {
        if(!plobjc_isValidTaggedPointer((const void*)address))
        {
            return false;
        }
    }

    return true;
}

static inline unsigned hashIndex(const void* object)
{
    uintptr_t objPtr = (uintptr_t)object;
    objPtr >>= (sizeof(object)-1);
    return objPtr & g_zombieHashMask;
}

static inline const char* plregister_plzombie_className(const void* object)
{
    volatile PLZombie* cache = g_zombieCache;
    if(cache == NULL || object == NULL)
    {
        return NULL;
    }

    PLZombie* zombie = (PLZombie*)cache + hashIndex(object);
    if(zombie->object == object)
    {
        return zombie->className;
    }
    return NULL;
}

bool isValidString(const void* const address)
{
    if((void*)address == NULL)
    {
        return false;
    }

    char buffer[500];
    if((uintptr_t)address+sizeof(buffer) < (uintptr_t)address)
    {
        // Wrapped around the address range.
        return false;
    }
    if(!plmem_copySafely(address, buffer, sizeof(buffer)))
    {
        return false;
    }
    return plstring_isNullTerminatedUTF8String(buffer, kPLMinStringLength, sizeof(buffer));
}

bool plregister_is_notable_address(const uintptr_t address)
{
    if(!plregister_is_valid_pointer(address))
    {
        return false;
    }
    
    const void* object = (const void*)address;

    if(plregister_plzombie_className(object) != NULL)
    {
        return true;
    }

    if(plobjc_objectType(object) != PLObjCTypeUnknown)
    {
        return true;
    }

    if(isValidString(object))
    {
        return true;
    }

    return false;
}
