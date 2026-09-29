// SPDX-License-Identifier: MIT
#include "CnaService/Listener.hpp"
#include "CnaService/Protocol.hpp"
#include <iostream>
#include <map>
#ifdef __unix__
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace {
// One server process owns a database: relay routing, rate limits and caches live in its memory, so
// a second process on the same file would split them. The lock is held until the process exits;
// the admin tool deliberately takes none and works beside a running server.
void ownDatabase(const std::string& database) {
#ifdef __unix__
    const auto path=database+".lock";
    const int fd=::open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);
    if(fd<0)throw CnaService::Error("DATABASE_LOCK_FAILED");
    if(::flock(fd,LOCK_EX|LOCK_NB)!=0){::close(fd);throw CnaService::Error("DATABASE_IN_USE");}
#else
    (void)database;
#endif
}
}
int main(int argc,char** argv) {
    try {
        std::map<std::string,std::string> args;bool insecure=false;
        for(int i=1;i<argc;++i) {
            if(std::string(argv[i])=="--insecure-loopback")insecure=true;
            else { if(i+1>=argc)throw CnaService::Error("INVALID_ARGUMENT");const std::string key=argv[i];args[key]=argv[++i]; }
        }
        for(const auto& [key,value]:args) { (void)value;if(key!="--database"&&key!="--listen"&&key!="--port"&&key!="--cert"&&key!="--key")throw CnaService::Error("INVALID_ARGUMENT"); }
        if(!args.contains("--database"))throw CnaService::Error("INVALID_ARGUMENT");
        const auto portText=args.contains("--port")?args["--port"]:"47831";
        std::size_t used=0;const int port=std::stoi(portText,&used);
        if(used!=portText.size()||port<0||port>65535)throw CnaService::Error("INVALID_ARGUMENT");
        ownDatabase(args["--database"]);
        CnaService::listen(args["--database"],args.contains("--listen")?args["--listen"]:"127.0.0.1",
            static_cast<unsigned short>(port),args["--cert"],args["--key"],insecure);
        return 0;
    } catch(const CnaService::Error& e){std::cerr<<e.code()<<'\n';}
      catch(...){std::cerr<<"Server configuration or transport failure\n";}
    return 1;
}
