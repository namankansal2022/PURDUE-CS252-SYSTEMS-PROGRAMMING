#include <pthread.h>
#include <semaphore.h>
#include <assert.h>
#include <stdio.h>

enum {MaxSize = 10};

class BoundedBuffer{
  int _queue[MaxSize];
  int _head;
  int _tail;
  pthread_mutex_t _mutex;
  sem_t _emptySem;
  sem_t _fullSem;
public:
  BoundedBuffer();
  void enqueue(int val);
  int dequeue();
};

BoundedBuffer::BoundedBuffer() {
  _head = 0;
  _tail = 0;
  pthread_mutex_init(&_mutex, NULL);
  sem_init(&_emptySem, 0, MaxSize);
  sem_init(&_fullSem, 0, 0);
}

void
BoundedBuffer::enqueue(int val)
{
  sem_wait(&_emptySem);
  pthread_mutex_lock(&_mutex);
  _queue[_tail] = val;
  _tail = (_tail + 1) % MaxSize;
  pthread_mutex_unlock(&_mutex);
  sem_post(&_fullSem);
}

int
BoundedBuffer::dequeue()
{
  sem_wait(&_fullSem);
  pthread_mutex_lock(&_mutex);
  int val = _queue[_head];
  _head = (_head + 1) % MaxSize;
  pthread_mutex_unlock(&_mutex);
  sem_post(&_emptySem);
  return val;
}

struct ThreadArgs {
   BoundedBuffer *queue;
   int n;
};

void 
producerThread(ThreadArgs * args) {
    printf("Running producer thread %d times\n", args->n);
    for (int i = 0; i < args->n; i++) {
        args->queue->enqueue(i);
    }
}

void 
consumerThread(ThreadArgs * args) {
    int lastVal = 0;
    printf("Running consumer thread %d times\n", args->n);
    for (int i = 0; i < args->n; i++) {
        int val = args->queue->dequeue();
        assert(val == lastVal);
        lastVal++;
    }
}

int main( int argc, char ** argv )
{
        pthread_t t1, t2;
        pthread_attr_t attr;

        pthread_attr_init( &attr );
        pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);

        ThreadArgs threadArgs;
        threadArgs.queue = new BoundedBuffer();
        threadArgs.n = 1000000;

        pthread_create( &t1, &attr, (void * (*)(void *)) producerThread, &threadArgs);
        pthread_create( &t2, &attr, (void * (*)(void *)) consumerThread, &threadArgs);

        pthread_join( t1, NULL );
        pthread_join( t2, NULL );

        printf("Semaphores passed\n");
}
