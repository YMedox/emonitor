#define _VERSION_ "1.12.5"
#define _BUILD_TYPE_ "Debug"
struct _version_ { char* version; char* machine; char* date; };
_version_ _v_ = { .version = (char *)_VERSION_, .machine = (char *)"YourPC", .date = (char *)"01.01.1970" };
