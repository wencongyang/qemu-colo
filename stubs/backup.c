#include "block/block_int.h"

void backup_start(BlockDriverState *bs, BlockDriverState *target,
                  int64_t speed, MirrorSyncMode sync_mode,
                  BlockdevOnError on_source_error,
                  BlockdevOnError on_target_error,
                  BlockCompletionFunc *cb, void *opaque,
                  Error **errp)
{
    error_setg(errp, "this feature or command is not currently supported");
}
