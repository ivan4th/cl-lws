/* Odds and ends that need platform headers. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include "csmb.h"

int csmb_test_openpty(int *master_fd, char *slave_path, size_t path_len)
{
    int mfd, sfd;
    char name[128];

    if (openpty(&mfd, &sfd, name, NULL, NULL) < 0)
        return -1;
    /* The engine opens the slave side by path itself; we only needed
     * the name.  Keep the master side open (returned to the caller). */
    close(sfd);
    if (strlen(name) + 1 > path_len) {
        close(mfd);
        return -1;
    }
    strcpy(slave_path, name);
    *master_fd = mfd;
    return 0;
}
