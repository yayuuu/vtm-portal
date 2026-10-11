// SPDX-License-Identifier: MIT
#include "common.hpp"
#include "paths.h"
#include <iostream>
#include <signal.h>
#include <stdexcept>

using namespace vtmportal;
int main(int argc,char** argv) {
    signal(SIGPIPE,SIG_IGN);
    try {
        std::string executable_name=fs::path(argv[0]).filename().string();
        std::string mode;
        int start=1;
        if (executable_name=="vtm-open") mode="open";
        else if (executable_name=="vtm-run") mode="run";
        else {
            if (argc<2) throw std::runtime_error("Usage: vtm-portal-helper open|run|launch-picker|picker|exec [ARGUMENTS]");
            mode=argv[start++];
        }
        if (mode=="--version") { std::cout<<"vtm-portal-helper "<<PORTAL_VERSION<<"\n"; return 0; }
        if (start<argc && std::string(argv[start])=="--") ++start;
        std::vector<std::string> args;
        for (int i=start;i<argc;++i) args.emplace_back(argv[i]);
        if (mode=="exec") {
            if (args.size()!=1) throw std::runtime_error("exec requires one encoded argument vector");
            exec_command(decode_argv(args.front()));
        }
        if (mode=="picker") {
            if (args.size()!=1) throw std::runtime_error("picker requires a request directory");
            return run_picker(fs::absolute(args.front()));
        }
        auto config=configuration();
        if (mode=="open") {
            if (args.empty()) throw std::runtime_error("Usage: vtm-open URI [URI ...]");
            for (auto& uri:args) uri=normalize_uri(uri,config);
            for (const auto& uri:args) {
                auto browser=config.browser_command; browser.push_back(uri);
                launch(browser,"vtm-browser",config.browser_title,config.browser_size,config);
            }
        } else if (mode=="run") {
            if (args.empty()) throw std::runtime_error("Usage: vtm-run -- PROGRAM [ARGUMENT ...]");
            launch(args,"vtm-terminal",fs::path(args.front()).filename().string(),config.terminal_size,config);
        } else if (mode=="launch-picker") {
            if (args.size()!=1) throw std::runtime_error("launch-picker requires a request directory");
            auto job=fs::absolute(args.front());
            if (fs::exists(job/"cancel")) {
                if (fs::exists(job/"orphan")) { std::error_code ignored; fs::remove_all(job,ignored); fs::remove(job.parent_path(),ignored); }
                return 0;
            }
            auto request=read_request(job);
            write_file(job/"start-timeout",std::to_string(config.start_timeout));
            gchar* uuid=g_uuid_string_random(); std::string item="vtm-picker-"+std::string(uuid); g_free(uuid);
            launch({helper_path(),"picker",job.string()},item,request.title.empty()?"Choose a file":request.title,config.picker_size,config);
        } else throw std::runtime_error("Unknown helper mode: "+mode);
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<"vtm-portal: "<<error.what()<<"\n"; return 1;
    }
}
