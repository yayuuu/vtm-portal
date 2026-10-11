// SPDX-License-Identifier: MIT
#include "common.hpp"
#include <future>
#include <iostream>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>
using namespace vtmportal;
static constexpr auto name="org.freedesktop.impl.portal.desktop.vtm";
static constexpr auto rootpath="/org/freedesktop/portal/desktop";
static constexpr auto iface="org.freedesktop.impl.portal.FileChooser";
static void check(bool value,const char* why) {if(!value) throw std::runtime_error(why);}
template<class F> void wait(F f) {auto end=g_get_monotonic_time()+8*G_USEC_PER_SEC; while(!f()) {if(g_get_monotonic_time()>end) throw std::runtime_error("Test timed out");g_usleep(30000);}}
static GDBusConnection* connection() {
    GError* e=nullptr;auto c=g_dbus_connection_new_for_address_sync(g_getenv("DBUS_SESSION_BUS_ADDRESS"),
      static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT|G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),nullptr,nullptr,&e);
    if(!c){std::string s=e->message;g_error_free(e);throw std::runtime_error(s);}return c;
}
static Variant call(GDBusConnection* c,const std::string& path,const char* interface,const char* method,GVariant* params=nullptr) {
    GError* e=nullptr;auto result=g_dbus_connection_call_sync(c,name,path.c_str(),interface,method,params,nullptr,G_DBUS_CALL_FLAGS_NO_AUTO_START,10000,nullptr,&e);
    if(!result){std::string s=e->message;g_error_free(e);throw std::runtime_error(s);}return own(result);
}
static unsigned code(GVariant* packet) {guint32 n;g_variant_get_child(packet,0,"u",&n);return n;}
static int mock(const fs::path& job) {
    auto req=read_request(job); if(req.title=="fail") return 1;
    if(req.title=="delay") g_usleep(500000);
    write_file(job/"started","test-worker");
    if(req.title=="wait"||req.title=="delay") {
      for(int i=0;i<200 && !fs::exists(job/"cancel");++i) g_usleep(25000);
      write_response(job,1,nullptr);
    } else if(req.title=="malformed") write_file(job/"result.variant","not a variant");
    else {
      g_usleep(100000);
      auto result=parse_variant(req.title=="bad"?"{'uris': <['https://not-a-file.test/']>}":
       "{'uris': <['file:///tmp/selected%20file.txt']>, 'current_filter': <('Text', [(uint32 0, '*.txt')])>, 'choices': <[('encoding', 'utf8')]>}","a{sv}");
      write_response(job,0,result.get());
    }
    return 0;
}
int main(int argc,char** argv) {
  try {
    if(argc==3 && std::string(argv[1])=="launch-picker") return mock(argv[2]);
    check(argc==2,"Expected backend binary");
    GError* e=nullptr;char* folder=g_dir_make_tmp("vtm-native-protocol-XXXXXX",&e); check(folder,"Cannot create test directory");
    fs::path tmp(folder);g_free(folder);fs::create_directory(tmp/"runtime");fs::permissions(tmp/"runtime",fs::perms::owner_all);
    g_setenv("XDG_RUNTIME_DIR",(tmp/"runtime").c_str(),TRUE);g_setenv("VTM_PORTAL_HELPER",fs::absolute(argv[0]).c_str(),TRUE);
    GPid backend;char* args[]={argv[1],nullptr};
    check(g_spawn_async(nullptr,args,nullptr,G_SPAWN_DO_NOT_REAP_CHILD,nullptr,nullptr,&backend,&e),"Cannot start backend");
    struct Cleanup { GPid pid;fs::path tmp; ~Cleanup(){kill(pid,SIGTERM);waitpid(pid,nullptr,0);std::error_code e;fs::remove_all(tmp,e);} } cleanup{backend,tmp};
    auto client=connection(),other=connection();
    struct Connections{GDBusConnection* a,*b;~Connections(){g_object_unref(a);g_object_unref(b);}} connections{client,other};
    wait([&]{try{auto v=call(client,rootpath,"org.freedesktop.DBus.Properties","Get",g_variant_new("(ss)",iface,"version"));auto box=own(g_variant_get_child_value(v.get(),0));auto version=own(g_variant_get_variant(box.get()));return g_variant_get_uint32(version.get())==4;}catch(const std::exception&){return false;}});
    auto choose=[&](const std::string& token,const std::string& title="ok",const char* method="OpenFile",GDBusConnection* c=nullptr) {
      if(!c)c=client;
      std::string path=std::string(rootpath)+"/request/test/"+token;
      auto options=parse_variant("{'current_folder': <b'/tmp'>, 'files': <[b'foo.txt']>}","a{sv}");
      auto params=own(g_variant_new("(osss@a{sv})",path.c_str(),"test.application","",title.c_str(),g_variant_ref(options.get())));
      return std::make_pair(path,std::async(std::launch::async,[c,method,p=std::move(params)]{return call(c,rootpath,iface,method,p.get());}));
    };
    auto job=[&](const std::string& title) {
      for(const auto& a:fs::directory_iterator(tmp/"runtime")) if(a.is_directory())
        for(const auto& b:fs::directory_iterator(a.path())) if(fs::exists(b.path()/"request.variant")) {
          try {if(read_request(b.path()).title==title)return b.path();}catch(const std::exception&){}
        }
      return fs::path{};
    };
    for(auto method:{"OpenFile","SaveFile","SaveFiles"}) {
      auto p=choose(method,"ok",method);auto response=p.second.get();check(code(response.get())==0,"FileChooser failed");
      auto dict=own(g_variant_get_child_value(response.get(),1));auto uris=option(dict.get(),"uris","as");
      const char* uri;g_variant_get_child(uris.get(),0,"&s",&uri);check(std::string(uri)=="file:///tmp/selected%20file.txt","URI changed");
      check(bool(option(dict.get(),"choices","a(ss)")),"Missing choices");
    }
    std::cout<<"PASS: FileChooser methods, version and typed responses\n";
    auto p=choose("cancel","wait");fs::path item;wait([&]{item=job("wait");return !item.empty()&&fs::exists(item/"started");});
    bool denied=false;try {call(other,p.first,"org.freedesktop.impl.portal.Request","Close");}catch(const std::exception& ex){denied=std::string(ex.what()).find("AccessDenied")!=std::string::npos;}check(denied,"Foreign cancellation accepted");
    auto duplicate=choose("cancel","wait");denied=false;try{duplicate.second.get();}catch(const std::exception& ex){denied=std::string(ex.what()).find("InvalidArgs")!=std::string::npos;}check(denied,"Duplicate request accepted");
    call(client,p.first,"org.freedesktop.impl.portal.Request","Close");check(code(p.second.get().get())==1,"Close did not cancel");wait([&]{return !fs::exists(item);});
    p=choose("late","delay");wait([&]{item=job("delay");return !item.empty();});call(client,p.first,"org.freedesktop.impl.portal.Request","Close");check(code(p.second.get().get())==1,"Early cancellation failed");wait([&]{return !fs::exists(item);});
    std::cout<<"PASS: owner-only Close, duplicates, cancellation and startup race\n";
    std::vector<std::future<Variant>> futures;
    for(int i=0;i<6;++i) futures.push_back(choose("parallel"+std::to_string(i)).second);
    for(auto& f:futures) check(code(f.get().get())==0,"Concurrent request failed");
    for(auto title:{"bad","malformed","fail"})check(code(choose(title,title).second.get().get())==2,"Invalid response accepted");
    auto disposable=connection();p=choose("gone","wait","OpenFile",disposable);wait([&]{item=job("wait");return !item.empty()&&fs::exists(item/"started");});
    g_dbus_connection_close_sync(disposable,nullptr,nullptr);try{p.second.get();}catch(const std::exception&){}g_object_unref(disposable);wait([&]{return !fs::exists(item);});
    std::cout<<"PASS: concurrency, invalid results, launch failure and disconnection\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}
}
