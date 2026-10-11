// SPDX-License-Identifier: MIT
#include "common.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fnmatch.h>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <regex>
#include <set>
#include <signal.h>
#include <stdexcept>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace vtmportal {
namespace {
struct Cancelled {};
volatile sig_atomic_t interrupted=0;
void signal_cancel(int) { interrupted=1; }
std::string display(const std::string& value) {
    std::string result;
    for (unsigned char c:value) if (c>=32 && c!=127) result+=static_cast<char>(c);
    return result;
}
std::string trim(std::string value) {
    auto start=value.find_first_not_of(" \t\r\n");
    return start==std::string::npos?"":value.substr(start,value.find_last_not_of(" \t\r\n")-start+1);
}
struct Interaction {
    fs::path job;
    termios settings{};
    bool tty=false;
    pid_t child=-1;
    std::string input_buffer;
    explicit Interaction(fs::path path):job(std::move(path)) {
        tty=tcgetattr(STDIN_FILENO,&settings)==0;
        interrupted=0;
        struct sigaction action{}; action.sa_handler=signal_cancel; sigemptyset(&action.sa_mask);
        for (int signum:{SIGTERM,SIGHUP,SIGINT}) sigaction(signum,&action,nullptr);
    }
    void check() const { if (interrupted || fs::exists(job/"cancel")) throw Cancelled{}; }
    void restore() {
        std::cout<<"\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1005l"
                   "\x1b[?1006l\x1b[?1015l\x1b[?2004l"<<std::flush;
        if (tty) { tcsetattr(STDIN_FILENO,TCSANOW,&settings); tcflush(STDIN_FILENO,TCIFLUSH); }
    }
    void stop_child() noexcept {
        if (child<0) return;
        kill(child,SIGTERM);
        auto deadline=g_get_monotonic_time()+3*G_USEC_PER_SEC;
        int status=0;
        while (waitpid(child,&status,WNOHANG)==0) {
            if (g_get_monotonic_time()>deadline) {
                kill(child,SIGKILL); while(waitpid(child,&status,0)<0 && errno==EINTR){} break;
            }
            g_usleep(10000);
        }
        child=-1;
    }
    std::string answer(const std::string& prompt) {
        std::cout<<prompt<<std::flush;
        for (;;) {
            check();
            auto newline=input_buffer.find('\n');
            if (newline!=std::string::npos) {
                std::string result=input_buffer.substr(0,newline); input_buffer.erase(0,newline+1);
                result=std::regex_replace(result,std::regex("\x1b\\[[0-?]*[ -/]*[@-~]"),"");
                if (display(result)!=result && !(result.size() && result.back()=='\r'))
                    throw std::runtime_error("Input contains control characters");
                result=trim(result);
                if (display(result)!=result) throw std::runtime_error("Input contains control characters");
                return result;
            }
            if (input_buffer.size()>65536) throw std::runtime_error("Input is too long");
            pollfd fd{STDIN_FILENO,POLLIN,0};
            int rc=poll(&fd,1,100);
            if (rc<0) { if (errno==EINTR) continue; throw std::runtime_error("Cannot read prompt input"); }
            if (!rc) continue;
            if (fd.revents&(POLLIN|POLLHUP)) {
                char text[1024]; ssize_t count=read(STDIN_FILENO,text,sizeof(text));
                if (count>0) input_buffer.append(text,static_cast<std::size_t>(count));
                else if (count==0) throw Cancelled{};
                else if (errno!=EINTR && errno!=EAGAIN) throw std::runtime_error("Cannot read prompt input");
            } else if (fd.revents&(POLLERR|POLLNVAL)) throw Cancelled{};
        }
    }
    int run_elio(std::vector<std::string> argv) {
        check(); argv=executable(std::move(argv));
        std::vector<char*> args;
        for (auto& value:argv) args.push_back(value.data());
        args.push_back(nullptr);
        GError* error=nullptr; GPid process=0;
        if (!g_spawn_async(nullptr,args.data(),nullptr,
                           static_cast<GSpawnFlags>(G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_CHILD_INHERITS_STDIN),
                            nullptr,nullptr,&process,&error)) {
            std::string message=error->message; g_error_free(error); throw std::runtime_error(message);
        }
        child=process;
        for (;;) {
            check();
            int status=0; auto result=waitpid(child,&status,WNOHANG);
            if (result==child) { child=-1; g_spawn_close_pid(process); return WIFEXITED(status)?WEXITSTATUS(status):1; }
            if (result<0 && errno!=EINTR) throw std::runtime_error("Cannot wait for Elio");
            poll(nullptr,0,50);
        }
    }
    ~Interaction() { stop_child(); restore(); }
};
std::size_t choose_index(Interaction& ui,std::size_t count,std::size_t initial) {
    for (;;) {
        auto value=ui.answer("Choose ["+std::to_string(initial+1)+"]: ");
        if (value.empty()) return initial;
        try {
            std::size_t used=0; auto index=std::stoul(value,&used);
            if (used==value.size() && index>=1 && index<=count) return index-1;
        } catch (const std::exception&) {}
    }
}
Variant choose_extra(Interaction& ui,GVariant* options) {
    GVariantBuilder builder; g_variant_builder_init(&builder,G_VARIANT_TYPE_VARDICT);
    auto filters=option(options,"filters","a(sa(us))");
    auto selected=option(options,"current_filter","(sa(us))");
    auto count=filters?g_variant_n_children(filters.get()):0;
    if (!selected && count) selected=own(g_variant_get_child_value(filters.get(),0));
    if (count>1) {
        std::size_t initial=0;
        std::cout<<"File type:\n";
        for (std::size_t i=0;i<count;++i) {
            auto item=own(g_variant_get_child_value(filters.get(),i));
            const char* label; GVariant* rules;
            g_variant_get(item.get(),"(&s@a(us))",&label,&rules); g_variant_unref(rules);
            std::cout<<"  "<<i+1<<". "<<display(label)<<"\n";
            if (g_variant_equal(selected.get(),item.get())) initial=i;
        }
        selected=own(g_variant_get_child_value(filters.get(),choose_index(ui,count,initial)));
    }
    if (selected) g_variant_builder_add(&builder,"{sv}","current_filter",selected.get());
    auto choices=option(options,"choices","a(ssa(ss)s)");
    if (choices && g_variant_n_children(choices.get())) {
        GVariantBuilder chosen; g_variant_builder_init(&chosen,G_VARIANT_TYPE("a(ss)"));
        for (std::size_t i=0;i<g_variant_n_children(choices.get());++i) {
            auto item=own(g_variant_get_child_value(choices.get(),i));
            const char *id,*label,*initial; GVariant* values;
            g_variant_get(item.get(),"(&s&s@a(ss)&s)",&id,&label,&values,&initial);
            auto values_ref=own(values); std::string result;
            auto n=g_variant_n_children(values);
            if (!n) {
                std::string default_value=std::string(initial)=="true"?"true":"false";
                for (;;) {
                    auto value=ui.answer(display(label)+" [yes/no; "+default_value+"]: ");
                    std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return g_ascii_tolower(c);});
                    if (value.empty()) { result=default_value; break; }
                    if (value=="yes" || value=="y" || value=="true" || value=="1") { result="true"; break; }
                    if (value=="no" || value=="n" || value=="false" || value=="0") { result="false"; break; }
                }
            } else {
                std::cout<<display(label)<<"\n"; std::size_t preferred=0;
                for (std::size_t j=0;j<n;++j) {
                    const char *key,*text;
                    g_variant_get_child(values,j,"(&s&s)",&key,&text);
                    std::cout<<"  "<<j+1<<". "<<display(text)<<"\n";
                    if (!strcmp(key,initial)) preferred=j;
                }
                const char *key,*text;
                g_variant_get_child(values,choose_index(ui,n,preferred),"(&s&s)",&key,&text); result=key;
            }
            g_variant_builder_add(&chosen,"(ss)",id,result.c_str());
        }
        g_variant_builder_add(&builder,"{sv}","choices",g_variant_builder_end(&chosen));
    }
    return own(g_variant_builder_end(&builder));
}
std::vector<fs::path> read_selection(const fs::path& output) {
    auto text=read_file(output); std::vector<fs::path> result;
    std::size_t start=0;
    while (start<text.size()) {
        auto end=text.find('\n',start); if (end==std::string::npos) end=text.size();
        auto line=text.substr(start,end-start);
        if (line.size() && line.back()=='\r') line.pop_back();
        if (!line.empty()) {
            if (line.find('\0')!=std::string::npos) throw std::runtime_error("Elio returned a path containing NUL");
            result.emplace_back(line);
        }
        start=end+1;
    }
    return result;
}
}
bool filter_matches(const fs::path& path,GVariant* filter) {
    if (!filter) return true;
    const char* name; GVariant* array;
    g_variant_get(filter,"(&s@a(us))",&name,&array);
    auto rules=own(array); GVariantIter iter; guint32 kind; const char* pattern;
    g_variant_iter_init(&iter,array);
    while (g_variant_iter_next(&iter,"(u&s)",&kind,&pattern)) {
        if (kind==0 && fnmatch(pattern,path.filename().c_str(),0)==0) return true;
        if (kind==1) {
            char data[8192]; std::ifstream file(path,std::ios::binary);
            file.read(data,sizeof(data)); auto length=file.gcount(); gboolean uncertain=FALSE;
            gchar* type=g_content_type_guess(path.c_str(),reinterpret_cast<const guchar*>(data),static_cast<gsize>(length),&uncertain);
            gchar* mime=g_content_type_get_mime_type(type);
            bool matches=(mime && !fnmatch(pattern,mime,0)) || g_content_type_is_a(type,pattern);
            g_free(type); g_free(mime); if (matches) return true;
        }
    }
    return false;
}
void valid_name(const std::string& name) {
    if (name.empty() || fs::path(name).filename()!=name || name=="." || name==".." || display(name)!=name)
        throw std::runtime_error("Enter a filename without a directory or control characters");
}
Variant translate(const std::vector<fs::path>& input,GVariant* options,
                  const std::string& mode,const std::string& name,GVariant* extra) {
    if (input.empty()) throw Cancelled{};
    std::vector<fs::path> paths;
    for (const auto& path:input) paths.push_back(fs::absolute(path));
    if (mode=="save" || mode=="savefiles") {
        auto folder=paths.front();
        if (!fs::is_directory(folder)) throw std::runtime_error("Choose a destination directory");
        paths.clear();
        if (mode=="save") { valid_name(name); paths.push_back(folder/name); }
        else {
            auto files=option(options,"files","aay");
            if (!files || !g_variant_n_children(files.get())) throw std::runtime_error("SaveFiles requires at least one filename");
            std::set<fs::path> reserved;
            for (std::size_t i=0;i<g_variant_n_children(files.get());++i) {
                auto v=own(g_variant_get_child_value(files.get(),i)); auto filename=byte_path(v.get()); valid_name(filename);
                fs::path original=filename, target=folder/original; unsigned counter=1;
                while (fs::exists(target) || reserved.count(target))
                    target=folder/(original.stem().string()+" ("+std::to_string(counter++)+")"+original.extension().string());
                reserved.insert(target); paths.push_back(target);
            }
        }
    } else {
        bool directory=bool_option(options,"directory");
        for (const auto& path:paths)
            if (!fs::exists(path) || fs::is_directory(path)!=directory)
                throw std::runtime_error(directory?"Choose an existing directory":"Choose an existing file");
        if (!bool_option(options,"multiple")) paths.resize(1);
        auto filter=extra?option(extra,"current_filter","(sa(us))"):Variant{};
        if (!directory) for (const auto& path:paths)
            if (!filter_matches(path,filter.get())) throw std::runtime_error("Selected files do not match the selected file type");
    }
    GVariantBuilder results, uris;
    g_variant_builder_init(&results,G_VARIANT_TYPE_VARDICT); g_variant_builder_init(&uris,G_VARIANT_TYPE("as"));
    for (const auto& path:paths) {
        GError* error=nullptr; gchar* uri=g_filename_to_uri(path.c_str(),nullptr,&error);
        if (!uri) {
            std::string reason=error->message; g_error_free(error);
            g_variant_builder_clear(&results); g_variant_builder_clear(&uris); throw std::runtime_error(reason);
        }
        g_variant_builder_add(&uris,"s",uri); g_free(uri);
    }
    g_variant_builder_add(&results,"{sv}","uris",g_variant_builder_end(&uris));
    if (extra) {
        GVariantIter iter; const char* key; GVariant* value;
        g_variant_iter_init(&iter,extra);
        while (g_variant_iter_next(&iter,"{&sv}",&key,&value)) {
            g_variant_builder_add(&results,"{sv}",key,value); g_variant_unref(value);
        }
    }
    return own(g_variant_builder_end(&results));
}
int run_picker(const fs::path& job) {
    unsigned response=1; Variant results;
    {
        Interaction ui(job);
        try {
            write_file(job/"started",std::to_string(getpid())); ui.check();
            auto request=read_request(job); auto config=configuration();
            auto extra=choose_extra(ui,request.options.get());
            auto current_file=option(request.options.get(),"current_file","ay");
            auto current_folder=option(request.options.get(),"current_folder","ay");
            std::string initial=current_file?byte_path(current_file.get()):"";
            if (initial.empty() && current_folder) initial=byte_path(current_folder.get());
            fs::path start=initial.empty()?fs::path(g_get_home_dir()):fs::absolute(initial);
            while (!fs::exists(start) && start.parent_path()!=start) start=start.parent_path();
            for (;;) {
                auto output=job/"selection.txt", cwd=job/"cwd.txt";
                fs::remove(output); fs::remove(cwd);
                auto args=config.picker_command;
                for (const auto& arg:std::vector<std::string>{"--chooser-file",output.string(),"--cwd-file",cwd.string(),start.string()}) args.push_back(arg);
                int code=ui.run_elio(args); ui.restore(); ui.check();
                if (code || !fs::exists(output)) throw Cancelled{};
                auto paths=read_selection(output);
                if (request.mode=="save" || request.mode=="savefiles" ||
                    (bool_option(request.options.get(),"directory") && paths.empty())) {
                    if (paths.empty() || !fs::is_directory(paths.front())) {
                        if (!fs::exists(cwd)) throw std::runtime_error("Elio did not return a destination directory");
                        auto folder=read_file(cwd); while (folder.size() && folder.back()=='\n') folder.pop_back();
                        if (folder.empty() || folder.find('\0')!=std::string::npos) throw std::runtime_error("Invalid Elio destination directory");
                        paths={fs::path(folder)};
                    }
                }
                std::string filename;
                if (request.mode=="save") {
                    auto suggested=string_option(request.options.get(),"current_name",fs::path(initial).filename().string());
                    std::cout<<"\x1b[2J\x1b[HSave file\nDirectory: "<<display(paths.front().string())<<"\n";
                    for (;;) {
                        try {
                            filename=ui.answer("Filename ["+display(suggested)+"]: ");
                            if (filename.empty()) filename=suggested;
                            valid_name(filename); break;
                        } catch (const std::runtime_error& error) { std::cout<<error.what()<<"\n"; }
                    }
                }
                try {
                    results=translate(paths,request.options.get(),request.mode,filename,extra.get());
                    if (request.mode=="save" && fs::exists(paths.front()/filename)) {
                        auto replace=ui.answer("File exists. Replace? [y/N]: ");
                        std::transform(replace.begin(),replace.end(),replace.begin(),[](unsigned char c){return g_ascii_tolower(c);});
                        if (replace!="y" && replace!="yes") { start=paths.front(); results.reset(); continue; }
                    }
                    response=0; break;
                } catch (const std::runtime_error& error) {
                    std::cout<<error.what()<<"\n"; ui.answer("Enter to choose again; Ctrl+C to cancel: ");
                    if (!paths.empty() && fs::is_directory(paths.front())) start=paths.front();
                }
            }
        } catch (const Cancelled&) { response=1; results.reset(); }
          catch (const std::exception& error) { std::cerr<<"vtm-picker: "<<error.what()<<"\n"; response=2; results.reset(); }
        // Disable cancellation while restoring the terminal and publishing a result.
        for (int signum:{SIGTERM,SIGHUP,SIGINT}) signal(signum,SIG_IGN);
    }
    if (fs::exists(job)) {
        write_response(job,response,results.get());
        if (fs::exists(job/"orphan")) {
            auto parent=job.parent_path(); fs::remove_all(job); std::error_code ignored; fs::remove(parent,ignored);
        }
    }
    return 0;
}
}
