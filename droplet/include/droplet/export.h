#pragma once

#if defined(_WIN32) && defined(DROPLET_SHARED_LIBRARY)
#if defined(DROPLET_BUILDING_LIBRARY)
#define DROPLET_API __declspec(dllexport)
#else
#define DROPLET_API __declspec(dllimport)
#endif


#elif defined(__GNUC__) && defined(DROPLET_SHARED_LIBRARY)
#define DROPLET_API __attribute__((visibility("default")))
#else
#define DROPLET_API
#endif