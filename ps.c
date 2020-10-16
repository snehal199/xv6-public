#include "types.h"
#include "fcntl.h"
#include "stat.h"
#include "user.h"

int
main(int argc,char *argv[])
{
    struct uproc *up=(struct uproc*)malloc(sizeof(struct uproc));
    getprocinfo(1,up);
    exit();
}