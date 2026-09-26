// The doctest runner shared by every test executable that uses doctest. It is compiled
// once as an object library; each suite defines only its own TEST_CASEs.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
