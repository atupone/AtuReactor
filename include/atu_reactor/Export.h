#pragma once

#if defined(_WIN32)
    #ifdef AtuReactor_EXPORTS
        #define ATU_API __declspec(dllexport)
    #else
        #define ATU_API __declspec(dllimport)
    #endif
#else
    #define ATU_API __attribute__((visibility("default")))
#endif
