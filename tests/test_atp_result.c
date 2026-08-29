#include "atp_result.h"

#include <assert.h>
#include <string.h>

int main(void) {
    const struct {
        atp_result_t result;
        const char *text;
    } cases[] = {
        {ATP_OK, "Success"},
        {ATP_ERR_GENERAL, "General error"},
        {ATP_ERR_NOMEM, "Out of memory"},
        {ATP_ERR_NOENT, "Not found"},
        {ATP_ERR_PERM, "Permission denied"},
        {ATP_ERR_TIMEOUT, "Operation timed out"},
        {ATP_ERR_BUSY, "Resource busy"},
        {ATP_ERR_INVAL, "Invalid argument"},
        {ATP_ERR_IO, "I/O error"},
        {ATP_ERR_CONFIG, "Configuration error"},
        {ATP_ERR_NOTSUP, "Operation not supported"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        assert(strcmp(atp_result_string(cases[i].result), cases[i].text) == 0);
    }
    assert(strcmp(atp_result_string((atp_result_t)-999), "Unknown error") == 0);
    return 0;
}
