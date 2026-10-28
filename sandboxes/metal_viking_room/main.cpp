// main.cpp                                                           -*-C++-*-

#undef NS_PRIVATE_IMPLEMENTATION
#undef CA_PRIVATE_IMPLEMENTATION
#undef MTL_PRIVATE_IMPLEMENTATION
#undef MTK_PRIVATE_IMPLEMENTATION

// std
#include <cstdlib>

// vkr
#include <viking_room.h>

int main()
{
    vkr::VikingRoom app;
    app.run();
    return EXIT_SUCCESS;
}
