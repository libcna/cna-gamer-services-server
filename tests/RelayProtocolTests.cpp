// SPDX-License-Identifier: MIT
#include "CnaService/RelayProtocol.hpp"
#include "CnaService/Protocol.hpp"
#include <fstream>
#include <iostream>
#include <random>
using namespace CnaService;
namespace {
int checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
std::vector<unsigned char> bytes(const std::string& value) {
    std::vector<unsigned char> result;
    for(std::size_t i=0;i<value.size();i+=2)result.push_back(static_cast<unsigned char>(std::stoul(value.substr(i,2),nullptr,16)));
    return result;
}
template<class Work> void error(Work work,const std::string& expected) {
    try{work();throw std::runtime_error("expected relay refusal");}catch(const RelayError& actual){check(actual.code()==expected,"relay error code");}
}
}
int main() {
    try {
        std::ifstream input(CNA_RELAY_VECTORS);Json corpus;input>>corpus;
        check(corpus["version"]==1,"golden version");
        for(const auto& vector:corpus["vectors"]) {
            const auto frame=bytes(vector["hex"].get<std::string>());const auto expected=vector["error"].get<std::string>();
            if(expected!="OK"){error([&]{(void)parseRelayFrame(frame);},expected);continue;}
            const auto parsed=parseRelayFrame(frame);
            check(relayMachineName(parsed.machine)==vector["machine"].get<std::string>(),"golden identity");
            check(std::vector<unsigned char>(parsed.datagram.begin(),parsed.datagram.end())==bytes(vector["payload"].get<std::string>()),"golden payload");
            check(parsed.datagram.data()==frame.data()+RelayHeaderBytes,"borrowed payload");
            check(encodeRelayFrame(parsed.machine,parsed.datagram)==frame,"golden byte encoding");
        }
        const auto machine=relayMachineId("00112233445566778899aabbccddeeff");
        std::vector<unsigned char> payload(MaxRelayDatagramBytes,0xff);auto maximum=encodeRelayFrame(machine,payload);
        check(maximum.size()==MaxRelayFrameBytes&&parseRelayFrame(maximum).datagram.size()==MaxRelayDatagramBytes,"maximum datagram");
        maximum.push_back(0);error([&]{(void)parseRelayFrame(maximum);},"RELAY_TOO_LARGE");
        payload.push_back(0);error([&]{(void)encodeRelayFrame(machine,payload);},"RELAY_TOO_LARGE");
        error([&]{(void)encodeRelayFrame(machine,{});},"RELAY_EMPTY_PAYLOAD");
        error([&]{(void)encodeRelayFrame(RelayMachineId{},std::array<unsigned char,1>{0});},"RELAY_MACHINE_ID");
        for(const auto* invalid:{"","../file","00000000000000000000000000000000","00112233445566778899AABBCCDDEEFF","00112233445566778899aabbccddeefg"})
            error([&]{(void)relayMachineId(invalid);},"RELAY_MACHINE_ID");
        std::mt19937 rng(0xc0a108);
        const auto base=bytes(corpus["vectors"][0]["hex"].get<std::string>());
        for(int i=0;i<10000;++i) {
            auto candidate=base;const auto mode=rng()%3;
            if(mode==0)candidate.resize(rng()%(MaxRelayFrameBytes+2),0x55);
            else candidate[rng()%candidate.size()]=static_cast<unsigned char>(rng());
            try {
                const auto parsed=parseRelayFrame(candidate);
                check(encodeRelayFrame(parsed.machine,parsed.datagram)==candidate,"accepted mutation roundtrip");
            }catch(const RelayError& e){check(e.code().starts_with("RELAY_"),"safe mutation refusal");}
        }
        std::cout<<checks<<" relay protocol assertions passed\n";return 0;
    }catch(const std::exception&){std::cerr<<"relay protocol test failed\n";return 1;}
}
