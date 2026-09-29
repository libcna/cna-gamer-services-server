// SPDX-License-Identifier: MIT
#include "AvatarFixtures.hpp"
#include "CnaService/Avatars.hpp"
#include "CnaService/Service.hpp"
#include <array>
#include <filesystem>
#include <iostream>
using namespace CnaService;
namespace {
int checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
Json call(Service& service,const std::string& op,const Json& args,const std::string& token,const std::string& title="one") {
    static int sequence=0;
    return parse(service.handle(Json{{"v",1},{"id","avatar-"+std::to_string(++sequence)},{"op",op},{"game",title},{"token",token},{"args",args}}.dump(),"avatar-test"));
}
// Valid minimal models: each name gets distinct contents, the male rig differs from the female one.
std::string glb(const std::string& tag,int clips=0,float scale=1.0f) {
    auto g=Fixtures::avatarGlb(tag,clips,scale);
    return g.build();
}
struct Catalog {std::string manifest;std::map<std::string,std::string> files;};
Catalog catalog(int version,const std::vector<std::pair<int,std::string>>& items,char tag='a') {
    Catalog c;
    auto add=[&](const std::string& name,const std::string& bytes){c.files[name]=bytes;};
    add("body.female.glb",glb("female"));add("body.male.glb",glb("male",0,1.1f));add("face_features.png",Fixtures::atlasPng());
    add("animations.glb",glb("animations",31));
    Json list=Json::array(), features=Json::array();
    for(const auto& [id,slot]:items) {
        const auto name="item"+std::to_string(id);
        add(name+".female.glb",glb(name+"f"+tag));add(name+".male.glb",glb(name+"m"+tag,0,1.1f));
        (slot=="facialHair"?features:list).push_back(Json{{"id",id},{"slot",slot},{"name",name},
            {"assets",{{"female",name+".female.glb"},{"male",name+".male.glb"}}}});
    }
    Json assets=Json::array();
    for(const auto& [name,bytes]:c.files)assets.push_back(Json{{"name",name},{"sha256",sha256(bytes)},{"size",bytes.size()}});
    Json manifest{{"format",1},{"catalogVersion",version},{"rig","cna-avatar-71"},{"assets",assets},{"items",list},
        {"bodies",{{"female",{{"asset","body.female.glb"},{"authoredHeightMillimeters",1680}}},{"male",{{"asset","body.male.glb"},{"authoredHeightMillimeters",1800}}}}},
        {"face",{{"asset","face_features.png"},{"layout",Fixtures::atlasLayout()}}},{"animations",{{"asset","animations.glb"},{"presets",Json::array()}}}};
    if(!features.empty())manifest["featureItems"]=features;
    c.manifest=manifest.dump();
    return c;
}
std::string description(int version,std::array<int,6> items,int body=1,int height=1800) {
    std::string d(AvatarDescriptionSize,'\0');
    d[0]=1;d[1]='C';d[2]='N';d[3]='A';d[4]=static_cast<char>(body);
    d[5]=static_cast<char>(height&0xff);d[6]=static_cast<char>(height>>8);d[7]=static_cast<char>(128);
    d[8]=static_cast<char>(version&0xff);d[9]=static_cast<char>(version>>8);
    for(int i=10;i<31;++i)d[static_cast<std::size_t>(i)]=static_cast<char>(i*7);
    for(int slot=0;slot<6;++slot){d[31+slot*2]=static_cast<char>(items[slot]&0xff);d[32+slot*2]=static_cast<char>(items[slot]>>8);}
    const auto crc=crc32(std::string_view(d).substr(0,AvatarDescriptionSize-4));
    for(int i=0;i<4;++i)d[AvatarDescriptionSize-4+i]=static_cast<char>((crc>>(8*i))&0xff);
    return d;
}
std::string hex(std::string_view bytes){constexpr char digits[]="0123456789abcdef";std::string t;for(unsigned char b:bytes){t+=digits[b>>4];t+=digits[b&15];}return t;}
std::string refreshCrc(std::string d) {
    const auto crc=crc32(std::string_view(d).substr(0,AvatarDescriptionSize-4));
    for(int i=0;i<4;++i)d[AvatarDescriptionSize-4+i]=static_cast<char>((crc>>(8*i))&0xff);
    return d;
}
}
int main() {
    const auto path=std::filesystem::current_path()/"avatar-unit.sqlite3";
    auto clean=[&]{for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(path.string()+suffix);};
    try {
        clean();
        std::array<std::string,2> users;
        {Store store(path.string());store.title("one","One");store.title("two","Two");
         users[0]=store.user("alice","alice-password","Alice");users[1]=store.user("bob","bob-password","Bob");
         Statement version(store.db(),"PRAGMA user_version");(void)version.row();check(version.number(0)==SchemaVersion,"current schema");}
        Service service(path.string());std::array<std::string,2> tokens;
        int index=0;for(const auto* name:{"alice","bob"}) {
            auto login=call(service,"auth.login",{{"username",name},{"password",std::string(name)+"-password"}},{});
            check(login["error"]=="OK","fixture authentication");tokens[index++]=login["result"]["token"].get<std::string>();
        }
        check(call(service,"hello",Json::object(),{})["result"]["capabilities"].dump().find("\"avatars\"")!=std::string::npos,"capability");
        check(call(service,"avatars.get",{{"userIds",Json::array({users[0]})}},"")["error"]=="UNAUTHENTICATED","signed-in only");
        // Without an imported catalog nothing can be described.
        check(call(service,"avatars.catalog",Json::object(),tokens[0])["error"]=="NOT_FOUND","no catalog yet");
        const std::array<int,6> outfit{1,20,40,60,0,0};
        check(call(service,"avatars.set",{{"description",hex(description(1,outfit))}},tokens[0])["error"]=="INVALID_ARGUMENT","unknown catalog");
        {Store store(path.string());bool refused=false;try{(void)randomAvatarDescription(store.db(),std::nullopt);}catch(const Error& e){refused=e.code()=="NOT_FOUND";}
         check(refused,"random needs a catalog");}

        // Import validation.
        const std::vector<std::pair<int,std::string>> v1{{1,"hair"},{2,"hair"},{20,"top"},{40,"bottom"},{60,"shoes"},{80,"glasses"},{100,"hat"}};
        {Store store(path.string());
         auto bad=catalog(1,v1);bad.files["body.male.glb"]=glb("z",0,1.1f);
         bool refused=false;try{importAvatarCatalog(store,bad.manifest,bad.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
         check(refused,"hash mismatch refused");
         auto missing=catalog(1,v1);missing.files.erase("animations.glb");
         refused=false;try{importAvatarCatalog(store,missing.manifest,missing.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
         check(refused,"missing file refused");
         auto notGlb=catalog(1,v1);notGlb.files["body.male.glb"]="plain text of the right size.......";
         auto m=parse(notGlb.manifest);for(auto& asset:m["assets"])if(asset["name"]=="body.male.glb"){asset["sha256"]=sha256(notGlb.files["body.male.glb"]);asset["size"]=notGlb.files["body.male.glb"].size();}
         refused=false;try{importAvatarCatalog(store,m.dump(),notGlb.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
         check(refused,"file type checked");
         auto traversal=catalog(1,v1);auto t=parse(traversal.manifest);t["assets"][0]["name"]="../x.glb";
         refused=false;try{importAvatarCatalog(store,t.dump(),traversal.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
         check(refused,"asset names are plain file names");
         const auto good=catalog(1,v1);
         check(importAvatarCatalog(store,good.manifest,good.files)==1,"catalog v1 imported");
         check(importAvatarCatalog(store,good.manifest,good.files)==1,"identical re-import is a no-op");
         auto changed=catalog(1,v1,'q');
         refused=false;try{importAvatarCatalog(store,changed.manifest,changed.files);}catch(const Error& e){refused=e.code()=="CONFLICT";}
         check(refused,"a version cannot change");
         auto shrunk=catalog(2,{{1,"hair"},{20,"top"},{40,"bottom"},{60,"shoes"},{80,"glasses"},{100,"hat"}});
         refused=false;try{importAvatarCatalog(store,shrunk.manifest,shrunk.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
         check(refused,"items never disappear");
         auto moved=catalog(2,{{1,"top"},{2,"hair"},{20,"top"},{40,"bottom"},{60,"shoes"},{80,"glasses"},{100,"hat"}});
         refused=false;try{importAvatarCatalog(store,moved.manifest,moved.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
         check(refused,"items never change slot");}

        auto current=call(service,"avatars.catalog",Json::object(),tokens[1])["result"];
        check(current["version"]==1&&current["manifest"]["items"].size()==7,"catalog manifest served");
        std::string bodyHash;for(const auto& asset:current["manifest"]["assets"])if(asset["name"]=="body.male.glb")bodyHash=asset["sha256"];
        const auto titleTwo=call(service,"auth.login",{{"username","alice"},{"password","alice-password"}},{},"two")["result"]["token"].get<std::string>();
        auto part=call(service,"assets.read",{{"hash",bodyHash},{"offset",0},{"length",12288}},titleTwo,"two");
        check(part["error"]=="OK"&&part["result"]["mime"]=="model/gltf-binary"&&part["result"]["hex"].get<std::string>().substr(0,8)=="676c5446",
              "catalog files are readable from every title");

        // Descriptions.
        check(call(service,"avatars.set",{{"description",hex(description(1,outfit))}},tokens[0])["result"]["revision"]==1,"first avatar");
        check(call(service,"avatars.set",{{"description",hex(description(1,outfit))}},tokens[0])["error"]=="RATE_LIMITED","update spacing");
        {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");}
        check(call(service,"avatars.set",{{"description",hex(description(1,{2,20,40,60,80,100},0,1600))}},tokens[0])["result"]["revision"]==2,"revision advances");
        auto read=call(service,"avatars.get",{{"userIds",Json::array({users[0],users[1],std::string(32,'0')})}},tokens[1])["result"]["avatars"];
        check(read.size()==3&&read[0]["description"]==hex(description(1,{2,20,40,60,80,100},0,1600))&&read[0]["revision"]==2,"another account reads it");
        check(read[1]["description"].is_null()&&read[2]["description"].is_null(),"no avatar reads as null");
        check(call(service,"avatars.get",{{"userIds",Json::array()}},tokens[1])["error"]=="INVALID_ARGUMENT","empty request");
        check(call(service,"avatars.get",{{"userIds",Json(std::vector<std::string>(17,users[0]))}},tokens[1])["error"]=="INVALID_ARGUMENT","batch bound");
        check(call(service,"avatars.get",{{"userIds",Json::array({"NOT-HEX"})}},tokens[1])["error"]=="INVALID_ARGUMENT","identifier format");
        auto refuse=[&](std::string bytes,const char* reason){
            {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");}
            check(call(service,"avatars.set",{{"description",hex(bytes)}},tokens[1])["error"]=="INVALID_ARGUMENT",reason);
        };
        auto damaged=description(1,outfit);damaged[40]^=1;refuse(damaged,"checksum");
        refuse(description(1,{1,20,40,999,0,0}),"unknown item");
        refuse(description(1,{20,1,40,60,0,0}),"item in the wrong slot");
        refuse(description(1,{1,20,0,60,0,0}),"required slot");
        refuse(description(2,outfit),"catalog not imported");
        refuse(description(1,outfit,1,2100),"height range");
        auto reserved=description(1,outfit);reserved[500]=1;refuse(refreshCrc(reserved),"reserved bytes");
        auto format=description(1,outfit);format[0]=3;refuse(refreshCrc(format),"format version");
        auto neutral=description(1,outfit);neutral[0]=2;std::fill(neutral.begin()+45,neutral.begin()+61,static_cast<char>(128));
        refuse(refreshCrc(neutral),"format 2 must use facial hair or a face shape");
        refuse(description(1,outfit).substr(0,1000),"length");
        check(call(service,"avatars.set",{{"description","zz"+hex(description(1,outfit)).substr(2)}},tokens[1])["error"]=="INVALID_ARGUMENT","hex");

        // A newer catalog adds items; older descriptions stay valid.
        {Store store(path.string());
         auto v2=catalog(2,{{1,"hair"},{2,"hair"},{20,"top"},{40,"bottom"},{60,"shoes"},{80,"glasses"},{100,"hat"},{102,"hat"}});
         check(importAvatarCatalog(store,v2.manifest,v2.files)==2,"catalog v2");
         auto older=catalog(1,v1);bool refused=false;
         try{(void)importAvatarCatalog(store,older.manifest,older.files);}catch(...){refused=true;}
         check(!refused,"re-importing v1 stays a no-op");
         store.exec("UPDATE avatars SET updated=updated-5");}
        check(call(service,"avatars.set",{{"description",hex(description(2,{1,20,40,60,0,102}))}},tokens[1])["result"]["revision"]==1,"new item");
        check(call(service,"avatars.catalog",Json::object(),tokens[1])["result"]["version"]==2,"newest catalog by default");
        check(call(service,"avatars.catalog",{{"version",1}},tokens[1])["result"]["version"]==1,"a specific version");
        check(call(service,"avatars.catalog",{{"version",7}},tokens[1])["error"]=="NOT_FOUND","unknown version");
        {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");}
        check(call(service,"avatars.set",{{"description",hex(description(1,outfit))}},tokens[1])["error"]=="OK","v1 descriptions stay valid");
        // Format 2: facial hair must be a feature item of the named catalog; clients that do not
        // list format 2 get the format 1 copy.
        {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");
         auto v3=catalog(3,{{1,"hair"},{2,"hair"},{20,"top"},{40,"bottom"},{60,"shoes"},{80,"glasses"},{100,"hat"},{102,"hat"},
                            {120,"facialHair"}});
         check(importAvatarCatalog(store,v3.manifest,v3.files)==3,"catalog v3 with a feature item");}
        auto shaped=description(3,outfit);shaped[0]=2;shaped[43]=120;shaped[50]=static_cast<char>(200);shaped=refreshCrc(shaped);
        check(call(service,"avatars.set",{{"description",hex(shaped)}},tokens[1])["error"]=="OK","format 2 stored");
        auto wrongFeature=shaped;wrongFeature[43]=100;
        {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");}
        refuse(refreshCrc(wrongFeature),"facial hair must be a feature item");
        const auto latest=call(service,"avatars.get",{{"userIds",Json::array({users[1]})},{"formats",{1,2}}},tokens[0])["result"]["avatars"][0];
        check(latest["description"]==hex(shaped),"a format 2 reader gets format 2");
        const auto older=call(service,"avatars.get",{{"userIds",Json::array({users[1]})}},tokens[0])["result"]["avatars"][0];
        auto downgraded=shaped;downgraded[0]=1;std::fill(downgraded.begin()+43,downgraded.begin()+61,'\0');
        check(older["description"]==hex(refreshCrc(downgraded)),"an older reader gets the format 1 copy");
        {Store store(path.string());validateAvatarDescription(store.db(),refreshCrc(downgraded));}
        // Versions are exact: a v1 description is checked against v1 alone, and v1 is served as imported.
        {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");}
        check(call(service,"avatars.set",{{"description",hex(description(1,{1,20,40,60,0,102}))}},tokens[1])["error"]=="INVALID_ARGUMENT",
              "a v1 description cannot name an item only v2 has");
        check(call(service,"avatars.catalog",{{"version",1}},tokens[1])["result"]["manifest"]==parse(catalog(1,v1).manifest),
              "v1 is served exactly as imported after v2");

        // Administration.
        {Store store(path.string());
         for(int round=0;round<20;++round) {
             const auto random=randomAvatarDescription(store.db(),round%2);
             validateAvatarDescription(store.db(),random);
             check(static_cast<unsigned char>(random[4])==round%2,"random honours the body type");
         }
         setAvatar(store,"bob",randomAvatarDescription(store.db(),std::nullopt));
         setAvatar(store,"alice",std::nullopt);
         bool refused=false;try{setAvatar(store,"nobody",std::nullopt);}catch(const Error& e){refused=e.code()=="NOT_FOUND";}
         check(refused,"unknown account");}
        read=call(service,"avatars.get",{{"userIds",Json::array({users[0],users[1]})}},tokens[1])["result"]["avatars"];
        check(read[0]["description"].is_null()&&!read[1]["description"].is_null(),"administration set and clear");
        clean();
        std::cout<<"avatars "<<checks<<" assertions passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<"avatar test failed: "<<error.what()<<"\n";clean();return 1;}
}
