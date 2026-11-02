// Force-included into miniaudio.c on ESP-IDF (see rive_sources.cmake).
// ESP-IDF's pthreads have no scheduling-policy attributes. miniaudio only
// uses them to raise a device thread's priority, which it never creates
// here (MA_NO_DEVICE_IO), so they can safely do nothing.
#pragma once
#include <pthread.h>

static inline int pthread_attr_setschedpolicy(pthread_attr_t* attr, int policy)
{
  (void)attr;
  (void)policy;
  return 0;
}

static inline int pthread_attr_setinheritsched(pthread_attr_t* attr, int inherit)
{
  (void)attr;
  (void)inherit;
  return 0;
}

#ifndef PTHREAD_EXPLICIT_SCHED
#define PTHREAD_EXPLICIT_SCHED 1
#endif
