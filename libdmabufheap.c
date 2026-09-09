#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

typedef struct {
   int fd;
} BufferAllocator;

BufferAllocator* CreateDmabufHeapBufferAllocator() {
    return malloc(sizeof(BufferAllocator));
}

void FreeDmabufHeapBufferAllocator(BufferAllocator* buffer_allocator) {
   free(buffer_allocator);
};

int DmabufHeapAlloc(BufferAllocator* buffer_allocator, const char* heap_name, size_t len,
                    unsigned int heap_flags) {
   printf("DmabufHeapAlloc\n");
   return -EINVAL;
}

int MapDmabufHeapNameToIonHeap(BufferAllocator* buffer_allocator, const char* heap_name,
                               const char* ion_heap_name, unsigned int ion_heap_flags,
                               unsigned int legacy_ion_heap_mask,
                               unsigned int legacy_ion_heap_flags) {
   printf("MapDmabufHeapNameToIonHeap\n");
   return -EINVAL;
}
