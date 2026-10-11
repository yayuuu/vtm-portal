// SPDX-License-Identifier: MIT
#include "common.hpp"
#include <iostream>
#include <regex>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
using namespace vtmportal;
static void check(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
template<class F> void rejects(F f) { bool bad=false; try {f();} catch(const std::exception&) {bad=true;} check(bad,"Invalid input accepted"); }
static std::vector<std::string> strings(GVariant* v,const char* key) {
    auto a=option(v,key,"as"); check(bool(a),"Missing string array");
    std::vector<std::string> out; GVariantIter iter; const char* s;
    g_variant_iter_init(&iter,a.get()); while(g_variant_iter_next(&iter,"&s",&s)) out.emplace_back(s);
    return out;
}
int main(int argc,char** argv) {
  try {
    if(argc>1 && std::string(argv[1])=="monitor") {
      std::string input; char data[4096]; ssize_t n;
      while((n=read(STDIN_FILENO,data,sizeof(data)))>0) input.append(data,n);
      write_file(argv[2],input); return 0;
    }
    if(argc!=2) throw std::runtime_error("Expected native helper path");
    GError* error=nullptr; gchar* folder=g_dir_make_tmp("vtm-native-helpers-XXXXXX",&error);
    check(folder!=nullptr,"Cannot create temporary directory"); fs::path root(folder); g_free(folder);
    struct Cleanup { fs::path p; ~Cleanup(){std::error_code e; fs::remove_all(p,e);} } cleanup{root};
    g_setenv("XDG_CONFIG_HOME",(root/"config").c_str(),TRUE);
    g_setenv("VTM_PORTAL_SYSTEM_CONFIG",(root/"missing").c_str(),TRUE);
    g_setenv("VTM_PORTAL_CONFIG","",TRUE); g_setenv("VTM_DESKTOP_PIN","",TRUE);
    auto config=configuration(); check(config.browser_command==std::vector<std::string>{"carbonyl"},"Bad default browser");
    auto path=root/"config/xdg-desktop-portal-vtm/config.toml"; fs::create_directories(path.parent_path());
    write_file(path,"[browser]\ncommand=['w3m', '-B']\n[vtm]\nsession='testing'\n");
    config=configuration(); check(config.session=="testing" && config.browser_command.back()=="-B" && config.picker_command.front()=="elio","Partial config failed");
    g_setenv("VTM_DESKTOP_PIN","environment-session",TRUE); check(configuration().session=="environment-session","Environment session precedence failed");
    g_setenv("VTM_DESKTOP_PIN","",TRUE);
    for(const auto& bad:{"[browser]\ncommand='wrong'", "[vtm]\nlaunch_timeout=true", "[picker]\nsize='0:3'", "[unknown]\na=1"}) {
      write_file(path,bad); rejects([]{configuration();});
    }
    write_file(path,"[browser]\ncommand=['carbonyl']\n"); config=configuration();
    std::string uri="https://example.org/?q=a%20b&x=\"quote\"&p=$(touch%20/tmp/no)&a='ą'";
    check(normalize_uri(uri,config)==uri,"URI changed");
    for(const auto& bad:{"javascript:alert(1)","https://","https://test/\n","file://remote/a"}) rejects([&]{normalize_uri(bad,config);});
    check(normalize_uri((root/"a b.html").string(),config).find("a%20b.html")!=std::string::npos,"File URI encoding failed");
    std::vector<std::string> args{fs::absolute(argv[0]).string(),"a file; $(no) 'ą'",uri,""};
    check(decode_argv(encode_argv(args))==args,"Argument transport changed argv");
    for(const auto& bad:{"", "!!!!", "AA==", "AAAA====", "AA=A"}) rejects([&]{decode_argv(bad);});
    rejects([]{encode_argv({"/bin/true",std::string("x\0y",3)});});
    check(lua_string("ą")=="\"\\196\\133\"","UTF-8 Lua escaping failed");
    config.vtm_command={fs::absolute(argv[0]).string(),"monitor",(root/"capture").string()};
    g_setenv("VTM_PORTAL_HELPER",fs::absolute(argv[1]).c_str(),TRUE);
    launch(args,"testing","A title\" ); evil()","100:30",config);
    auto script=read_file(root/"capture"); check(script.find("evil()")==std::string::npos,"Lua injection");
    std::smatch match; check(std::regex_search(script,match,std::regex("cmd=\"((?:\\\\[0-9]{3})*)\"")),"Missing Lua command");
    auto encoded=match[1].str(); std::string command;
    for(std::size_t i=0;i<encoded.size();i+=4) command+=static_cast<char>(std::stoi(encoded.substr(i+1,3)));
    check(decode_argv(command.substr(command.find(" exec ")+6))==args,"Monitor transport changed argv");
    auto a=root/"ą file.txt", b=root/"two.txt"; write_file(a,"one");write_file(b,"two");
    auto options=parse_variant("{'multiple': <true>}","a{sv}"); auto empty=parse_variant("{}","a{sv}");
    auto results=translate({a,b},options.get(),"open","",empty.get()); check(strings(results.get(),"uris").size()==2,"Multiple selection lost");
    results=translate({a,b},empty.get(),"open","",empty.get()); check(strings(results.get(),"uris").size()==1,"Single selection failed");
    auto filter=parse_variant("('Text', [(uint32 1, 'text/plain')])","(sa(us))"); check(filter_matches(a,filter.get()),"MIME filter failed");
    auto extra=parse_variant("{'current_filter': <('Images', [(uint32 0, '*.png')])>}","a{sv}");
    rejects([&]{translate({a},empty.get(),"open","",extra.get());});
    auto dirs=parse_variant("{'directory': <true>}","a{sv}"); results=translate({root},dirs.get(),"open","",empty.get());
    check(strings(results.get(),"uris").size()==1,"Directory selection failed");
    rejects([&]{translate({root},empty.get(),"open","",empty.get());});
    write_file(root/"foo.txt","old");
    auto files=parse_variant("{'files': <[b'foo.txt', b'foo.txt', b'new.txt']>}","a{sv}");
    results=translate({root},files.get(),"savefiles","",empty.get()); auto names=strings(results.get(),"uris");
    check(names.size()==3 && names[0].find("foo%20(1).txt")!=std::string::npos && names[1].find("foo%20(2).txt")!=std::string::npos,"SaveFiles collision handling failed");
    check(!fs::exists(root/"new.txt"),"Picker created a target file");
    results=translate({root},empty.get(),"save","name space.txt",empty.get()); check(strings(results.get(),"uris")[0].find("name%20space.txt")!=std::string::npos,"SaveFile failed");
    for(const auto& bad:{"", "..", "../escape", "name\n", "/absolute"}) rejects([&]{valid_name(bad);});
    std::cout<<"PASS: native TOML, URI/argv transport, monitor, file selection, filters and saves\n"; return 0;
  } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
