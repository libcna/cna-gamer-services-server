// SPDX-License-Identifier: MS-PL
#include "CnaService/Listener.hpp"
#include "CnaService/Protocol.hpp"
#include <iostream>
#include <map>
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
        CnaService::listen(args["--database"],args.contains("--listen")?args["--listen"]:"127.0.0.1",
            static_cast<unsigned short>(port),args["--cert"],args["--key"],insecure);
        return 0;
    } catch(const CnaService::Error& e){std::cerr<<e.code()<<'\n';}
      catch(...){std::cerr<<"Server configuration or transport failure\n";}
    return 1;
}
