#include "net.h"
#include "config.h"

int main(int argc, char **argv)
{
    Config c = loadconfig(argc > 1 ? argv[1] : "conf/server.conf");
    return serve(&c);
}
