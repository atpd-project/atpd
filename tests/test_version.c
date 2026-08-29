#include "version.h"

#include <assert.h>
#include <string.h>

int main(void) {
    assert(strcmp(atp_get_version(), "1.0.0") == 0);
    assert(strstr(atp_get_full_version(), "1.0.0") == atp_get_full_version());
    assert(atp_get_commit()[0] != '\0');
    return 0;
}
