// SPDX-License-Identifier: MIT
#include "AvatarFixtures.hpp"
#include "CnaService/Avatars.hpp"
#include "CnaService/Service.hpp"
#include <array>
#include <filesystem>
#include <functional>
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
        check(call(service,"avatars.set",{{"description",hex(description(3,outfit))}},tokens[0])["error"]=="INVALID_ARGUMENT","unknown catalog");
        {Store store(path.string());bool refused=false;try{(void)randomAvatarDescription(store.db(),std::nullopt);}catch(const Error& e){refused=e.code()=="NOT_FOUND";}
         check(refused,"random needs a catalog");}

        // Import validation.
        // The baseline (synthetic, without feature items): every later catalog grows from it.
        const std::vector<std::pair<int,std::string>> v3{{1,"hair"},{2,"hair"},{20,"top"},{40,"bottom"},{60,"shoes"},{80,"glasses"},{100,"hat"}};
        {Store store(path.string());
         // Catalogs 1 and 2 were CNA development catalogs; 0 is no catalog at all.
         for(int legacy:{0,1,2}) {
             const auto development=catalog(legacy,v3);bool refused=false;
             try{importAvatarCatalog(store,development.manifest,development.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
             check(refused,"a development catalog is never imported");
         }
         {Statement none(store.db(),"SELECT COUNT(*) FROM avatar_catalogs");(void)none.row();check(none.number(0)==0,"nothing imported");}
         // A later catalog grows from the baseline, so it needs the baseline first.
         {const auto early=catalog(4,v3);bool refused=false;
          try{importAvatarCatalog(store,early.manifest,early.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
          check(refused,"a later catalog before the baseline is refused");}
         auto bad=catalog(3,v3);bad.files["body.male.glb"]=glb("z",0,1.1f);
         bool refused=false;try{importAvatarCatalog(store,bad.manifest,bad.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
         check(refused,"hash mismatch refused");
         auto missing=catalog(3,v3);missing.files.erase("animations.glb");
         refused=false;try{importAvatarCatalog(store,missing.manifest,missing.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
         check(refused,"missing file refused");
         auto notGlb=catalog(3,v3);notGlb.files["body.male.glb"]="plain text of the right size.......";
         auto m=parse(notGlb.manifest);for(auto& asset:m["assets"])if(asset["name"]=="body.male.glb"){asset["sha256"]=sha256(notGlb.files["body.male.glb"]);asset["size"]=notGlb.files["body.male.glb"].size();}
         refused=false;try{importAvatarCatalog(store,m.dump(),notGlb.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
         check(refused,"file type checked");
         auto traversal=catalog(3,v3);auto t=parse(traversal.manifest);t["assets"][0]["name"]="../x.glb";
         refused=false;try{importAvatarCatalog(store,t.dump(),traversal.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
         check(refused,"asset names are plain file names");
         const auto good=catalog(3,v3);
         check(importAvatarCatalog(store,good.manifest,good.files)==3,"the baseline catalog v3 imported");
         check(importAvatarCatalog(store,good.manifest,good.files)==3,"identical re-import is a no-op");
         auto changed=catalog(3,v3,'q');
         refused=false;try{importAvatarCatalog(store,changed.manifest,changed.files);}catch(const Error& e){refused=e.code()=="CONFLICT";}
         check(refused,"a version cannot change");
         auto shrunk=catalog(4,{{1,"hair"},{20,"top"},{40,"bottom"},{60,"shoes"},{80,"glasses"},{100,"hat"}});
         refused=false;try{importAvatarCatalog(store,shrunk.manifest,shrunk.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
         check(refused,"items never disappear");
         auto moved=catalog(4,{{1,"top"},{2,"hair"},{20,"top"},{40,"bottom"},{60,"shoes"},{80,"glasses"},{100,"hat"}});
         refused=false;try{importAvatarCatalog(store,moved.manifest,moved.files);}catch(const Error& e){refused=e.code()=="INVALID_ARGUMENT";}
         check(refused,"items never change slot");}

        auto current=call(service,"avatars.catalog",Json::object(),tokens[1])["result"];
        check(current["version"]==3&&current["manifest"]["items"].size()==7,"catalog manifest served");
        std::string bodyHash;for(const auto& asset:current["manifest"]["assets"])if(asset["name"]=="body.male.glb")bodyHash=asset["sha256"];
        const auto titleTwo=call(service,"auth.login",{{"username","alice"},{"password","alice-password"}},{},"two")["result"]["token"].get<std::string>();
        auto part=call(service,"assets.read",{{"hash",bodyHash},{"offset",0},{"length",12288}},titleTwo,"two");
        check(part["error"]=="OK"&&part["result"]["mime"]=="model/gltf-binary"&&part["result"]["hex"].get<std::string>().substr(0,8)=="676c5446",
              "catalog files are readable from every title");

        // Descriptions.
        check(call(service,"avatars.set",{{"description",hex(description(3,outfit))}},tokens[0])["result"]["revision"]==1,"first avatar");
        check(call(service,"avatars.set",{{"description",hex(description(3,outfit))}},tokens[0])["error"]=="RATE_LIMITED","update spacing");
        {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");}
        check(call(service,"avatars.set",{{"description",hex(description(3,{2,20,40,60,80,100},0,1600))}},tokens[0])["result"]["revision"]==2,"revision advances");
        auto read=call(service,"avatars.get",{{"userIds",Json::array({users[0],users[1],std::string(32,'0')})}},tokens[1])["result"]["avatars"];
        check(read.size()==3&&read[0]["description"]==hex(description(3,{2,20,40,60,80,100},0,1600))&&read[0]["revision"]==2,"another account reads it");
        check(read[1]["description"].is_null()&&read[2]["description"].is_null(),"no avatar reads as null");
        check(call(service,"avatars.get",{{"userIds",Json::array()}},tokens[1])["error"]=="INVALID_ARGUMENT","empty request");
        check(call(service,"avatars.get",{{"userIds",Json(std::vector<std::string>(17,users[0]))}},tokens[1])["error"]=="INVALID_ARGUMENT","batch bound");
        check(call(service,"avatars.get",{{"userIds",Json::array({"NOT-HEX"})}},tokens[1])["error"]=="INVALID_ARGUMENT","identifier format");
        auto refuse=[&](std::string bytes,const char* reason){
            {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");}
            check(call(service,"avatars.set",{{"description",hex(bytes)}},tokens[1])["error"]=="INVALID_ARGUMENT",reason);
        };
        auto damaged=description(3,outfit);damaged[40]^=1;refuse(damaged,"checksum");
        refuse(description(3,{1,20,40,999,0,0}),"unknown item");
        refuse(description(3,{20,1,40,60,0,0}),"item in the wrong slot");
        refuse(description(3,{1,20,0,60,0,0}),"required slot");
        refuse(description(4,outfit),"catalog not imported");
        // Catalog 1 and 2 descriptions, format 1 or format 2, are never accepted: there is no such
        // catalog here and never will be, although catalog 3 has every item they name.
        for(int legacy:{0,1,2}) {
            refuse(description(legacy,outfit),"a description on a development catalog");
            auto face=description(legacy,outfit);face[0]=2;face[50]=static_cast<char>(200);std::fill(face.begin()+45,face.begin()+50,static_cast<char>(128));
            std::fill(face.begin()+51,face.begin()+61,static_cast<char>(128));
            refuse(refreshCrc(face),"a format 2 description on a development catalog");
        }
        refuse(description(3,outfit,1,2100),"height range");
        auto reserved=description(3,outfit);reserved[500]=1;refuse(refreshCrc(reserved),"reserved bytes");
        auto format=description(3,outfit);format[0]=3;refuse(refreshCrc(format),"format version");
        auto neutral=description(3,outfit);neutral[0]=2;std::fill(neutral.begin()+45,neutral.begin()+61,static_cast<char>(128));
        refuse(refreshCrc(neutral),"format 2 must use facial hair or a face shape");
        refuse(description(3,outfit).substr(0,1000),"length");
        check(call(service,"avatars.set",{{"description","zz"+hex(description(3,outfit)).substr(2)}},tokens[1])["error"]=="INVALID_ARGUMENT","hex");
        // A shaped face (format 2) is valid on the baseline even without feature items.
        {Store store(path.string());auto shapedBaseline=description(3,outfit);shapedBaseline[0]=2;
         std::fill(shapedBaseline.begin()+45,shapedBaseline.begin()+61,static_cast<char>(128));shapedBaseline[47]=static_cast<char>(30);
         validateAvatarDescription(store.db(),refreshCrc(shapedBaseline));}

        // A future catalog (v4) grows from the baseline; baseline descriptions stay valid.
        {Store store(path.string());
         auto v4=catalog(4,{{1,"hair"},{2,"hair"},{20,"top"},{40,"bottom"},{60,"shoes"},{80,"glasses"},{100,"hat"},{102,"hat"}});
         check(importAvatarCatalog(store,v4.manifest,v4.files)==4,"catalog v4");
         auto older=catalog(3,v3);bool refused=false;
         try{(void)importAvatarCatalog(store,older.manifest,older.files);}catch(...){refused=true;}
         check(!refused,"re-importing v3 stays a no-op");
         store.exec("UPDATE avatars SET updated=updated-5");}
        check(call(service,"avatars.set",{{"description",hex(description(4,{1,20,40,60,0,102}))}},tokens[1])["result"]["revision"]==1,"new item");
        check(call(service,"avatars.catalog",Json::object(),tokens[1])["result"]["version"]==4,"newest catalog by default");
        check(call(service,"avatars.catalog",{{"version",0}},tokens[1])["result"]["version"]==4,"version 0 is the newest");
        check(call(service,"avatars.catalog",{{"version",3}},tokens[1])["result"]["version"]==3,"a specific version");
        check(call(service,"avatars.catalog",{{"version",7}},tokens[1])["error"]=="NOT_FOUND","unknown version");
        for(int legacy:{1,2})
            check(call(service,"avatars.catalog",{{"version",legacy}},tokens[1])["error"]=="INVALID_ARGUMENT","no development catalog manifest");
        {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");}
        check(call(service,"avatars.set",{{"description",hex(description(3,outfit))}},tokens[1])["error"]=="OK","v3 descriptions stay valid");
        // Format 2: facial hair must be a feature item of the named catalog; clients that do not
        // list format 2 get the format 1 copy.
        {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");
         auto v5=catalog(5,{{1,"hair"},{2,"hair"},{20,"top"},{40,"bottom"},{60,"shoes"},{80,"glasses"},{100,"hat"},{102,"hat"},
                            {120,"facialHair"}});
         check(importAvatarCatalog(store,v5.manifest,v5.files)==5,"catalog v5 with a feature item");}
        auto shaped=description(5,outfit);shaped[0]=2;shaped[43]=120;shaped[50]=static_cast<char>(200);shaped=refreshCrc(shaped);
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
        // Versions are exact: a v3 description is checked against v3 alone, and v3 is served as imported.
        {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");}
        check(call(service,"avatars.set",{{"description",hex(description(3,{1,20,40,60,0,102}))}},tokens[1])["error"]=="INVALID_ARGUMENT",
              "a v3 description cannot name an item only v4 has");
        check(call(service,"avatars.catalog",{{"version",3}},tokens[1])["result"]["manifest"]==parse(catalog(3,v3).manifest),
              "v3 is served exactly as imported after v4");

        // Catalog packs: one catalog version as an installable unit, its manifest and files by content.
        const auto hello=call(service,"hello",Json::object(),{})["result"]["capabilities"].dump();
        check(hello.find("\"avatar-catalog-packs\"")!=std::string::npos&&hello.find("\"files\"")!=std::string::npos,"pack capabilities");
        const auto pack=call(service,"avatars.catalogPack",{{"version",5}},tokens[0])["result"];

        std::string v5Manifest;long long v5Total=0;
        {Store store(path.string());Statement row(store.db(),"SELECT manifest FROM avatar_catalogs WHERE version=5");(void)row.row();v5Manifest=row.text(0);}
        const auto v5Json=parse(v5Manifest);
        for(const auto& asset:v5Json["assets"])v5Total+=asset["size"].get<long long>();
        check(pack["version"]==5&&pack["packFormat"]==1&&pack["reader"]==1&&pack["descriptionFormats"]==Json::array({1,2})&&
              pack["manifestSha256"]==sha256(v5Manifest)&&pack["manifestSize"]==v5Manifest.size()&&pack["totalBytes"]==v5Total,"pack descriptor");
        check(call(service,"avatars.catalogPack",{{"version",4}},tokens[0])["result"]["descriptionFormats"]==Json::array({1}),
              "a catalog without face controls or feature items is named by format 1 only");
        check(call(service,"avatars.catalogPack",{{"version",3}},tokens[0])["result"]["version"]==3,"the baseline is a pack too");
        check(call(service,"avatars.catalogPack",{{"version",7}},tokens[0])["error"]=="NOT_FOUND","unknown pack");
        for(int legacy:{0,1,2})
            check(call(service,"avatars.catalogPack",{{"version",legacy}},tokens[0])["error"]=="INVALID_ARGUMENT","no development catalog pack");
        auto manifestFile=service.file("one",tokens[0],sha256(v5Manifest));
        check(manifestFile.code=="OK"&&manifestFile.mime=="application/json"&&manifestFile.bytes==v5Manifest,"the manifest is a file");
        std::string itemHash;for(const auto& asset:v5Json["assets"])if(asset["name"]=="item120.male.glb")itemHash=asset["sha256"];
        const auto itemFile=service.file("one",tokens[0],itemHash);
        check(itemFile.code=="OK"&&itemFile.mime=="model/gltf-binary"&&sha256(itemFile.bytes)==itemHash,"catalog files by content");
        check(service.file("two",tokens[0],itemHash).code=="UNAUTHENTICATED","tokens are title-scoped");
        check(service.file("one",std::string(64,'0'),itemHash).code=="UNAUTHENTICATED","signed-in only");
        check(service.file("one",tokens[0],std::string(64,'e')).code=="NOT_FOUND","only published files");
        check(service.file("one",tokens[0],"not-a-hash").code=="INVALID_ARGUMENT","hash format");

        // Negotiation: Bob's stored avatar is format 2 on catalog 5 with facial hair 120.
        auto readAs=[&](const Json& extra){Json args{{"userIds",Json::array({users[1]})},{"formats",{1,2}}};args.update(extra);
            return call(service,"avatars.get",args,tokens[0])["result"]["avatars"][0];};
        auto has=readAs({{"catalogs",{3,4,5}},{"catalogUpdates",false},{"maxCatalogBytes",0},{"reader",1}});
        check(has["description"]==hex(shaped)&&!has.contains("projected"),"a client with the catalog gets the stored avatar");
        auto installs=readAs({{"catalogs",{3,4}},{"catalogUpdates",true},{"maxCatalogBytes",1<<30},{"reader",1}});
        check(installs["description"]==hex(shaped)&&!installs.contains("projected"),"a client that will install the catalog gets the stored avatar");
        auto declines=readAs({{"catalogs",{3,4}},{"catalogUpdates",false},{"maxCatalogBytes",1<<30},{"reader",1}});
        auto onFour=description(4,outfit);
        check(declines["projected"]==true&&declines["catalogVersion"]==5&&declines["revision"]==has["revision"]&&
              declines["description"]==hex(onFour),"a client without the catalog gets a projection onto the newest it has");
        {Store store(path.string());validateAvatarDescription(store.db(),onFour);}
        auto small=readAs({{"catalogs",{3,4}},{"catalogUpdates",true},{"maxCatalogBytes",16},{"reader",1}});
        check(small["projected"]==true,"a pack larger than the client accepts is projected");
        check(readAs({{"catalogs",{3,4,5}},{"catalogUpdates",false},{"maxCatalogBytes",0},{"reader",1}})["description"]==hex(shaped),
              "a projection never changes the stored avatar");
        // Development catalogs a client lists name nothing here and are ignored; a client must list
        // some supported catalog.
        auto onBaseline=description(3,outfit);
        auto devAndBaseline=readAs({{"catalogs",{1,2,3}},{"catalogUpdates",false},{"maxCatalogBytes",0},{"reader",1}});
        check(devAndBaseline["projected"]==true&&devAndBaseline["description"]==hex(onBaseline),"a listed development catalog is never a projection target");
        check(readAs({{"catalogs",{1,2}},{"catalogUpdates",false},{"maxCatalogBytes",0},{"reader",1}}).is_null(),
              "a client listing only development catalogs is refused");
        check(readAs(Json::object())["description"]==hex(shaped),"an older client (no catalog list) reads contract level 1 catalogs as before");
        check(readAs({{"catalogs",Json::array()},{"catalogUpdates",false},{"maxCatalogBytes",0},{"reader",1}}).is_null(),"a client has some catalog");
        // Items the target lacks become their slot's default; optional slots are emptied.
        {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");
         auto v6=catalog(6,{{1,"hair"},{2,"hair"},{3,"hair"},{20,"top"},{40,"bottom"},{60,"shoes"},{80,"glasses"},{100,"hat"},{102,"hat"},
                            {120,"facialHair"}});
         check(importAvatarCatalog(store,v6.manifest,v6.files)==6,"catalog v6");}
        check(call(service,"avatars.set",{{"description",hex(description(6,{3,20,40,60,80,102}))}},tokens[0])["error"]=="OK","v6 avatar");
        const auto aliceOnBaseline=call(service,"avatars.get",{{"userIds",Json::array({users[0]})},{"formats",{1,2}},{"catalogs",{3}},
            {"catalogUpdates",false},{"maxCatalogBytes",0},{"reader",1}},tokens[1])["result"]["avatars"][0];
        check(aliceOnBaseline["description"]==hex(description(3,{1,20,40,60,80,0})),"hair 3 becomes hair 1, hat 102 is left out on catalog 3");
        {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");}

        // Administration.
        {Store store(path.string());
         for(int round=0;round<20;++round) {
             const auto random=randomAvatarDescription(store.db(),round%2);
             validateAvatarDescription(store.db(),random);
             check(static_cast<unsigned char>(random[4])==round%2,"random honours the body type");
         }
         check(static_cast<unsigned char>(randomAvatarDescription(store.db(),std::nullopt)[8])==6,"random avatars use the newest catalog");
         bool legacyRefused=false;try{setAvatar(store,"bob",description(2,outfit));}catch(const Error& e){legacyRefused=e.code()=="INVALID_ARGUMENT";}
         check(legacyRefused,"administration cannot set a development catalog avatar");
         setAvatar(store,"bob",randomAvatarDescription(store.db(),std::nullopt));
         setAvatar(store,"alice",std::nullopt);
         bool refused=false;try{setAvatar(store,"nobody",std::nullopt);}catch(const Error& e){refused=e.code()=="NOT_FOUND";}
         check(refused,"unknown account");}
        read=call(service,"avatars.get",{{"userIds",Json::array({users[0],users[1]})}},tokens[1])["result"]["avatars"];
        check(read[0]["description"].is_null()&&!read[1]["description"].is_null(),"administration set and clear");
        clean();

        // A baseline like CNA's own, with a feature item: format 1 and format 2 descriptions on
        // catalog 3, and catalog 3's version bytes kept as stored.
        {
            {Store store(path.string());store.title("one","One");store.user("erin","erin-password","Erin");
             const auto baseline=catalog(3,{{1,"hair"},{20,"top"},{40,"bottom"},{60,"shoes"},{100,"hat"},{120,"facialHair"}});
             check(importAvatarCatalog(store,baseline.manifest,baseline.files)==3,"a baseline with a feature item");}
            Service fresh(path.string());
            const auto erin=call(fresh,"auth.login",{{"username","erin"},{"password","erin-password"}},{})["result"]["token"].get<std::string>();
            const auto formatOne=description(3,{1,20,40,60,0,100});
            check(call(fresh,"avatars.set",{{"description",hex(formatOne)}},erin)["error"]=="OK","a format 1 description on catalog 3");
            auto formatTwo=description(3,{1,20,40,60,0,0});formatTwo[0]=2;formatTwo[43]=120;
            std::fill(formatTwo.begin()+45,formatTwo.begin()+61,static_cast<char>(128));formatTwo[49]=static_cast<char>(222);formatTwo=refreshCrc(formatTwo);
            {Store store(path.string());store.exec("UPDATE avatars SET updated=updated-5");}
            check(call(fresh,"avatars.set",{{"description",hex(formatTwo)}},erin)["error"]=="OK","a format 2 description with facial hair on catalog 3");
            {Store store(path.string());Statement row(store.db(),"SELECT description FROM avatars");check(row.row()&&row.blob(0)==formatTwo,"stored as sent");
             check(static_cast<unsigned char>(row.blob(0)[8])==3&&row.blob(0)[9]==0,"catalog version bytes 8-9 name catalog 3");}
            check(call(fresh,"avatars.catalogPack",{{"version",3}},erin)["result"]["descriptionFormats"]==Json::array({1,2}),
                  "the baseline with feature items is named by formats 1 and 2");
            clean();
        }

        // Schema 24: an upgraded development database keeps no catalog 1 or 2 state and keeps
        // everything else; fresh databases have the same guards.
        {
            const auto legacyPath=std::filesystem::current_path()/"avatar-migration.sqlite3";
            auto cleanLegacy=[&]{for(const auto* suffix:{"","-wal","-shm"})std::filesystem::remove(legacyPath.string()+suffix);};
            cleanLegacy();
            std::array<std::string,4> people;
            std::string sharedHash;
            const std::string legacyOnly="a file only catalog 1 listed";
            const auto legacyOnlyHash=sha256(legacyOnly);
            const auto carolAvatar=description(3,{1,20,40,60,0,100});
            {
                Store store(legacyPath.string());store.title("one","One");
                int index=0;for(const auto* name:{"ann","ben","cat","dan"})people[static_cast<std::size_t>(index++)]=store.user(name,std::string(name)+"-password",name);
                const auto baseline=catalog(3,{{1,"hair"},{20,"top"},{40,"bottom"},{60,"shoes"},{100,"hat"}});
                check(importAvatarCatalog(store,baseline.manifest,baseline.files)==3,"migration fixture baseline");
                // What a schema 23 development database could hold: catalogs 1 and 2, sharing a file
                // with catalog 3, and avatars on them.
                store.exec("DROP TRIGGER avatar_catalogs_baseline_insert; DROP TRIGGER avatar_catalogs_baseline_update; "
                           "DROP TRIGGER avatars_baseline_insert; DROP TRIGGER avatars_baseline_update");
                {Statement shared(store.db(),"SELECT hash FROM avatar_catalog_assets WHERE version=3 AND name='body.male.glb'");
                 check(shared.row(),"baseline body file");sharedHash=shared.text(0);}
                {Statement asset(store.db(),"INSERT INTO assets(hash,mime,size,bytes) VALUES(?,'model/gltf-binary',?,?)");
                 asset.bind(1,legacyOnlyHash);asset.bind(2,static_cast<long long>(legacyOnly.size()));asset.blob(3,legacyOnly);(void)asset.row();}
                store.exec("INSERT INTO avatar_catalogs(version,manifest,imported) VALUES(1,'{}',0),(2,'{}',0)");
                for(long long legacy:{1LL,2LL}) {
                    for(const auto& [id,slot]:std::array<std::pair<long long,long long>,4>{{{1,0},{20,1},{40,2},{60,3}}}) {
                        Statement item(store.db(),"INSERT INTO avatar_catalog_items(version,id,slot) VALUES(?,?,?)");
                        item.bind(1,legacy);item.bind(2,id);item.bind(3,slot);(void)item.row();
                    }
                    Statement link(store.db(),"INSERT INTO avatar_catalog_assets(version,name,hash) VALUES(?,'body.male.glb',?)");
                    link.bind(1,legacy);link.bind(2,sharedHash);(void)link.row();
                }
                {Statement link(store.db(),"INSERT INTO avatar_catalog_assets(version,name,hash) VALUES(1,'old.glb',?)");link.bind(1,legacyOnlyHash);(void)link.row();}
                auto put=[&](const std::string& user,const std::string& bytes) {
                    Statement row(store.db(),"INSERT INTO avatars(user_id,description,revision,updated) VALUES(?,?,3,0)");
                    row.bind(1,user);row.blob(2,bytes);(void)row.row();
                };
                put(people[0],description(1,{1,20,40,60,0,0}));
                auto bearded=description(2,{1,20,40,60,0,0});bearded[0]=2;bearded[43]=120;put(people[1],refreshCrc(bearded));
                put(people[2],carolAvatar);
                store.exec("PRAGMA user_version=23");
            }
            {
                Store upgraded(legacyPath.string());
                auto count=[&](const char* sql){Statement s(upgraded.db(),sql);(void)s.row();return s.number(0);};
                check(count("PRAGMA user_version")==SchemaVersion&&SchemaVersion==24,"schema 24 migration");
                check(count("SELECT COUNT(*) FROM avatar_catalogs WHERE version<3")==0&&count("SELECT COUNT(*) FROM avatar_catalogs WHERE version=3")==1,
                      "only the baseline catalog remains");
                check(count("SELECT COUNT(*) FROM avatar_catalog_items WHERE version<3")==0,"development catalog items removed");
                check(count("SELECT COUNT(*) FROM avatar_catalog_assets WHERE version<3")==0,"development catalog file links removed");
                check(count("SELECT COUNT(*) FROM avatar_catalog_items WHERE version=3")==5&&count("SELECT COUNT(*) FROM avatar_catalog_assets WHERE version=3")>0,
                      "the baseline's items and files are kept");
                {Statement left(upgraded.db(),"SELECT user_id FROM avatars");check(left.row()&&left.text(0)==people[2]&&!left.row(),"only the baseline avatar remains");}
                check(count("SELECT COUNT(*) FROM users")==4&&count("SELECT COUNT(*) FROM titles")==1,"accounts and titles are kept");
                {Statement kept(upgraded.db(),"SELECT COUNT(*) FROM assets WHERE hash IN (?,?)");kept.bind(1,sharedHash);kept.bind(2,legacyOnlyHash);
                 (void)kept.row();check(kept.number(0)==2,"content-addressed files are not deleted");}
                // From now on the tables refuse development catalog state.
                auto refusedSql=[&](const char* sql,const std::function<void(Statement&)>& bind) {
                    Statement s(upgraded.db(),sql);bind(s);
                    try{(void)s.row();}catch(const Error& e){return e.code()=="CONFLICT";}
                    return false;
                };
                check(refusedSql("INSERT INTO avatar_catalogs(version,manifest,imported) VALUES(2,'{}',0)",[](Statement&){}),"no catalog 2 row");
                // A row without items, so that only the guard can refuse the renumbering.
                upgraded.exec("INSERT INTO avatar_catalogs(version,manifest,imported) VALUES(9,'{}',0)");
                check(refusedSql("UPDATE avatar_catalogs SET version=1 WHERE version=9",[](Statement&){}),"no catalog renumbered below the baseline");
                upgraded.exec("DELETE FROM avatar_catalogs WHERE version=9");
                check(refusedSql("INSERT INTO avatars(user_id,description,revision,updated) VALUES(?,?,1,0)",[&](Statement& s){
                    s.bind(1,people[3]);s.blob(2,description(1,{1,20,40,60,0,0}));}),"no avatar on catalog 1");
                check(refusedSql("UPDATE avatars SET description=? WHERE user_id=?",[&](Statement& s){
                    s.blob(1,description(2,{1,20,40,60,0,0}));s.bind(2,people[2]);}),"no avatar moved to catalog 2");
            }
            {
                Service migrated(legacyPath.string());
                const auto token=call(migrated,"auth.login",{{"username","dan"},{"password","dan-password"}},{})["result"]["token"].get<std::string>();
                const auto avatars=call(migrated,"avatars.get",{{"userIds",Json::array({people[0],people[1],people[2]})}},token)["result"]["avatars"];
                check(avatars[0]["description"].is_null()&&avatars[0]["revision"]==0&&avatars[1]["description"].is_null()&&avatars[1]["revision"]==0,
                      "a removed development avatar reads as no avatar");
                check(avatars[2]["description"]==hex(carolAvatar)&&avatars[2]["revision"]==3,"a baseline avatar is served unchanged");
                check(migrated.file("one",token,legacyOnlyHash).code=="NOT_FOUND","a file only a development catalog listed is no longer served");
                check(migrated.file("one",token,sharedHash).code=="OK","a file the baseline lists still is");
                check(call(migrated,"avatars.catalogPack",{{"version",1}},token)["error"]=="INVALID_ARGUMENT","no catalog 1 pack after the upgrade");
            }
            cleanLegacy();
            {
                Store fresh(legacyPath.string());
                Statement guards(fresh.db(),"SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND name LIKE '%baseline%'");(void)guards.row();
                check(guards.number(0)==4,"a fresh database has the same guards");
            }
            cleanLegacy();
        }
        std::cout<<"avatars "<<checks<<" assertions passed\n";return 0;
    }catch(const std::exception& error){std::cerr<<"avatar test failed: "<<error.what()<<"\n";clean();return 1;}
}
