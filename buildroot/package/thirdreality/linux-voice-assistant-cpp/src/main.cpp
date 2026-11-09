#include <malloc.h>
#include <unistd.h>

#include <utility>

#include "app/Application.h"
#include "app/CliOptions.h"

int main(int argc, char** argv) {
    // Configure arenas before any worker thread starts.
    ::mallopt(M_ARENA_MAX, 1);
    ::mallopt(M_MMAP_THRESHOLD, 128 * 1024);
    ::mallopt(M_TRIM_THRESHOLD, 256 * 1024);
    lva::app::CliOptions cli;
    if (!lva::app::ParseCli(argc, argv, cli)) return 2;
    int rc;
    {
        lva::app::Application application(std::move(cli));
        rc = application.Run();
    }
    // All application owners have been destroyed. Preserve the embedded
    // process's existing exit policy for third-party globals/diagnostic threads.
    ::_exit(rc);
}
