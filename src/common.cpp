// SPDX-License-Identifier: MIT
#include "common.hpp"
#include "paths.h"
#include "toml.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <regex>
#include <signal.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace vtmportal {
static std::runtime_error failure(const std::string& message) { return std::runtime_error(message); }
Variant own(GVariant* v) { return Variant(v ? g_variant_take_ref(v) : nullptr); }
std::string environment(const char* name) { const char* v=g_getenv(name); return v ? v : ""; }
static bool controls(const std::string& text) {
    return std::any_of(text.begin(),text.end(),[](unsigned char c){return c<32 || c==127;});
}
std::string read_file(const fs::path& path, std::size_t max_size) {
    auto size=fs::file_size(path);
    if (size>max_size) throw failure("File is too large: "+path.string());
    gchar* text=nullptr; gsize length=0; GError* error=nullptr;
    if (!g_file_get_contents(path.c_str(),&text,&length,&error)) {
        std::string reason=error->message; g_error_free(error); throw failure(reason);
    }
    std::string result(text,length); g_free(text);
    if (length>max_size) throw failure("File is too large: "+path.string());
    return result;
}
void write_file(const fs::path& path,const std::string& value) {
    GError* error=nullptr;
    if (!g_file_set_contents(path.c_str(),value.data(),static_cast<gssize>(value.size()),&error)) {
        std::string reason=error->message; g_error_free(error); throw failure(reason);
    }
}
static std::vector<std::string> string_array(const toml::node& value,const std::string& key) {
    auto* array=value.as_array();
    if (!array || array->empty()) throw failure(key+" must be a nonempty array of strings");
    std::vector<std::string> result;
    for (const auto& node:*array) {
        auto text=node.value<std::string>();
        if (!text || text->find('\0')!=std::string::npos) throw failure("Invalid "+key+" argument");
        result.push_back(*text);
    }
    if (result.front().empty()) throw failure(key+" cannot start with an empty string");
    return result;
}
static std::string text_value(const toml::node& node,const std::string& key) {
    auto value=node.value<std::string>();
    if (!value || value->find('\0')!=std::string::npos) throw failure(key+" must be a string without NUL");
    return *value;
}
static double timeout_value(const toml::node& node,const std::string& key) {
    std::optional<double> number;
    if (auto v=node.value_exact<int64_t>()) number=static_cast<double>(*v);
    else number=node.value_exact<double>();
    if (!number || !std::isfinite(*number) || *number<1 || *number>300)
        throw failure(key+" must be between 1 and 300 seconds");
    return *number;
}
static void merge_config(Config& c,const fs::path& path,bool required=false) {
    if (!fs::exists(path)) {
        if (required) throw failure("Configuration does not exist: "+path.string());
        return;
    }
    auto parsed=toml::parse(read_file(path),path.string());
    for (const auto& [section_key,node]:parsed) {
        std::string section(section_key.str());
        auto* table=node.as_table();
        if (!table || (section!="vtm" && section!="browser" && section!="picker" && section!="terminal"))
            throw failure("Unknown configuration section: "+section);
        for (const auto& [key_name,value]:*table) {
            std::string key(key_name.str()), full=section+"."+key;
            if (key=="command" && section!="terminal") {
                auto args=string_array(value,full);
                if (section=="vtm") c.vtm_command=std::move(args);
                else if (section=="browser") c.browser_command=std::move(args);
                else c.picker_command=std::move(args);
            } else if (key=="size" && section!="vtm") {
                auto size=text_value(value,full);
                if (!std::regex_match(size,std::regex("[1-9][0-9]{0,3}:[1-9][0-9]{0,3}")))
                    throw failure(full+" must have the form WIDTH:HEIGHT");
                if (section=="browser") c.browser_size=size;
                else if (section=="picker") c.picker_size=size;
                else c.terminal_size=size;
            } else if (full=="vtm.session") c.session=text_value(value,full);
            else if (full=="vtm.launch_timeout") c.launch_timeout=timeout_value(value,full);
            else if (full=="picker.start_timeout") c.start_timeout=timeout_value(value,full);
            else if (full=="browser.title") c.browser_title=text_value(value,full);
            else if (full=="browser.schemes") {
                c.schemes=string_array(value,full);
                for (const auto& scheme:c.schemes)
                    if (!std::regex_match(scheme,std::regex("[a-z][a-z0-9+.-]*"))) throw failure("Invalid URI scheme: "+scheme);
            } else throw failure("Unknown configuration option: "+full);
        }
    }
}
Config configuration() {
    Config c;
    auto system=environment("VTM_PORTAL_SYSTEM_CONFIG");
    merge_config(c,system.empty()?PORTAL_CONFIG:system);
    merge_config(c,fs::path(g_get_user_config_dir())/"xdg-desktop-portal-vtm/config.toml");
    auto explicit_path=environment("VTM_PORTAL_CONFIG");
    if (!explicit_path.empty()) merge_config(c,explicit_path,true);
    auto session=environment("VTM_DESKTOP_PIN");
    if (!session.empty()) c.session=session;
    return c;
}
std::vector<std::string> executable(std::vector<std::string> argv) {
    if (argv.empty() || argv.front().empty()) throw failure("Empty command");
    for (const auto& arg:argv) if (arg.find('\0')!=std::string::npos) throw failure("NUL in command argument");
    auto& program=argv.front();
    if (program.find('/')!=std::string::npos) {
        auto absolute=fs::absolute(program);
        if (access(absolute.c_str(),X_OK) || !fs::is_regular_file(absolute)) throw failure("Executable not found: "+program);
        program=absolute.string();
        return argv;
    }
    std::string search=(fs::path(g_get_home_dir())/".local/bin").string()+":/usr/local/bin:"+environment("PATH");
    gchar** parts=g_strsplit(search.c_str(),":",-1);
    for (unsigned i=0;parts[i];++i) {
        auto candidate=fs::absolute(fs::path(*parts[i]?parts[i]:".")/program);
        if (!access(candidate.c_str(),X_OK) && fs::is_regular_file(candidate)) {
            program=candidate.string(); g_strfreev(parts); return argv;
        }
    }
    g_strfreev(parts); throw failure("Executable not found: "+program);
}
std::string helper_path() {
    auto override=environment("VTM_PORTAL_HELPER");
    if (!override.empty()) return executable({override}).front();
    // Source-tree binaries are next to each other; installed paths come from CMake.
    GError* error=nullptr;
    gchar* self=g_file_read_link("/proc/self/exe",&error);
    if (self) {
        fs::path sibling=fs::path(self).parent_path()/"vtm-portal-helper";
        g_free(self);
        if (!access(sibling.c_str(),X_OK) && fs::is_regular_file(sibling)) return sibling.string();
    }
    if (error) g_error_free(error);
    return executable({PORTAL_HELPER}).front();
}
std::string lua_string(const std::string& value) {
    std::string result="\"";
    for (unsigned char c:value) {
        char bytes[5]; g_snprintf(bytes,sizeof(bytes),"\\%03u",static_cast<unsigned>(c)); result+=bytes;
    }
    return result+'"';
}
std::string encode_argv(const std::vector<std::string>& args) {
    if (args.empty()) throw failure("Empty argument vector");
    std::vector<const char*> values;
    for (const auto& arg:args) {
        if (arg.find('\0')!=std::string::npos || !g_utf8_validate(arg.data(),static_cast<gssize>(arg.size()),nullptr))
            throw failure("Arguments must be valid UTF-8 without NUL");
        values.push_back(arg.c_str());
    }
    auto value=own(g_variant_new_strv(values.data(),static_cast<gssize>(values.size())));
    gchar* encoded=g_base64_encode(static_cast<const guchar*>(g_variant_get_data(value.get())),g_variant_get_size(value.get()));
    std::string result(encoded); g_free(encoded); return result;
}
std::vector<std::string> decode_argv(const std::string& payload) {
    auto padding=payload.find('=');
    auto head=padding==std::string::npos?payload.size():padding;
    if (payload.empty() || payload.size()>128*1024 || payload.size()%4 || payload.size()-head>2 ||
        !std::all_of(payload.begin(),payload.begin()+static_cast<std::ptrdiff_t>(head),[](unsigned char c){
            return g_ascii_isalnum(c) || c=='+' || c=='/';
        }) || !std::all_of(payload.begin()+static_cast<std::ptrdiff_t>(head),payload.end(),[](char c){return c=='=';}))
        throw failure("Invalid encoded argument vector");
    gsize length=0; guchar* decoded=g_base64_decode(payload.c_str(),&length);
    gchar* canonical=g_base64_encode(decoded,length);
    bool valid=payload==canonical; g_free(canonical);
    if (!valid) { g_free(decoded); throw failure("Invalid base64 encoding"); }
    auto value=own(g_variant_new_from_data(G_VARIANT_TYPE("as"),decoded,length,FALSE,g_free,decoded));
    if (!g_variant_is_normal_form(value.get()) || !g_variant_n_children(value.get())) throw failure("Invalid argument vector");
    std::vector<std::string> argv;
    GVariantIter iter; const char* text;
    g_variant_iter_init(&iter,value.get());
    while (g_variant_iter_next(&iter,"&s",&text)) argv.emplace_back(text);
    if (!fs::path(argv.front()).is_absolute()) throw failure("Encoded executable must be an absolute path");
    return argv;
}
void exec_command(const std::vector<std::string>& argv) {
    std::vector<char*> args;
    for (const auto& value:argv) args.push_back(const_cast<char*>(value.c_str()));
    args.push_back(nullptr);
    signal(SIGPIPE,SIG_DFL);
    execv(args.front(),args.data());
    throw failure("Cannot execute "+argv.front()+": "+std::strerror(errno));
}
static std::string vtm_quote(const std::string& path) {
    std::string result="\"";
    for (char c:path) { if (c=='"' || c=='\\') result+='\\'; result+=c; }
    return result+'"';
}
void launch(std::vector<std::string> argv,const std::string& item,const std::string& title,
            const std::string& size,const Config& config) {
    auto payload=encode_argv(executable(std::move(argv)));
    auto cmd=vtm_quote(helper_path())+" exec "+payload;
    std::string script="vtm.desktop.Run({id="+lua_string(item)+",type=\"term\",title="+lua_string(title)+
        ",cmd="+lua_string(cmd)+",winsize="+lua_string(size)+",hidden=\"true\"});\n";
    if (script.size()>512*1024) throw failure("Command exceeds VTM monitor limit");
    auto monitor=executable(config.vtm_command);
    if (!config.session.empty()) { monitor.emplace_back("-p"); monitor.push_back(config.session); }
    monitor.emplace_back("-m");
    std::vector<char*> args;
    for (auto& value:monitor) args.push_back(value.data());
    args.push_back(nullptr);
    int master=posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC);
    if (master<0) throw failure("Cannot allocate a PTY");
    int slave=-1, pipes[2]={-1,-1}; pid_t pid=-1;
    auto cleanup=[&] {
        if (pipes[0]>=0) close(pipes[0]);
        if (pipes[1]>=0) close(pipes[1]);
        if (slave>=0) close(slave);
        close(master);
        if (pid>0) {
            kill(pid,SIGTERM);
            auto end=g_get_monotonic_time()+2*G_USEC_PER_SEC;
            int status;
            while (waitpid(pid,&status,WNOHANG)==0) {
                if (g_get_monotonic_time()>end) { kill(pid,SIGKILL); while(waitpid(pid,&status,0)<0 && errno==EINTR){} break; }
                g_usleep(10000);
            }
        }
    };
    try {
        if (grantpt(master) || unlockpt(master)) throw failure("Cannot initialize a PTY");
        auto* slave_name=ptsname(master);
        if (!slave_name) throw failure("Cannot obtain PTY name");
        slave=open(slave_name,O_RDWR|O_NOCTTY|O_CLOEXEC);
        if (slave<0 || pipe(pipes)) throw failure("Cannot create monitor descriptors");
        fcntl(pipes[0],F_SETFD,FD_CLOEXEC); fcntl(pipes[1],F_SETFD,FD_CLOEXEC);
        pid=fork();
        if (pid<0) throw failure("Cannot fork VTM monitor");
        if (pid==0) {
            setsid();
            if (dup2(pipes[0],STDIN_FILENO)<0 || dup2(slave,STDOUT_FILENO)<0 || dup2(slave,STDERR_FILENO)<0) _exit(127);
            close(master); close(slave); close(pipes[0]); close(pipes[1]);
            execv(args.front(),args.data()); _exit(127);
        }
        close(slave); slave=-1; close(pipes[0]); pipes[0]=-1;
        fcntl(master,F_SETFL,fcntl(master,F_GETFL)|O_NONBLOCK);
        fcntl(pipes[1],F_SETFL,fcntl(pipes[1],F_GETFL)|O_NONBLOCK);
        std::size_t sent=0; std::string output;
        auto deadline=g_get_monotonic_time()+static_cast<gint64>(config.launch_timeout*G_USEC_PER_SEC);
        for (;;) {
            pollfd descriptors[]={{master,POLLIN,0},{pipes[1],POLLOUT,0}};
            int rc=poll(descriptors,2,50);
            if (rc<0 && errno!=EINTR) throw failure("VTM monitor poll failed");
            if (descriptors[0].revents&(POLLIN|POLLHUP)) {
                char data[8192]; ssize_t n;
                while ((n=read(master,data,sizeof(data)))>0) output.append(data,static_cast<std::size_t>(n));
                if (output.size()>65536) output.erase(0,output.size()-65536);
            }
            if (pipes[1]>=0 && descriptors[1].revents&POLLOUT) {
                ssize_t n=write(pipes[1],script.data()+sent,script.size()-sent);
                if (n>0) sent+=static_cast<std::size_t>(n);
                else if (errno!=EINTR && errno!=EAGAIN) throw failure("VTM monitor input closed");
                if (sent==script.size()) { close(pipes[1]); pipes[1]=-1; }
            }
            int status=0;
            auto waited=waitpid(pid,&status,WNOHANG);
            if (waited==pid) {
                pid=-1;
                if (!WIFEXITED(status) || WEXITSTATUS(status)!=0 || sent!=script.size())
                    throw failure(output.empty()?"Cannot connect to the running VTM session":output);
                break;
            }
            if (waited<0 && errno!=EINTR) throw failure("Cannot wait for VTM monitor");
            if (g_get_monotonic_time()>deadline) throw failure("VTM monitor timed out; check the session name");
        }
        cleanup();
    } catch (...) { cleanup(); throw; }
}
std::string normalize_uri(const std::string& input,const Config& config) {
    if (controls(input)) throw failure("URI contains control characters");
    std::string value=input;
    gchar* scheme=g_uri_parse_scheme(value.c_str());
    if (!scheme) {
        fs::path path=value;
        if (value=="~" || value.rfind("~/",0)==0) path=fs::path(g_get_home_dir())/(value=="~"?"":value.substr(2));
        auto absolute=fs::absolute(path);
        gchar* uri=g_filename_to_uri(absolute.c_str(),nullptr,nullptr);
        if (!uri) throw failure("Cannot make file URI");
        value=uri; g_free(uri); scheme=g_strdup("file");
    }
    std::string name=scheme; g_free(scheme);
    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return g_ascii_tolower(c);});
    if (std::find(config.schemes.begin(),config.schemes.end(),name)==config.schemes.end())
        throw failure("Unsupported URI scheme: "+name);
    GError* error=nullptr;
    GUri* parsed=g_uri_parse(value.c_str(),G_URI_FLAGS_PARSE_RELAXED,&error);
    if (!parsed) { std::string reason=error->message; g_error_free(error); throw failure(reason); }
    std::unique_ptr<GUri,decltype(&g_uri_unref)> uri(parsed,g_uri_unref);
    const char* host=g_uri_get_host(parsed);
    if ((name=="http" || name=="https") && (!host || !*host)) throw failure("Web URL must include a hostname");
    if (name=="file" && host && *host && strcmp(host,"localhost")) throw failure("Only local file:// URLs are supported");
    return value;
}
Variant parse_variant(const std::string& text,const char* type) {
    GError* error=nullptr;
    auto* value=g_variant_parse(G_VARIANT_TYPE(type),text.data(),text.data()+text.size(),nullptr,&error);
    if (!value) { std::string reason=error->message; g_error_free(error); throw failure("Invalid request: "+reason); }
    return own(value);
}
std::string variant_string(GVariant* value) {
    gchar* text=g_variant_print(value,TRUE); std::string result(text); g_free(text); return result;
}
Variant option(GVariant* options,const char* key,const char* type) {
    auto result=own(g_variant_lookup_value(options,key,nullptr));
    if (result && !g_variant_is_of_type(result.get(),G_VARIANT_TYPE(type))) throw failure(std::string("Wrong type for ")+key);
    return result;
}
std::string string_option(GVariant* options,const char* key,const std::string& fallback) {
    auto v=option(options,key,"s"); return v ? g_variant_get_string(v.get(),nullptr) : fallback;
}
bool bool_option(GVariant* options,const char* key,bool fallback) {
    auto v=option(options,key,"b"); return v ? g_variant_get_boolean(v.get()) : fallback;
}
std::string byte_path(GVariant* value) {
    gsize size=0; const char* bytes=static_cast<const char*>(g_variant_get_fixed_array(value,&size,1));
    if (!bytes || !size) return "";
    while (size && bytes[size-1]=='\0') --size;
    if (memchr(bytes,'\0',size)) throw failure("Embedded NUL in file path");
    return std::string(bytes,size);
}
Request read_request(const fs::path& job) {
    auto value=parse_variant(read_file(job/"request.variant"),"(ssssa{sv})");
    const char *mode,*app,*parent,*title; GVariant* options;
    g_variant_get(value.get(),"(&s&s&s&s@a{sv})",&mode,&app,&parent,&title,&options);
    return {mode,title,own(options)};
}
void write_response(const fs::path& job,unsigned response,GVariant* results) {
    GVariant* dictionary=results?g_variant_ref(results):g_variant_new_array(G_VARIANT_TYPE("{sv}"),nullptr,0);
    auto value=own(g_variant_new("(u@a{sv})",response,dictionary));
    write_file(job/"result.tmp",variant_string(value.get()));
    fs::rename(job/"result.tmp",job/"result.variant");
}
}
