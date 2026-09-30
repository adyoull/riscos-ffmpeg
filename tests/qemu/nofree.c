/* LD_PRELOAD for faststart.sh: free() never gives memory back, so nothing
   freed is ever handed out again. A legal allocator; on RISC OS a freed
   block isn't sure to come back either. Patch 0020's first version closed
   and reopened the output's AVIOContext while movenc kept the old pointer,
   and only worked when the new one came back at the same address. */
#include <stdlib.h>
void free(void *p) { (void)p; }
