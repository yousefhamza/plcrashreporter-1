//
//  PLCrashRegisterContent.h
//  CrashReporter-MacOSX
//
//  Created by Yousef Hamza on 11/14/19.
//

#ifndef PLCrashRegisterContent_h
#define PLCrashRegisterContent_h

#include <stdio.h>
#include <objc/runtime.h>

bool plregister_is_notable_address(const uintptr_t address);
char const *plregister_get_content(const char* const regname, const uintptr_t address);

#endif /* PLCrashRegisterContent_h */
