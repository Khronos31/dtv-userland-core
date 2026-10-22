#include <dtv/userland/control_connection.h>

int main()
{
    dtv::userland::wire::ServerHello server;
    dtv::userland::wire::ControlConnection connection(server);
    return connection.established() ? 1 : 0;
}
