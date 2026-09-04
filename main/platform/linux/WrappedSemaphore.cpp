#include "WrappedSemaphore.h"
#include <sys/time.h>

using namespace bell;

WrappedSemaphore::WrappedSemaphore(int count) {
  sem_init(&this->semaphoreHandle, 0, 0);  // eek pointer
}

WrappedSemaphore::~WrappedSemaphore() {
  sem_destroy(&this->semaphoreHandle);
}

int WrappedSemaphore::wait() {
  sem_wait(&this->semaphoreHandle);
  return 0;
}

int WrappedSemaphore::twait(long milliseconds) {
  // wait on semaphore with timeout
  struct timespec ts;
  struct timeval tv;

  gettimeofday(&tv, 0);

  // both terms can approach one second, so the sum has to be split rather than
  // stored straight into tv_nsec: sem_timedwait() fails with EINVAL unless
  // tv_nsec is below one second, and failing turns a polling caller into a
  // busy loop
  long nsec = tv.tv_usec * 1000 + (milliseconds % 1000) * 1000000;

  ts.tv_sec = tv.tv_sec + milliseconds / 1000 + nsec / 1000000000;
  ts.tv_nsec = nsec % 1000000000;

  return sem_timedwait(&this->semaphoreHandle, &ts);
}

void WrappedSemaphore::give() {
  sem_post(&this->semaphoreHandle);
}
