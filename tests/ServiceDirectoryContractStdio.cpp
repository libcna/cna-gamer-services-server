// SPDX-License-Identifier: MIT
// Private socket-free fixture transport. This executable is not a service endpoint.
#include "CnaService/Service.hpp"
#include <iostream>
#include <map>
#include <memory>
using namespace CnaService;
int main(int argc,char** argv) {
    try {
        if(argc!=2)return 2;
        {Store db(argv[1]);db.title("one","One");for(const auto* name:{"alice","bob","charlie","dana"})db.user(name,std::string(name)+"-password",name);}
        Service service(argv[1]);std::map<std::string,RelayGrant> relay;std::map<std::string,QosBinding> qos;
        std::cout<<"ready\n"<<std::flush;
        for(std::string line;std::getline(std::cin,line);) {
            Json reply{{"ok",false},{"value","fixture-command-failed"}};
            try {
                if(line.size()>2*MaxMessageBytes)throw Error("LIMIT_EXCEEDED");
                const auto request=parse(line);const auto action=stringField(request,"action",32),value=stringField(request,"value",MaxMessageBytes);
                std::string result;
                if(action=="handle")result=service.handle(value,"directory-contract-stdio");
                else if(action=="scalar") {Store db(argv[1]);Statement q(db.db(),value.c_str());if(!q.row())throw Error("NOT_FOUND");result=std::to_string(q.number(0));}
                else if(action=="exec") {Store db(argv[1]);db.exec(value.c_str());result="true";}
                else if(action=="relay-redeem") {relay[value]=service.redeemRelayTicket("one",value);result="true";}
                else if(action=="relay-valid")result=relay.contains(value)&&service.validateRelayGrant(relay.at(value))?"true":"false";
                else if(action=="qos-redeem") {qos[value]=service.redeemQosTicket("one",value);result="true";}
                else if(action=="qos-valid")result=qos.contains(value)&&service.validateQosGrant(qos.at(value))?"true":"false";
                else throw Error("UNKNOWN_OPERATION");
                reply={{"ok",true},{"value",result}};
            }catch(const std::exception&){}
            std::cout<<reply.dump()<<'\n'<<std::flush;
        }
        return 0;
    }catch(const std::exception&){std::cerr<<"directory contract fixture startup failed\n";return 1;}
}
