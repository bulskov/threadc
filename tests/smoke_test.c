/* Scaffold smoke test: the public header compiles under the project's warning
 * flags, the library links, and the generated version header exists.
 * Replace with real tests as the implementation lands. */

#include "ctt.h"
#include "threadc/threadc.h"
#include "threadc/version.h"

TEST(version_header_is_generated)
{
    ASSERT_TRUE(sizeof(THREADC_VERSION_STRING) > 1);
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "threadc smoke");
}
