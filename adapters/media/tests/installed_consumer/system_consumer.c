#include <mgmem/media_read_bridge.h>

int main(void) {
    mgmem_handle_v1 output = {0};
    int32_t status = -1;
    return mgmem_media_import_read_v1(0, 0, 1, 0, 0, &output, &status) == MGMEM_INVALID_ARGUMENT &&
           output.id == 0 && status == 0 ? 0 : 1;
}
