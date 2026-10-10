// SPDX-License-Identifier: MIT
#include "CnaService/DatabaseLock.hpp"
#include "CnaService/Listener.hpp"
#include "CnaService/Protocol.hpp"
#include "CnaService/Version.hpp"
#include "CnaService/Store.hpp"
#include <charconv>
#include <iostream>
#include <map>
#include <set>
namespace {
unsigned short port(const std::string& text) {
    unsigned int value=0;
    const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
    if(text.empty()||error!=std::errc{}||end!=text.data()+text.size()||value>65535)
        throw CnaService::Error("INVALID_ARGUMENT");
    return static_cast<unsigned short>(value);
}
void usage() {
    std::cout<<"Usage: cna-gamer-services-server --database PATH [options]\n"
        "  --listen ADDRESS             Control listener address (default 127.0.0.1)\n"
        "  --port PORT                  Control listener port (default 47831; 0 chooses one)\n"
        "  --cert PATH --key PATH       TLS certificate and private key\n"
        "  --insecure-loopback          Disable TLS only on a loopback control listener\n"
        "  --diagnostics-port PORT      Enable loopback HTTP health and metrics\n"
        "  --diagnostics-listen ADDRESS Diagnostics address (default 127.0.0.1)\n"
        "  --log-format human|json      Operational log format (default human)\n"
        "  --help                       Show this help\n"
        "  --version                    Show the server version\n";
}
}
int main(int argc,char** argv) {
    bool jsonLogging=false;
    try {
        for(int i=1;i<argc;++i) {
            const std::string argument=argv[i];
            if(argument=="--help"){usage();return 0;}
            if(argument=="--version"){std::cout<<"cna-gamer-services-server "<<CNA_GAMER_SERVICES_VERSION_STRING<<'\n';return 0;}
        }
        std::map<std::string,std::string> args;bool insecure=false;
        for(int i=1;i<argc;++i) {
            if(std::string(argv[i])=="--insecure-loopback")insecure=true;
            else { if(i+1>=argc)throw CnaService::Error("INVALID_ARGUMENT");const std::string key=argv[i];args[key]=argv[++i]; }
        }
        const auto logFormat=args.contains("--log-format")?args["--log-format"]:"human";
        if(logFormat!="human"&&logFormat!="json")throw CnaService::Error("INVALID_ARGUMENT");
        jsonLogging=logFormat=="json";
        const std::set<std::string> valued{"--database","--listen","--port","--cert","--key",
            "--diagnostics-listen","--diagnostics-port","--log-format"};
        for(const auto& [key,value]:args) { (void)value;if(!valued.contains(key))throw CnaService::Error("INVALID_ARGUMENT"); }
        if(!args.contains("--database"))throw CnaService::Error("INVALID_ARGUMENT");
        const auto controlPort=port(args.contains("--port")?args["--port"]:"47831");
        const bool diagnostics=args.contains("--diagnostics-port");
        if(args.contains("--diagnostics-listen")&&!diagnostics)throw CnaService::Error("INVALID_ARGUMENT");
        const auto diagnosticsPort=diagnostics?port(args["--diagnostics-port"]):0;
        // One server process owns a database: relay routing, rate limits and caches live in its
        // memory, so a second process on the same file would split them. The lock is held until
        // the process exits; the admin tool deliberately takes none and works beside a server.
        const CnaService::DatabaseLock owner(args["--database"]);
        if(owner.result()==CnaService::DatabaseLock::Result::InUse)throw CnaService::Error("DATABASE_IN_USE");
        if(owner.result()!=CnaService::DatabaseLock::Result::Owned)throw CnaService::Error("DATABASE_LOCK_FAILED");
        CnaService::listen(args["--database"],args.contains("--listen")?args["--listen"]:"127.0.0.1",
            controlPort,args["--cert"],args["--key"],insecure,diagnostics,
            args.contains("--diagnostics-listen")?args["--diagnostics-listen"]:"127.0.0.1",
            diagnosticsPort,jsonLogging);
        return 0;
    } catch(const CnaService::Error& e){
        if(jsonLogging)std::cerr<<CnaService::Json{{"timestamp",CnaService::now()},{"level","error"},
            {"event","startup_failed"},{"code",e.code()}}.dump()<<'\n';
        else std::cerr<<e.code()<<'\n';
    } catch(...){
        if(jsonLogging)std::cerr<<CnaService::Json{{"timestamp",CnaService::now()},{"level","error"},
            {"event","startup_failed"},{"code","CONFIGURATION_OR_TRANSPORT_FAILURE"}}.dump()<<'\n';
        else std::cerr<<"Server configuration or transport failure\n";
    }
    return 1;
}
