/* group_buffer.c — buffer manager slots for the kwabi shim */

#include "shim_internal.h"

static Buffer
shim_buffer_get(Relation rel, BlockNumber blocknum)
{
    Buffer result = InvalidBuffer;

    if (rel == NULL)
        return InvalidBuffer;

    PG_TRY();
    {
        result = ReadBuffer(rel, blocknum);
    }
    PG_CATCH();
    {
        shim_capture_error();
        result = InvalidBuffer;
    }
    PG_END_TRY();

    return result;
}

static void
shim_buffer_release(Buffer buffer)
{
    if (buffer == InvalidBuffer)
        return;

    PG_TRY();
    {
        ReleaseBuffer(buffer);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

static Page
shim_buffer_get_page(Buffer buffer)
{
    if (buffer == InvalidBuffer)
        return NULL;

    return (Page) BufferGetPage(buffer);
}

static void
shim_buffer_mark_dirty(Buffer buffer)
{
    if (buffer == InvalidBuffer)
        return;

    PG_TRY();
    {
        MarkBufferDirty(buffer);
    }
    PG_CATCH();
    {
        shim_capture_error();
    }
    PG_END_TRY();
}

void
init_group_buffer(void)
{
    shim_table.buffer_get = shim_buffer_get;
    shim_table.buffer_release = shim_buffer_release;
    shim_table.buffer_get_page = shim_buffer_get_page;
    shim_table.buffer_mark_dirty = shim_buffer_mark_dirty;
}
